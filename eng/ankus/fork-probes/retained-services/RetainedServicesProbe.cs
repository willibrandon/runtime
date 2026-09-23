using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace NativeForkProbe;

/// <summary>
/// Checks original timer registrations and queued objects retained across the native fork checkpoint.
/// </summary>
public static unsafe class RetainedServicesProbe
{
    /// <summary>
    /// Identifies the one-shot timer experiment.
    /// </summary>
    private const int TimerMode = 1;

    /// <summary>
    /// Identifies the local and global work queue experiment.
    /// </summary>
    private const int QueueMode = 2;

    /// <summary>
    /// Identifies original registered waits and timeout deadlines.
    /// </summary>
    private const int WaitMode = 3;

    /// <summary>
    /// Identifies unregister requests made during native fork preparation.
    /// </summary>
    private const int WaitRetirementMode = 4;

    /// <summary>
    /// Identifies completed waits whose callbacks and unregister notifications are still pending at fork.
    /// </summary>
    private const int QueuedWaitMode = 5;

    /// <summary>
    /// Identifies finalizer-driven blocking unregister during native fork preparation.
    /// </summary>
    private const int FinalizerWaitMode = 6;

    /// <summary>
    /// Identifies a pool worker waiting for a queued callback during preparation.
    /// </summary>
    private const int BlockingWorkerMode = 7;

    /// <summary>
    /// Identifies a finalizer waiting for a queued callback during preparation.
    /// </summary>
    private const int BlockingFinalizerMode = 8;

    /// <summary>
    /// Counts the items enqueued from the native caller's managed setup frame.
    /// </summary>
    private const int GlobalCount = 16;

    /// <summary>
    /// Counts all distinct callback objects, including sixty-four worker-local items.
    /// </summary>
    private const int ItemCount = 80;

    /// <summary>
    /// Retains the synchronous native observation function for callbacks in either process.
    /// </summary>
    private static delegate* unmanaged[Cdecl]<int, int, long, long> s_control;

    /// <summary>
    /// Retains the original one-shot timer object until verification completes.
    /// </summary>
    private static Timer? s_timer;

    /// <summary>
    /// Retains the original callback state independently of the timer.
    /// </summary>
    private static TimerState? s_timerState;

    /// <summary>
    /// Roots the exact original timer through a separate runtime handle.
    /// </summary>
    private static GCHandle s_timerHandle;

    /// <summary>
    /// Roots either original callback state or the original work batch through a separate runtime handle.
    /// </summary>
    private static GCHandle s_stateHandle;

    /// <summary>
    /// Retains all original queued object identities and their exact outcomes.
    /// </summary>
    private static WorkBatch? s_batch;

    /// <summary>
    /// Distinguishes each new parent setup from replay after fork.
    /// </summary>
    private static int s_setups;

    /// <summary>
    /// Creates objects in the parent that must remain pending at the actual native fork snapshot.
    /// </summary>
    /// <param name="mode">Selects retained timers, work, waits, queued wait callbacks, or cleanup during retirement.</param>
    /// <param name="round">The exact next setup count, beginning at one.</param>
    /// <param name="control">The process-lifetime native atomic observation callback.</param>
    /// <param name="report">The synchronous native result marker callback.</param>
    /// <returns>Zero for completed setup; otherwise a setup-specific failure.</returns>
    [UnmanagedCallersOnly(EntryPoint = "fork_probe_retained_prepare", CallConvs = [typeof(CallConvCdecl)])]
    public static int Prepare(int mode, int round, delegate* unmanaged[Cdecl]<int, int, long, long> control,
        delegate* unmanaged[Cdecl]<int, long, void> report)
    {
        try
        {
            if (Interlocked.Increment(ref s_setups) != round || s_stateHandle.IsAllocated)
            {
                return 101;
            }

            s_control = control;
            Guid token = Guid.NewGuid();
            s_control(11, 0, TokenWord(token, 0));
            s_control(11, 1, TokenWord(token, 1));
            int result = mode switch
            {
                TimerMode => PrepareTimer(token),
                QueueMode => PrepareQueue(token),
                WaitMode => RegisteredWaitProbe.Prepare(token, control, RegisteredWaitProbe.RetirementCaller.None),
                WaitRetirementMode => RegisteredWaitProbe.Prepare(token, control, RegisteredWaitProbe.RetirementCaller.Worker),
                QueuedWaitMode => QueuedWaitProbe.Prepare(token, control),
                FinalizerWaitMode => RegisteredWaitProbe.Prepare(token, control, RegisteredWaitProbe.RetirementCaller.Finalizer),
                BlockingWorkerMode or BlockingFinalizerMode => BlockingWaitProbe.Prepare(mode == BlockingFinalizerMode, control),
                _ => 102,
            };

            report(100, round);
            report(101, result);
            return result;
        }
        catch (Exception error)
        {
            report(900, error.HResult);
            return 190;
        }
    }

