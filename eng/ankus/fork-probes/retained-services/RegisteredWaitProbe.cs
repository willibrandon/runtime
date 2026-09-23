using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;

namespace NativeForkProbe;

/// <summary>
/// Preserves registrations spanning multiple wait threads, callback contexts, signals and deadlines across fork.
/// </summary>
internal static unsafe class RegisteredWaitProbe
{
    /// <summary>
    /// Selects the caller that unregisters after native wait-thread retirement.
    /// </summary>
    internal enum RetirementCaller
    {
        /// <summary>
        /// Leaves all cleanup to the ordinary post-fork checks.
        /// </summary>
        None,

        /// <summary>
        /// Unregisters from an occupied pool worker during retirement.
        /// </summary>
        Worker,

        /// <summary>
        /// Uses blocking unregister from an actual managed finalizer during retirement.
        /// </summary>
        Finalizer,
    }

    /// <summary>
    /// Exceeds one portable wait thread's sixty-three user slots.
    /// </summary>
    private const int SignalCount = 70;

    /// <summary>
    /// Identifies the repeating event registration.
    /// </summary>
    private const int RepeatingIndex = 70;

    /// <summary>
    /// Identifies the original finite timeout that is never signaled or rearmed.
    /// </summary>
    private const int TimeoutIndex = 71;

    /// <summary>
    /// Identifies a registration removed before the native snapshot.
    /// </summary>
    private const int CancelledIndex = 72;

    /// <summary>
    /// Carries distinct execution contexts for safe registrations.
    /// </summary>
    private static readonly AsyncLocal<string?> s_context = new();

    /// <summary>
    /// Retains every original callback state and registration.
    /// </summary>
    private static WaitState[] s_states = [];

    /// <summary>
    /// Roots the original array independently of the static field.
    /// </summary>
    private static GCHandle s_root;

    /// <summary>
    /// Identifies the original setup in independently inherited native storage.
    /// </summary>
    private static Guid s_token;

    /// <summary>
    /// Records independent parent and child mutations after callbacks finish.
    /// </summary>
    private static int s_marker;

    /// <summary>
    /// Records whether an active worker must unregister after every native wait thread exits.
    /// </summary>
    private static bool s_checkRetirement;

    /// <summary>
    /// Records the selected cleanup caller without recreating it after fork.
    /// </summary>
    private static RetirementCaller s_retirementCaller;

    /// <summary>
    /// Observes collection of the original unreachable finalizable object.
    /// </summary>
    private static WeakReference? s_finalizerReference;

    /// <summary>
    /// Records completion of unregister operations while framework activation is closed.
    /// </summary>
    private static int s_retirementResult;

    /// <summary>
    /// Counts callbacks from a temporary registration that must remain unsignaled and cancelled.
    /// </summary>
    private static int s_unexpectedCallbacks;

    /// <summary>
    /// Observes native counts and monotonic deadlines without calling back into managed code.
    /// </summary>
    private static delegate* unmanaged[Cdecl]<int, int, long, long> s_control;

    /// <summary>
    /// Creates original registrations without signaling any of their events.
    /// </summary>
    /// <param name="token">The identity already stored by the native host.</param>
    /// <param name="control">The process-lifetime native observation callback.</param>
    /// <param name="caller">The optional active worker or finalizer that unregisters after wait-thread retirement.</param>
    /// <returns>Zero after setup and completed cancellation.</returns>
    internal static int Prepare(Guid token, delegate* unmanaged[Cdecl]<int, int, long, long> control, RetirementCaller caller)
    {
        if (s_root.IsAllocated)
        {
            return 140;
        }

        s_control = control;
        s_token = token;
        s_marker = 73;
        s_checkRetirement = caller != RetirementCaller.None;
        s_retirementCaller = caller;
        s_finalizerReference = null;
        s_retirementResult = 0;
        s_unexpectedCallbacks = 0;
        s_states = new WaitState[CancelledIndex + (s_checkRetirement ? 2 : 1)];
        s_root = GCHandle.Alloc(s_states);
        for (int index = 0; index < s_states.Length; index++)
        {
            bool flowContext = index % 2 == 0;
            string context = "original-wait-" + index;
            s_context.Value = context;
            var state = new WaitState(index, flowContext ? context : null, new AutoResetEvent(false));
            s_states[index] = state;
            int timeout = index == TimeoutIndex ? 3000 : Timeout.Infinite;
            if (index == TimeoutIndex)
            {
                s_control(10, 0, s_control(1, 0, 0) + timeout);
            }

            state.Registration = flowContext
                ? ThreadPool.RegisterWaitForSingleObject(state.Signal, OnWait, state, timeout, index != RepeatingIndex)
                : ThreadPool.UnsafeRegisterWaitForSingleObject(state.Signal, OnWait, state, timeout, index != RepeatingIndex);
        }

        s_context.Value = "changed-after-registration";
        using var removed = new ManualResetEvent(false);
        if (!s_states[CancelledIndex].Registration!.Unregister(removed) || !removed.WaitOne(2000))
        {
            return 141;
        }

        if (s_checkRetirement)
        {
            if (caller == RetirementCaller.Finalizer)
            {
                s_finalizerReference = CreateFinalizer();
                GC.Collect(GC.MaxGeneration, GCCollectionMode.Forced, blocking: true, compacting: true);
            }
            else if (!ThreadPool.QueueUserWorkItem(static _ => UnregisterAfterRetirement()))
            {
                return 149;
            }

            long deadline = Environment.TickCount64 + 2000;
            while (s_control(9, 0, 0) == 0 && Environment.TickCount64 < deadline)
            {
                Thread.Sleep(1);
            }

            if (s_control(9, 0, 0) != 1)
            {
                return 150;
            }
        }

        return 0;
    }

