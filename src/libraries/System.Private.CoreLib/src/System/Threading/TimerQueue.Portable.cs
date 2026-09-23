// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

using System.Collections.Generic;
using System.Diagnostics;

namespace System.Threading
{
    //
    // Portable implementation of Timer
    //
    internal sealed partial class TimerQueue : IThreadPoolWorkItem
    {
        private static List<TimerQueue>? s_scheduledTimers;
        private static List<TimerQueue>? s_scheduledTimersToFire;

        /// <summary>
        /// This event is used by the timer thread to wait for timer expiration. It is also
        /// used to notify the timer thread that a new timer has been set.
        /// </summary>
        private static readonly AutoResetEvent s_timerEvent = new AutoResetEvent(false);

        private static readonly Lock s_timerEventLock = new Lock();

#if NATIVEAOT && TARGET_UNIX
        /// <summary>
        /// Retains the scheduling thread independently of the persistent timer lists.
        /// </summary>
        private static Thread? s_forkTimerThread;
#endif

        // this means that it's in the s_scheduledTimers collection, not that it's the one which would run on the next TimeoutCallback
        private bool _isScheduled;
        private long _scheduledDueTimeMs;

        private static List<TimerQueue> InitializeScheduledTimerManager_Locked()
        {
            Debug.Assert(s_scheduledTimers == null);

            var timers = new List<TimerQueue>(Instances.Length);
            s_scheduledTimersToFire ??= new List<TimerQueue>(Instances.Length);

#if NATIVEAOT && TARGET_UNIX
            ForkThreadServices.RegisterTimer();
            StartTimerThreadForFork_Locked();
#else

            // The timer thread must start in the default execution context without transferring the context, so
            // using UnsafeStart() instead of Start()
            Thread timerThread = new Thread(TimerThread)
            {
                Name = ".NET Timer",
                IsBackground = true
            };
            timerThread.UnsafeStart();
#endif

            // Do this after creating the thread in case thread creation fails so that it will try again next time
            s_scheduledTimers = timers;
            return timers;
        }

#if NATIVEAOT && TARGET_UNIX
        /// <summary>
        /// Starts a scheduling thread while preserving the manager's existing collections.
        /// </summary>
        private static void StartTimerThreadForFork_Locked()
        {
            if (s_forkTimerThread != null)
            {
                return;
            }

            var timerThread = new Thread(TimerThread)
            {
                Name = ".NET Timer",
                IsBackground = true
            };

            if (ForkThreadServices.StartThread(timerThread))
            {
                s_forkTimerThread = timerThread;
            }
        }

        /// <summary>
        /// Wakes the scheduling thread so it can stop outside its list and event locks.
        /// </summary>
        internal static void RequestForkRetirement() => s_timerEvent.Set();

        /// <summary>
        /// Releases the stopped scheduling thread without changing registrations or deadlines.
        /// </summary>
        /// <returns>Whether its last batch was completely published before retirement.</returns>
        internal static bool CompleteForkRetirement()
        {
            lock (s_timerEventLock)
            {
                if (s_scheduledTimersToFire?.Count != 0)
                {
                    return false;
                }

                s_forkTimerThread = null;
                s_timerEvent.Reset();
                return true;
            }
        }

        /// <summary>
        /// Restarts the scheduler and forces its first scan of retained absolute deadlines.
        /// </summary>
        internal static void ResumeAfterFork()
        {
            lock (s_timerEventLock)
            {
                if (s_scheduledTimers != null)
                {
                    StartTimerThreadForFork_Locked();
                }
            }

            s_timerEvent.Set();
        }
#endif

        private bool SetTimerPortable(uint actualDuration)
        {
            Debug.Assert((int)actualDuration >= 0);
            long dueTimeMs = TickCount64 + (int)actualDuration;
            AutoResetEvent timerEvent = s_timerEvent;
            Lock timerEventLock = s_timerEventLock;

            lock (timerEventLock)
            {
                if (!_isScheduled)
                {
                    List<TimerQueue> timers = s_scheduledTimers ?? InitializeScheduledTimerManager_Locked();

                    timers.Add(this);
                    _isScheduled = true;
                }

                _scheduledDueTimeMs = dueTimeMs;
            }

            timerEvent.Set();
            return true;
        }

        /// <summary>
        /// This method is executed on a dedicated timer thread. Its purpose is
        /// to handle timer requests and notify the TimerQueue when a timer expires.
        /// </summary>
        private static void TimerThread()
        {
#if NATIVEAOT && TARGET_UNIX
            try
            {
                TimerThreadCore();
            }
            finally
            {
                ForkThreadServices.OnThreadExit();
            }
#else
            TimerThreadCore();
#endif
        }

        /// <summary>
        /// Publishes each due timer batch before observing a framework retirement request.
        /// </summary>
        private static void TimerThreadCore()
        {
            AutoResetEvent timerEvent = s_timerEvent;
            Lock timerEventLock = s_timerEventLock;
            List<TimerQueue> timersToFire = s_scheduledTimersToFire!;
            List<TimerQueue> timers;

            lock (timerEventLock)
            {
                timers = s_scheduledTimers!;
            }

            int shortestWaitDurationMs = Timeout.Infinite;
            while (true)
            {
#if NATIVEAOT && TARGET_UNIX
                if (ForkThreadServices.IsPreparing)
                {
                    return;
                }
#endif
                timerEvent.WaitOne(shortestWaitDurationMs);

#if NATIVEAOT && TARGET_UNIX
                if (ForkThreadServices.IsPreparing)
                {
                    return;
                }
#endif

                long currentTimeMs = TickCount64;
                shortestWaitDurationMs = int.MaxValue;
                lock (timerEventLock)
                {
                    for (int i = timers.Count - 1; i >= 0; --i)
                    {
                        TimerQueue timer = timers[i];
                        long waitDurationMs = timer._scheduledDueTimeMs - currentTimeMs;
                        if (waitDurationMs <= 0)
                        {
                            timer._isScheduled = false;
                            timersToFire.Add(timer);

                            int lastIndex = timers.Count - 1;
                            if (i != lastIndex)
                            {
                                timers[i] = timers[lastIndex];
                            }
                            timers.RemoveAt(lastIndex);
                            continue;
                        }

                        if (waitDurationMs < shortestWaitDurationMs)
                        {
                            shortestWaitDurationMs = (int)waitDurationMs;
                        }
                    }
                }

                if (timersToFire.Count > 0)
                {
                    foreach (TimerQueue timerToFire in timersToFire)
                    {
                        ThreadPool.UnsafeQueueHighPriorityWorkItemInternal(timerToFire);
                    }
                    timersToFire.Clear();
                }

                if (shortestWaitDurationMs == int.MaxValue)
                {
                    shortestWaitDurationMs = Timeout.Infinite;
                }
            }
        }

        void IThreadPoolWorkItem.Execute() => FireNextTimers();
    }
}
