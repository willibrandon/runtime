// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

using System.Collections.Generic;

namespace System.Threading
{
    /// <summary>
    /// A LIFO semaphore.
    /// Waits on this semaphore are uninterruptible.
    /// </summary>
    internal sealed partial class LowLevelLifoSemaphore : IDisposable
    {
        private WaitSubsystem.WaitableObject _semaphore;

        private void Create(int maximumSignalCount)
        {
            _semaphore = WaitSubsystem.WaitableObject.NewSemaphore(0, maximumSignalCount);
        }

        public void Dispose()
        {
        }

        private bool WaitCore(int timeoutMs)
        {
            return WaitSubsystem.Wait(_semaphore, timeoutMs, false, true) == WaitHandle.WaitSuccess;
        }

        private void ReleaseCore(int count)
        {
            WaitSubsystem.ReleaseSemaphore(_semaphore, count);
        }

        /// <summary>
        /// Makes one shutdown credit available per worker after ordinary activation has stopped.
        /// </summary>
        /// <param name="workerCount">The number of worker loops that can still enter a wait.</param>
        internal void WakeForForkRetirement(int workerCount)
        {
            int additional = workerCount - (int)_separated._counts.SignalCount;
            if (additional > 0)
            {
                Release(additional);
            }
        }

        /// <summary>
        /// Reinitializes the private wait object only after all its waiters have exited normally.
        /// </summary>
        /// <returns>Whether no waiter or spinner remains.</returns>
        internal bool ResetAfterForkRetirement()
        {
            Counts counts = _separated._counts;
            if (counts.WaiterCount != 0 || counts.SpinnerCount != 0)
            {
                return false;
            }

            _separated = default;
            Create(_maximumSignalCount);
            return true;
        }
    }
}