    /// <summary>
    /// Removes both an inherited registration and a newly added registration after their waiter exits.
    /// </summary>
    private static void UnregisterAfterRetirement()
    {
        bool succeeded = false;
        try
        {
            if (s_control(13, 0, 0) != 1)
            {
                return;
            }

            using var removed = new ManualResetEvent(false);
            RegisteredWaitHandle inherited = s_states[CancelledIndex + 1].Registration!;
            if (s_retirementCaller == RetirementCaller.Finalizer)
            {
                using var blocking = new BlockingUnregisterHandle();
                if (Thread.CurrentThread.IsThreadPoolThread || !inherited.Unregister(blocking) || inherited.Unregister(null))
                {
                    return;
                }
            }
            else if (!inherited.Unregister(removed) || !removed.WaitOne(2000) || inherited.Unregister(null))
            {
                return;
            }

            removed.Reset();
            using var signal = new AutoResetEvent(false);
            RegisteredWaitHandle added = ThreadPool.RegisterWaitForSingleObject(signal,
                static (_, _) => Interlocked.Increment(ref s_unexpectedCallbacks), null, Timeout.Infinite, true);
            succeeded = added.Unregister(removed) && removed.WaitOne(2000) && !added.Unregister(null);
        }
        finally
        {
            Volatile.Write(ref s_retirementResult, succeeded ? 1 : -1);
            s_control(14, 0, succeeded ? 1 : 0);
        }
    }

    /// <summary>
    /// Checks callback identity, execution context, reason and count from the original registration.
    /// </summary>
    /// <param name="value">The original state object.</param>
    /// <param name="timedOut">Whether the original finite deadline expired.</param>
    private static void OnWait(object? value, bool timedOut)
    {
        WaitState state = (WaitState)value!;
        if (!ReferenceEquals(state, s_states[state.Index]) || !s_root.IsAllocated ||
            !ReferenceEquals(s_root.Target, s_states) || s_context.Value != state.Context ||
            timedOut != (state.Index == TimeoutIndex) ||
            state.Signal.SafeWaitHandle.DangerousGetHandle() != state.OriginalHandle)
        {
            Volatile.Write(ref state.Error, 1);
        }

        Volatile.Write(ref state.Value, state.Index * 17 + 5);
        Interlocked.Increment(ref state.Count);
        s_control(5, state.Index, 0);
    }