    /// <summary>
    /// Requires completion of the same pending objects without rearming or requeueing them.
    /// </summary>
    /// <param name="mode">Selects retained timers, work, waits, queued wait callbacks, or cleanup during retirement.</param>
    /// <param name="round">The original setup count retained independently by native code.</param>
    /// <param name="nativePid">The current process identifier from native code.</param>
    /// <param name="parentPid">The original managed parent's native process identifier.</param>
    /// <param name="report">The synchronous native result marker callback.</param>
    /// <returns>Zero only when pending snapshot, identity, completion, and process isolation match.</returns>
    [UnmanagedCallersOnly(EntryPoint = "fork_probe_retained_run", CallConvs = [typeof(CallConvCdecl)])]
    public static int Run(int mode, int round, int nativePid, int parentPid,
        delegate* unmanaged[Cdecl]<int, long, void> report)
    {
        try
        {
            if (s_setups != round || s_control(8, 0, 0) != 1)
            {
                return 103;
            }

            return mode switch
            {
                TimerMode => CheckTimer(nativePid != parentPid, report),
                QueueMode => CheckQueue(nativePid != parentPid, report),
                WaitMode or WaitRetirementMode or FinalizerWaitMode => RegisteredWaitProbe.Check(nativePid, parentPid, report),
                QueuedWaitMode => QueuedWaitProbe.Check(nativePid, parentPid, report),
                BlockingWorkerMode or BlockingFinalizerMode => BlockingWaitProbe.Check(nativePid, report),
                _ => 102,
            };
        }
        catch (Exception error)
        {
            report(900, error.HResult);
            return 190;
        }
    }

    /// <summary>
    /// Arms one original timer with enough time for retirement and an independent future-deadline assertion.
    /// </summary>
    /// <param name="token">The identity captured independently in native storage.</param>
    /// <returns>Zero after the original timer and its roots have been created.</returns>
    private static int PrepareTimer(Guid token)
    {
        var state = new TimerState(token);
        s_timerState = state;
        s_stateHandle = GCHandle.Alloc(state);
        long due = s_control(1, 0, 0) + 3000;
        s_control(10, 0, due);
        s_timer = new Timer(static value => OnTimer((TimerState)value!), state, 3000, Timeout.Infinite);
        state.Timer = s_timer;
        s_timerHandle = GCHandle.Alloc(s_timer);
        return 0;
    }

    /// <summary>
    /// Publishes the exact original timer state and completion without logging from the callback thread.
    /// </summary>
    /// <param name="state">The state retained by the timer before fork.</param>
    private static void OnTimer(TimerState state)
    {
        if (!ReferenceEquals(state, s_timerState) || !ReferenceEquals(state.Timer, s_timer) ||
            !s_timerHandle.IsAllocated || !ReferenceEquals(s_timerHandle.Target, state.Timer) ||
            !s_stateHandle.IsAllocated || !ReferenceEquals(s_stateHandle.Target, state))
        {
            Volatile.Write(ref state._error, 1);
        }

        Volatile.Write(ref state._value, state._marker * 13 + 7);
        Interlocked.Increment(ref state._count);
        s_control(2, 0, 0);
    }

