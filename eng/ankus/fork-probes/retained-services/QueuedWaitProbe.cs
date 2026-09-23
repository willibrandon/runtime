using System.Runtime.InteropServices;

namespace NativeForkProbe;

/// <summary>
/// Preserves original queued wait callbacks and pending unregister notifications across fork.
/// </summary>
internal static unsafe class QueuedWaitProbe
{
    /// <summary>
    /// Exceeds one wait thread's capacity and matches the independent native snapshot contract.
    /// </summary>
    private const int Count = 70;

    /// <summary>
    /// Supplies distinct captured contexts for safe registrations.
    /// </summary>
    private static readonly AsyncLocal<string?> s_context = new();

    /// <summary>
    /// Retains the original callback objects independently of the native GC handle.
    /// </summary>
    private static WaitState[] s_states = [];

    /// <summary>
    /// Roots the exact original state array.
    /// </summary>
    private static GCHandle s_root;

    /// <summary>
    /// Identifies each original setup without replay after fork.
    /// </summary>
    private static Guid s_token;

    /// <summary>
    /// Records an isolated mutation in each child.
    /// </summary>
    private static int s_marker;

    /// <summary>
    /// Records whether the occupied worker observed native fork preparation.
    /// </summary>
    private static int s_workerResult;

    /// <summary>
    /// Preserves the caller's original minimum worker and I/O limits.
    /// </summary>
    private static (int Workers, int Io) s_minimum;

    /// <summary>
    /// Preserves the caller's original maximum worker and I/O limits.
    /// </summary>
    private static (int Workers, int Io) s_maximum;

    /// <summary>
    /// Observes native counters and fork snapshots without managed reentry.
    /// </summary>
    private static delegate* unmanaged[Cdecl]<int, int, long, long> s_control;

    /// <summary>
    /// Queues original callbacks behind one worker which retires only after native fork preparation begins.
    /// </summary>
    /// <param name="token">The original identity already stored by the native host.</param>
    /// <param name="control">The process-lifetime native observation callback.</param>
    /// <returns>Zero only when every callback is queued and every unregister notification remains pending.</returns>
    internal static int Prepare(Guid token, delegate* unmanaged[Cdecl]<int, int, long, long> control)
    {
        if (s_root.IsAllocated)
        {
            return 160;
        }

        s_control = control;
        s_token = token;
        s_marker = 73;
        s_workerResult = 0;
        ThreadPool.GetMinThreads(out int minimum, out int minimumIo);
        ThreadPool.GetMaxThreads(out int maximum, out int maximumIo);
        s_minimum = (minimum, minimumIo);
        s_maximum = (maximum, maximumIo);
        if (!ThreadPool.SetMinThreads(1, minimumIo) || !ThreadPool.SetMaxThreads(1, maximumIo))
        {
            return 161;
        }

        s_states = new WaitState[Count];
        s_root = GCHandle.Alloc(s_states);
        for (int index = 0; index < Count; index++)
        {
            bool flowContext = index % 2 == 0;
            string context = "queued-wait-" + index;
            s_context.Value = context;
            var state = new WaitState(index, flowContext ? context : null,
                new AutoResetEvent(false), new ManualResetEvent(false));
            s_states[index] = state;
            state.Registration = flowContext
                ? ThreadPool.RegisterWaitForSingleObject(state.Signal, OnWait, state, Timeout.Infinite, true)
                : ThreadPool.UnsafeRegisterWaitForSingleObject(state.Signal, OnWait, state, Timeout.Infinite, true);
        }

        s_context.Value = "changed-after-registration";
        if (!ThreadPool.QueueUserWorkItem(static _ => HoldWorker()))
        {
            return 162;
        }

        long deadline = Environment.TickCount64 + 2000;
        while (s_control(9, 0, 0) == 0 && Environment.TickCount64 < deadline)
        {
            Thread.Sleep(1);
        }

        if (s_control(9, 0, 0) != 1)
        {
            return 163;
        }

        for (int index = 0; index < Count; index++)
        {
            WaitState state = s_states[index];
            state.Signal.Set();
            while (ThreadPool.PendingWorkItemCount < index + 1 && Environment.TickCount64 < deadline)
            {
                Thread.Sleep(1);
            }

            if (ThreadPool.PendingWorkItemCount != index + 1 ||
                !state.Registration!.Unregister(state.Removed) || state.Removed.WaitOne(0) ||
                Volatile.Read(ref state.Callbacks) != 0 || s_control(6, index, 0) != 0)
            {
                return 164;
            }
        }

        s_control(15, 0, ThreadPool.PendingWorkItemCount);
        return 0;
    }

    /// <summary>
    /// Occupies the only worker until native fork preparation requests callback retirement.
    /// </summary>
    private static void HoldWorker()
    {
        int result = s_control(13, 0, 0) == 1 ? 1 : -1;
        Volatile.Write(ref s_workerResult, result);
        s_control(14, 0, result == 1 ? 1 : 0);
    }