    /// <summary>
    /// Requires original callbacks in each process and checks completion of explicit unregister requests.
    /// </summary>
    /// <param name="nativePid">The current process identifier observed by native code.</param>
    /// <param name="parentPid">The process that created the registrations.</param>
    /// <param name="report">Reports exact counts and independent state mutations.</param>
    /// <returns>Zero only when every callback, cancellation and cleanup boundary matches.</returns>
    internal static int Check(int nativePid, int parentPid, delegate* unmanaged[Cdecl]<int, long, void> report)
    {
        Span<byte> bytes = stackalloc byte[16];
        s_token.TryWriteBytes(bytes);
        if (s_marker != 73 || Environment.ProcessId != nativePid || !s_root.IsAllocated ||
            !ReferenceEquals(s_root.Target, s_states) ||
            BitConverter.ToInt64(bytes[..8]) != s_control(12, 0, 0) ||
            BitConverter.ToInt64(bytes[8..]) != s_control(12, 1, 0) ||
            Volatile.Read(ref s_retirementResult) != (s_checkRetirement ? 1 : 0) ||
            Volatile.Read(ref s_unexpectedCallbacks) != 0 ||
            (s_retirementCaller == RetirementCaller.Finalizer &&
             (s_finalizerReference is null || s_finalizerReference.IsAlive)))
        {
            return 142;
        }

        for (int index = 0; index < s_states.Length; index++)
        {
            if (s_control(7, index, 0) != 0)
            {
                return 143;
            }
        }

        for (int index = CancelledIndex; index < s_states.Length; index++)
        {
            s_states[index].Signal.Set();
        }

        for (int index = 0; index < SignalCount; index++)
        {
            s_states[index].Signal.Set();
        }

        for (int count = 1; count <= 3; count++)
        {
            s_states[RepeatingIndex].Signal.Set();
            if (!WaitForCount(RepeatingIndex, count))
            {
                return 144;
            }
        }

        int callbacks = 0;
        for (int index = 0; index < s_states.Length; index++)
        {
            WaitState state = s_states[index];
            int expected = index >= CancelledIndex ? 0 : index == RepeatingIndex ? 3 : 1;
            if (expected != 0 && !WaitForCount(index, expected))
            {
                return 145;
            }

            using var removed = new ManualResetEvent(false);
            bool requested = state.Registration!.Unregister(removed);
            if (requested != (index < CancelledIndex) || (requested && !removed.WaitOne(2000)) ||
                state.Registration.Unregister(null) || Volatile.Read(ref state.Count) != expected ||
                s_control(6, index, 0) != expected || Volatile.Read(ref state.Error) != 0 ||
                Volatile.Read(ref state.Value) != (expected == 0 ? 0 : index * 17 + 5))
            {
                return 146;
            }

            callbacks += expected;
            state.Signal.Dispose();
        }

        GC.Collect(GC.MaxGeneration, GCCollectionMode.Forced, blocking: true, compacting: true);
        if (!ReferenceEquals(s_root.Target, s_states))
        {
            return 147;
        }

        if (nativePid != parentPid)
        {
            s_marker = 731;
        }

        report(140, callbacks);
        report(141, s_marker);
        s_root.Free();
        return callbacks == 74 && s_marker == (nativePid != parentPid ? 731 : 73) ? 0 : 148;
    }

    /// <summary>
    /// Waits for an exact native callback count with a bounded deadline.
    /// </summary>
    /// <param name="index">The original registration index.</param>
    /// <param name="expected">The exact expected callback count.</param>
    /// <returns>Whether the count was reached without duplicate callbacks.</returns>
    private static bool WaitForCount(int index, int expected)
    {
        long deadline = Environment.TickCount64 + 6000;
        while (s_control(6, index, 0) < expected && Environment.TickCount64 < deadline)
        {
            Thread.Sleep(1);
        }

        return s_control(6, index, 0) == expected;
    }

    /// <summary>
    /// Makes the cleanup object unreachable before collection without keeping it on the caller's stack.
    /// </summary>
    /// <returns>A short weak reference which must be cleared before finalizer completion.</returns>
    [MethodImpl(MethodImplOptions.NoInlining)]
    private static WeakReference CreateFinalizer() => new(new RetirementFinalizer());

    /// <summary>
    /// Runs unregister only when the runtime executes an actual finalizer.
    /// </summary>
    private sealed class RetirementFinalizer
    {
        /// <summary>
        /// Removes the original wait after native wait-thread exit while fork preparation is active.
        /// </summary>
        ~RetirementFinalizer() => UnregisterAfterRetirement();
    }

    /// <summary>
    /// Selects the documented blocking form of registered-wait unregister without owning a native handle.
    /// </summary>
    private sealed class BlockingUnregisterHandle : WaitHandle
    {
        /// <summary>
        /// Initializes the invalid-handle sentinel used by the blocking unregister contract.
        /// </summary>
        internal BlockingUnregisterHandle()
        {
            SafeWaitHandle = new SafeWaitHandle(new IntPtr(-1), ownsHandle: false);
        }
    }

    /// <summary>
    /// Retains an original wait handle, callback context and observable result.
    /// </summary>
    /// <param name="index">The native counter slot.</param>
    /// <param name="context">The expected captured execution context.</param>
    /// <param name="signal">The original event whose handle is retained independently.</param>
    private sealed class WaitState(int index, string? context, AutoResetEvent signal)
    {
        /// <summary>
        /// Gets the original registration index.
        /// </summary>
        internal int Index { get; } = index;

        /// <summary>
        /// Gets the expected callback context.
        /// </summary>
        internal string? Context { get; } = context;

        /// <summary>
        /// Gets the same original signal object in each process.
        /// </summary>
        internal AutoResetEvent Signal { get; } = signal;

        /// <summary>
        /// Gets the handle value after initialization for independent lifetime checks.
        /// </summary>
        internal IntPtr OriginalHandle { get; } = signal.SafeWaitHandle.DangerousGetHandle();

        /// <summary>
        /// Retains the original registration returned to the caller.
        /// </summary>
        internal RegisteredWaitHandle? Registration;

        /// <summary>
        /// Counts actual callback executions.
        /// </summary>
        internal int Count;

        /// <summary>
        /// Records the exact callback result.
        /// </summary>
        internal int Value;

        /// <summary>
        /// Records any callback contract mismatch.
        /// </summary>
        internal int Error;
    }
}