    /// <summary>
    /// Verifies and disposes the same one-shot timer in each process independently.
    /// </summary>
    /// <param name="child">Whether this process must make an isolated state mutation.</param>
    /// <param name="report">Reports exact callback count, value, and marker.</param>
    /// <returns>Zero only for one original callback and intact independent roots.</returns>
    private static int CheckTimer(bool child, delegate* unmanaged[Cdecl]<int, long, void> report)
    {
        TimerState state = s_timerState ?? throw new InvalidOperationException("Timer state was lost.");
        Timer timer = s_timer ?? throw new InvalidOperationException("Timer identity was lost.");
        if (s_control(7, 0, 0) != 0 || state._marker != 73 || !HasOriginalToken(state.Token) ||
            !s_timerHandle.IsAllocated || !ReferenceEquals(s_timerHandle.Target, timer) ||
            !s_stateHandle.IsAllocated || !ReferenceEquals(s_stateHandle.Target, state) ||
            !ReferenceEquals(state.Timer, timer))
        {
            return 110;
        }

        if (!WaitForNativeCount(0, 1, 6000))
        {
            return 111;
        }

        Task disposed = timer.DisposeAsync().AsTask();
        if (!disposed.Wait(2000))
        {
            return 112;
        }

        report(110, s_control(6, 0, 0));
        report(111, Volatile.Read(ref state._value));
        if (s_control(6, 0, 0) != 1 || Volatile.Read(ref state._count) != 1 ||
            Volatile.Read(ref state._value) != 956 || Volatile.Read(ref state._error) != 0)
        {
            return 113;
        }

        if (child)
        {
            state._marker = 731;
        }

        report(112, state._marker);
        bool isolated = state._marker == (child ? 731 : 73);
        s_timerHandle.Free();
        s_stateHandle.Free();
        s_timer = null;
        return isolated ? 0 : 114;
    }

    /// <summary>
    /// Queues local work behind one blocked worker and global work from the setup caller.
    /// </summary>
    /// <param name="token">The original batch identity retained in native storage.</param>
    /// <returns>Zero only when the worker has queued all local items and every global item was accepted.</returns>
    private static int PrepareQueue(Guid token)
    {
        ThreadPool.GetMinThreads(out int minimum, out int minimumIo);
        ThreadPool.GetMaxThreads(out int maximum, out int maximumIo);
        if (!ThreadPool.SetMinThreads(1, minimumIo) || !ThreadPool.SetMaxThreads(1, maximumIo))
        {
            return 120;
        }

        var batch = new WorkBatch(token, minimum, minimumIo, maximum, maximumIo);
        s_batch = batch;
        s_stateHandle = GCHandle.Alloc(batch);
        for (int index = 0; index < ItemCount; index++)
        {
            batch.Items[index] = new WorkItem(batch, index);
        }

        if (!ThreadPool.QueueUserWorkItem(static value => SeedLocalQueue(value), batch, preferLocal: false))
        {
            return 121;
        }

        long deadline = Environment.TickCount64 + 2000;
        while (s_control(9, 0, 0) == 0 && Environment.TickCount64 < deadline)
        {
            Thread.Sleep(1);
        }

        if (s_control(9, 0, 0) != 1 || Volatile.Read(ref batch._error) != 0)
        {
            return 122;
        }

        for (int index = 0; index < GlobalCount; index++)
        {
            if (!ThreadPool.QueueUserWorkItem(static item => OnWorkItem(item), batch.Items[index], preferLocal: false))
            {
                return 123;
            }
        }

        return 0;
    }

    /// <summary>
    /// Creates genuine worker-local entries and waits until the native prepare handler releases the worker.
    /// </summary>
    /// <param name="batch">The original parent batch.</param>
    private static void SeedLocalQueue(WorkBatch batch)
    {
        for (int index = GlobalCount; index < ItemCount; index++)
        {
            if (!ThreadPool.QueueUserWorkItem(static item => OnWorkItem(item), batch.Items[index], preferLocal: true))
            {
                Volatile.Write(ref batch._error, 1);
            }
        }

        s_control(3, 0, 0);
        if (s_control(4, 0, 0) != 1)
        {
            Volatile.Write(ref batch._error, 2);
        }
    }

    /// <summary>
    /// Records exactly one indexed outcome from the original callback object.
    /// </summary>
    /// <param name="item">The object that was queued before the native checkpoint.</param>
    private static void OnWorkItem(WorkItem item)
    {
        WorkBatch batch = item.Batch;
        int index = item.Index;
        if (!ReferenceEquals(batch, s_batch) || !ReferenceEquals(item, batch.Items[index]) ||
            !s_stateHandle.IsAllocated || !ReferenceEquals(s_stateHandle.Target, batch) ||
            !HasOriginalToken(batch.Token))
        {
            Volatile.Write(ref batch._error, 3);
        }

        Volatile.Write(ref batch.Values[index], index * 17 + 5);
        Interlocked.Increment(ref batch.Counts[index]);
        if (s_control(5, index, 0) != 1)
        {
            Volatile.Write(ref batch._error, 4);
        }
    }