    /// <summary>
    /// Executes the original queued callback with its original context and handle identities.
    /// </summary>
    /// <param name="value">The state queued before fork.</param>
    /// <param name="timedOut">False for the original event signal.</param>
    private static void OnWait(object? value, bool timedOut)
    {
        WaitState state = (WaitState)value!;
        if (timedOut || !Thread.CurrentThread.IsThreadPoolThread || s_context.Value != state.Context ||
            !ReferenceEquals(state, s_states[state.Index]) || !s_root.IsAllocated ||
            !ReferenceEquals(s_root.Target, s_states) ||
            state.Signal.SafeWaitHandle.DangerousGetHandle() != state.OriginalSignal ||
            state.Removed.SafeWaitHandle.DangerousGetHandle() != state.OriginalNotification)
        {
            Volatile.Write(ref state.Error, 1);
        }

        Volatile.Write(ref state.Value, state.Index * 29 + 11);
        Interlocked.Increment(ref state.Callbacks);
        s_control(5, state.Index, 0);
    }

    /// <summary>
    /// Requires callback completion and the original unregister notification without resignal or reregistration.
    /// </summary>
    /// <param name="nativePid">The independent native process identifier.</param>
    /// <param name="parentPid">The process that queued all original callbacks.</param>
    /// <param name="report">Reports exact completion counts and process-local mutation.</param>
    /// <returns>Zero only when every original callback and notification completes exactly once.</returns>
    internal static int Check(int nativePid, int parentPid, delegate* unmanaged[Cdecl]<int, long, void> report)
    {
        Span<byte> bytes = stackalloc byte[16];
        s_token.TryWriteBytes(bytes);
        if (s_marker != 73 || Environment.ProcessId != nativePid || Volatile.Read(ref s_workerResult) != 1 ||
            !s_root.IsAllocated || !ReferenceEquals(s_root.Target, s_states) ||
            BitConverter.ToInt64(bytes[..8]) != s_control(12, 0, 0) ||
            BitConverter.ToInt64(bytes[8..]) != s_control(12, 1, 0))
        {
            return 165;
        }

        long deadline = Environment.TickCount64 + 6000;
        for (int index = 0; index < Count; index++)
        {
            WaitState state = s_states[index];
            int remaining = (int)Math.Max(0, deadline - Environment.TickCount64);
            if (s_control(7, index, 0) != 0 || !state.Removed.WaitOne(remaining) ||
                Volatile.Read(ref state.Callbacks) != 1 || Volatile.Read(ref state.Value) != index * 29 + 11 ||
                Volatile.Read(ref state.Error) != 0 || s_control(6, index, 0) != 1 ||
                state.Registration!.Unregister(null) || !state.Removed.WaitOne(0))
            {
                return 166;
            }

            state.Signal.Dispose();
            state.Removed.Dispose();
        }

        if (ThreadPool.PendingWorkItemCount != 0 ||
            !ThreadPool.SetMaxThreads(s_maximum.Workers, s_maximum.Io) ||
            !ThreadPool.SetMinThreads(s_minimum.Workers, s_minimum.Io))
        {
            return 167;
        }

        Task<int> fresh = Task.Run(static () => 193);
        if (!fresh.Wait(2000) || fresh.Result != 193)
        {
            return 168;
        }

        GC.Collect(GC.MaxGeneration, GCCollectionMode.Forced, blocking: true, compacting: true);
        if (!ReferenceEquals(s_root.Target, s_states))
        {
            return 169;
        }

        if (nativePid != parentPid)
        {
            s_marker = 731;
        }

        report(160, Count);
        report(161, s_marker);
        s_root.Free();
        return 0;
    }

    /// <summary>
    /// Retains original event and completion handles plus exact callback outcomes.
    /// </summary>
    /// <param name="index">The original native counter index.</param>
    /// <param name="context">The expected captured execution context.</param>
    /// <param name="signal">The original event which is consumed before fork.</param>
    /// <param name="removed">The original pending unregister notification.</param>
    private sealed class WaitState(int index, string? context, AutoResetEvent signal, ManualResetEvent removed)
    {
        /// <summary>
        /// Gets the original callback index.
        /// </summary>
        internal int Index { get; } = index;

        /// <summary>
        /// Gets the expected original callback context.
        /// </summary>
        internal string? Context { get; } = context;

        /// <summary>
        /// Gets the event consumed before fork.
        /// </summary>
        internal AutoResetEvent Signal { get; } = signal;

        /// <summary>
        /// Gets the pending notification inherited by both processes.
        /// </summary>
        internal ManualResetEvent Removed { get; } = removed;

        /// <summary>
        /// Gets the original signal handle for identity checks.
        /// </summary>
        internal IntPtr OriginalSignal { get; } = signal.SafeWaitHandle.DangerousGetHandle();

        /// <summary>
        /// Gets the original notification handle for identity checks.
        /// </summary>
        internal IntPtr OriginalNotification { get; } = removed.SafeWaitHandle.DangerousGetHandle();

        /// <summary>
        /// Retains the original registration which has a pending callback at fork.
        /// </summary>
        internal RegisteredWaitHandle? Registration;

        /// <summary>
        /// Counts actual callback executions.
        /// </summary>
        internal int Callbacks;

        /// <summary>
        /// Records the original callback result.
        /// </summary>
        internal int Value;

        /// <summary>
        /// Records any mismatch observed inside the callback.
        /// </summary>
        internal int Error;
    }
}
