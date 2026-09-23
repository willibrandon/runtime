// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

using System.Collections.Generic;
using System.Runtime;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace System.Threading
{
    /// <summary>
    /// Retires initialized framework threads at the experimental native fork checkpoint.
    /// </summary>
    internal static class ForkThreadServices
    {
        /// <summary>
        /// Serializes framework thread activation with checkpoint admission.
        /// </summary>
        private static readonly Lock s_activationLock = new Lock(useTrivialWaits: true);

        /// <summary>
        /// Retains the framework threads that have not completed their managed exit path.
        /// </summary>
        private static readonly List<Thread> s_threads = new List<Thread>();

        /// <summary>
        /// Retains the existing pool without initializing an unused pool during recovery.
        /// </summary>
        private static PortableThreadPool? s_pool;

        /// <summary>
        /// Records initialization of the portable timer manager.
        /// </summary>
        private static bool s_timerInitialized;

        /// <summary>
        /// Records successful registration of the four native lifecycle callbacks.
        /// </summary>
        private static bool s_registered;

        /// <summary>
        /// Prevents activation while existing services retire and the native checkpoint runs.
        /// </summary>
        private static int s_preparing;

        /// <summary>
        /// Gets whether framework loops must return through their ordinary cleanup paths.
        /// </summary>
        internal static bool IsPreparing => Volatile.Read(ref s_preparing) != 0;

        /// <summary>
        /// Gets whether workers must stop dispatching after active callbacks have finished.
        /// </summary>
        internal static bool AreWorkersRetiring => IsPreparing && RuntimeImports.RhIsForkWorkRetired() != 0;

        /// <summary>
        /// Registers callbacks before application initialization or finalization can start services.
        /// </summary>
        internal static void Initialize()
        {
            lock (s_activationLock)
            {
                EnsureRegistered();
            }
        }

        /// <summary>
        /// Records the initialized pool and registers callbacks without recreating that pool.
        /// </summary>
        /// <param name="pool">The initialized singleton.</param>
        internal static void RegisterPool(PortableThreadPool pool)
        {
            lock (s_activationLock)
            {
                EnsureRegistered();
                s_pool = pool;
            }
        }

        /// <summary>
        /// Records that portable timer infrastructure has initialized.
        /// </summary>
        internal static void RegisterTimer()
        {
            lock (s_activationLock)
            {
                EnsureRegistered();
                s_timerInitialized = true;
            }
        }

        /// <summary>
        /// Registers stable unmanaged entry points with the owned native runtime.
        /// </summary>
        private static unsafe void EnsureRegistered()
        {
            if (s_registered)
            {
                return;
            }

            int result = RuntimeImports.RhRegisterForkServiceCallbacks(&Prepare, &Parent, &ChildReset, &ChildResume);
            if (result != 1 && result != -7)
            {
                throw new InvalidOperationException("Native fork service registration failed.");
            }

            s_registered = true;
        }

        /// <summary>
        /// Enters the activation gate unless retirement has already begun.
        /// </summary>
        /// <returns>Whether the caller owns the gate and may activate a service.</returns>
        internal static bool TryEnterActivation()
        {
            s_activationLock.Enter();
            if (AreWorkersRetiring)
            {
                s_activationLock.Exit();
                return false;
            }

            return true;
        }

        /// <summary>
        /// Releases a successfully entered activation gate.
        /// </summary>
        internal static void ExitActivation() => s_activationLock.Exit();

        /// <summary>
        /// Starts and records a framework thread while activation is admitted.
        /// </summary>
        /// <param name="thread">The unstarted framework thread.</param>
        /// <returns>Whether the thread was started.</returns>
        internal static bool StartThread(Thread thread)
        {
            if (!TryEnterActivation())
            {
                return false;
            }

            try
            {
                s_threads.Add(thread);
                try
                {
                    thread.UnsafeStart();
                    return true;
                }
                catch
                {
                    s_threads.Remove(thread);
                    throw;
                }
            }
            finally
            {
                ExitActivation();
            }
        }

        /// <summary>
        /// Removes a framework thread after its service loop and managed cleanup finish.
        /// </summary>
        internal static void OnThreadExit()
        {
            lock (s_activationLock)
            {
                s_threads.Remove(Thread.CurrentThread);
            }
        }

        /// <summary>
        /// Stops services through normal waits before the native runtime closes admission.
        /// </summary>
        /// <returns>Zero for a completed retirement; otherwise a native failure status.</returns>
        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        private static int Prepare()
        {
            try
            {
                Thread[] threads;
                lock (s_activationLock)
                {
                    if (IsPreparing)
                    {
                        return 1;
                    }

                    Volatile.Write(ref s_preparing, 1);
                }

                long deadline = Environment.TickCount64 + 5000;
                RuntimeImports.RhRequestForkWorkRetirement();
                while (!AreWorkersRetiring)
                {
                    if (Environment.TickCount64 >= deadline)
                    {
                        return 12;
                    }

                    Thread.Sleep(1);
                }

                lock (s_activationLock)
                {
                    threads = s_threads.ToArray();
                }

                if (s_timerInitialized)
                {
                    TimerQueue.RequestForkRetirement();
                }

                if (s_pool != null && !s_pool.RequestForkRetirement())
                {
                    return 2;
                }

                foreach (Thread thread in threads)
                {
                    int remaining = (int)Math.Max(0, deadline - Environment.TickCount64);
                    if (!thread.Join(remaining))
                    {
                        return 3;
                    }
                }

                lock (s_activationLock)
                {
                    if (s_threads.Count != 0)
                    {
                        return 4;
                    }
                }

                if (s_pool != null && !s_pool.CompleteForkRetirement())
                {
                    return 5;
                }

                if (s_timerInitialized && !TimerQueue.CompleteForkRetirement())
                {
                    return 6;
                }

                return 0;
            }
            catch
            {
                return 7;
            }
        }

        /// <summary>
        /// Restarts preserved services after the parent native checkpoint is released.
        /// </summary>
        /// <returns>Zero for successful resumption.</returns>
        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        private static int Parent() => Resume();

        /// <summary>
        /// Validates the inherited managed retirement before the child starts any service thread.
        /// </summary>
        /// <returns>Zero when the inherited service graph is still retired.</returns>
        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        private static int ChildReset()
        {
            try
            {
                return IsPreparing && s_threads.Count == 0 ? 0 : 8;
            }
            catch
            {
                return 9;
            }
        }

        /// <summary>
        /// Restarts preserved services after child native admission is reopened.
        /// </summary>
        /// <returns>Zero for successful resumption.</returns>
        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        private static int ChildResume() => Resume();

        /// <summary>
        /// Reopens activation and makes preserved queues observable to fresh workers.
        /// </summary>
        /// <returns>Zero for successful resumption.</returns>
        private static int Resume()
        {
            try
            {
                lock (s_activationLock)
                {
                    if (!IsPreparing)
                    {
                        return 10;
                    }

                    RuntimeImports.RhResumeForkWork();
                    Volatile.Write(ref s_preparing, 0);
                }

                if (s_timerInitialized)
                {
                    TimerQueue.ResumeAfterFork();
                }

                s_pool?.ResumeAfterFork();
                return 0;
            }
            catch
            {
                return 11;
            }
        }
    }
}