    /// <summary>
    /// Requires exact completion of the inherited batch and a later worker barrier before releasing roots.
    /// </summary>
    /// <param name="child">Whether this process must mutate its independent batch copy.</param>
    /// <param name="report">Reports native pending origins and exact completed item count.</param>
    /// <returns>Zero when every original index completed once and configured limits were restored.</returns>
    private static int CheckQueue(bool child, delegate* unmanaged[Cdecl]<int, long, void> report)
    {
        WorkBatch batch = s_batch ?? throw new InvalidOperationException("Work batch was lost.");
        if (batch._marker != 73 || !HasOriginalToken(batch.Token) || !s_stateHandle.IsAllocated ||
            !ReferenceEquals(s_stateHandle.Target, batch))
        {
            return 124;
        }

        int pendingGlobal = 0;
        int pendingLocal = 0;
        for (int index = 0; index < ItemCount; index++)
        {
            long beforeFork = s_control(7, index, 0);
            if (beforeFork is < 0 or > 1)
            {
                return 125;
            }

            if (beforeFork == 0)
            {
                if (index < GlobalCount)
                {
                    pendingGlobal++;
                }
                else
                {
                    pendingLocal++;
                }
            }
        }

        report(120, pendingGlobal);
        report(121, pendingLocal);
        if (pendingGlobal == 0 || pendingLocal == 0)
        {
            return 126;
        }

        long deadline = Environment.TickCount64 + 6000;
        for (int index = 0; index < ItemCount; index++)
        {
            int remaining = (int)Math.Max(0, deadline - Environment.TickCount64);
            if (!WaitForNativeCount(index, 1, remaining))
            {
                return 127;
            }
        }

        var barrier = new BarrierCompletion();
        if (!ThreadPool.QueueUserWorkItem(static signal => Volatile.Write(ref signal._completed, 1), barrier,
            preferLocal: false))
        {
            return 128;
        }

        deadline = Environment.TickCount64 + 2000;
        while (Volatile.Read(ref barrier._completed) == 0 && Environment.TickCount64 < deadline)
        {
            Thread.Sleep(1);
        }

        if (Volatile.Read(ref barrier._completed) != 1 || Volatile.Read(ref batch._error) != 0)
        {
            return 129;
        }

        for (int index = 0; index < ItemCount; index++)
        {
            if (s_control(6, index, 0) != 1 || Volatile.Read(ref batch.Counts[index]) != 1 ||
                Volatile.Read(ref batch.Values[index]) != index * 17 + 5 || batch.Items[index].Index != index ||
                !ReferenceEquals(batch.Items[index].Batch, batch))
            {
                return 130;
            }
        }

        report(122, ItemCount);
        if (child)
        {
            batch._marker = 731;
        }

        report(123, batch._marker);
        bool isolated = batch._marker == (child ? 731 : 73);
        bool restored = ThreadPool.SetMaxThreads(batch.Maximum, batch.MaximumIo) &&
            ThreadPool.SetMinThreads(batch.Minimum, batch.MinimumIo);
        ThreadPool.GetMinThreads(out int minimum, out int minimumIo);
        ThreadPool.GetMaxThreads(out int maximum, out int maximumIo);
        s_stateHandle.Free();
        return isolated && restored && minimum == batch.Minimum && minimumIo == batch.MinimumIo &&
            maximum == batch.Maximum && maximumIo == batch.MaximumIo ? 0 : 131;
    }

    /// <summary>
    /// Polls a native atomic count with a monotonic deadline and no thread-pool-dependent wait.
    /// </summary>
    /// <param name="index">The timer or work item counter.</param>
    /// <param name="minimum">The required count before completion can be inspected.</param>
    /// <param name="timeout">The bounded elapsed-time budget in milliseconds.</param>
    /// <returns>Whether the counter reached the required count before the deadline.</returns>
    private static bool WaitForNativeCount(int index, int minimum, int timeout)
    {
        long deadline = Environment.TickCount64 + timeout;
        while (s_control(6, index, 0) < minimum && Environment.TickCount64 < deadline)
        {
            Thread.Sleep(1);
        }

        return s_control(6, index, 0) >= minimum;
    }

    /// <summary>
    /// Checks both words against native storage captured before the fork.
    /// </summary>
    /// <param name="token">The managed state object's token.</param>
    /// <returns>Whether replay or state substitution has changed either word.</returns>
    private static bool HasOriginalToken(Guid token)
        => TokenWord(token, 0) == s_control(12, 0, 0) && TokenWord(token, 1) == s_control(12, 1, 0);

    /// <summary>
    /// Reads a fixed-width token word without reflection or allocation.
    /// </summary>
    /// <param name="token">The identity bytes.</param>
    /// <param name="index">The zero-based word index.</param>
    /// <returns>The exact signed 64-bit representation of the selected bytes.</returns>
    private static long TokenWord(Guid token, int index)
    {
        Span<byte> bytes = stackalloc byte[16];
        token.TryWriteBytes(bytes);
        return BitConverter.ToInt64(bytes.Slice(index * sizeof(long), sizeof(long)));
    }

    /// <summary>
    /// Retains a timer's immutable identity and separately observed callback result.
    /// </summary>
    /// <param name="token">The native-captured identity.</param>
    private sealed class TimerState(Guid token)
    {
        /// <summary>
        /// Gets the original identity token.
        /// </summary>
        internal Guid Token { get; } = token;

        /// <summary>
        /// Gets or sets the original timer reference, independent of the static field and handle.
        /// </summary>
        internal Timer? Timer { get; set; }

        /// <summary>
        /// Supplies the parent value and the child's isolated mutation.
        /// </summary>
        internal int _marker = 73;

        /// <summary>
        /// Counts exact callback executions.
        /// </summary>
        internal int _count;

        /// <summary>
        /// Stores the callback's exact derived value.
        /// </summary>
        internal int _value;

        /// <summary>
        /// Records an original-state identity failure in the callback thread.
        /// </summary>
        internal int _error;
    }

    /// <summary>
    /// Keeps callback objects, outcomes, and original pool settings reachable across fork.
    /// </summary>
    /// <param name="token">The native-captured batch identity.</param>
    /// <param name="minimum">The original worker minimum.</param>
    /// <param name="minimumIo">The original I/O minimum.</param>
    /// <param name="maximum">The original worker maximum.</param>
    /// <param name="maximumIo">The original I/O maximum.</param>
    private sealed class WorkBatch(Guid token, int minimum, int minimumIo, int maximum, int maximumIo)
    {
        /// <summary>
        /// Gets the original batch identity.
        /// </summary>
        internal Guid Token { get; } = token;

        /// <summary>
        /// Gets the original worker minimum.
        /// </summary>
        internal int Minimum { get; } = minimum;

        /// <summary>
        /// Gets the original I/O minimum.
        /// </summary>
        internal int MinimumIo { get; } = minimumIo;

        /// <summary>
        /// Gets the original worker maximum.
        /// </summary>
        internal int Maximum { get; } = maximum;

        /// <summary>
        /// Gets the original I/O maximum.
        /// </summary>
        internal int MaximumIo { get; } = maximumIo;

        /// <summary>
        /// Gets all original queued object references indexed independently of completion order.
        /// </summary>
        internal WorkItem[] Items { get; } = new WorkItem[ItemCount];

        /// <summary>
        /// Gets per-index callback invocation counts.
        /// </summary>
        internal int[] Counts { get; } = new int[ItemCount];

        /// <summary>
        /// Gets per-index derived callback results.
        /// </summary>
        internal int[] Values { get; } = new int[ItemCount];

        /// <summary>
        /// Supplies the parent value and the child's independent mutation.
        /// </summary>
        internal int _marker = 73;

        /// <summary>
        /// Records a callback failure without throwing on an arbitrary worker.
        /// </summary>
        internal int _error;
    }

    /// <summary>
    /// Supplies a distinct original state object for each queued callback.
    /// </summary>
    /// <param name="batch">The original containing batch.</param>
    /// <param name="index">The exact expected result index.</param>
    private sealed class WorkItem(WorkBatch batch, int index)
    {
        /// <summary>
        /// Gets the original containing batch.
        /// </summary>
        internal WorkBatch Batch { get; } = batch;

        /// <summary>
        /// Gets the callback's unique result index.
        /// </summary>
        internal int Index { get; } = index;
    }

    /// <summary>
    /// Observes a later worker inspection after all original indexed callbacks completed.
    /// </summary>
    private sealed class BarrierCompletion
    {
        /// <summary>
        /// Publishes barrier completion with an acquire/release operation.
        /// </summary>
        internal int _completed;
    }
}
