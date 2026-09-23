using Microsoft.Win32.SafeHandles;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace NativeForkProbe;

/// <summary>
/// Requires a queued wait callback to finish while fork preparation waits for its unregistering caller.
/// </summary>
internal static unsafe class BlockingWaitProbe
{
    /// <summary>
    /// Retains enough registrations to require two native wait threads.
    /// </summary>
    private static AutoResetEvent[] s_signals = [];

    /// <summary>
    /// Retains the original registrations across preparation and fork.
    /// </summary>
    private static RegisteredWaitHandle[] s_registrations = [];

    /// <summary>
    /// Records exact original callback counts.
    /// </summary>
    private static int[] s_counts = [];

    /// <summary>
    /// Roots the original callback state array.
    /// </summary>
    private static GCHandle s_root;

    /// <summary>
    /// Retains the native control callback.
    /// </summary>
    private static delegate* unmanaged[Cdecl]<int, int, long, long> s_control;

    /// <summary>
    /// Preserves original pool limits.
    /// </summary>
    private static (int Min, int MinIo, int Max, int MaxIo) s_limits;

    /// <summary>
    /// Selects an actual finalizer as the blocking caller.
    /// </summary>
    private static bool s_finalizer;

    /// <summary>
    /// Records entry into the unreachable object's finalizer.
    /// </summary>
    private static int s_finalizerStarted;

    /// <summary>
    /// Records successful blocking unregister after callback completion.
    /// </summary>
    private static int s_result;

    /// <summary>
    /// Records a callback contract failure.
    /// </summary>
    private static int s_error;

    /// <summary>
    /// Proves the finalizer object was unreachable before preparation.
    /// </summary>
    private static WeakReference? s_finalizerReference;

    /// <summary>
    /// Queues one original wait callback behind a worker released only during fork preparation.
    /// </summary>
    /// <param name="finalizer">Whether a finalizer performs blocking unregister.</param>
    /// <param name="control">The independent native observations.</param>
    /// <returns>Zero only after the callback is queued and its caller is active.</returns>
    internal static int Prepare(bool finalizer, delegate* unmanaged[Cdecl]<int, int, long, long> control)
    {
        s_control = control;
        s_finalizer = finalizer;
        s_result = 0;
        s_error = 0;
        s_finalizerStarted = 0;
        ThreadPool.GetMinThreads(out int minimum, out int minimumIo);
        ThreadPool.GetMaxThreads(out int maximum, out int maximumIo);
        s_limits = (minimum, minimumIo, maximum, maximumIo);
        if (!ThreadPool.SetMinThreads(1, minimumIo) || !ThreadPool.SetMaxThreads(1, maximumIo))
        {
            return 170;
        }

        s_signals = new AutoResetEvent[70];
        s_registrations = new RegisteredWaitHandle[70];
        s_counts = new int[70];
        s_root = GCHandle.Alloc(s_counts);
        for (int index = 0; index < s_signals.Length; index++)
        {
            s_signals[index] = new AutoResetEvent(false);
            s_registrations[index] = ThreadPool.RegisterWaitForSingleObject(
                s_signals[index], OnWait, index, Timeout.Infinite, true);
        }

        if (!ThreadPool.QueueUserWorkItem(static _ => HoldWorker()))
        {
            return 171;
        }

        long deadline = Environment.TickCount64 + 2000;
        while (s_control(9, 0, 0) != 1 && Environment.TickCount64 < deadline)
        {
            Thread.Sleep(1);
        }

        if (s_control(9, 0, 0) != 1)
        {
            return 172;
        }

        s_signals[0].Set();
        while (ThreadPool.PendingWorkItemCount != 1 && Environment.TickCount64 < deadline)
        {
            Thread.Sleep(1);
        }

        if (ThreadPool.PendingWorkItemCount != 1 || Volatile.Read(ref s_counts[0]) != 0)
        {
            return 173;
        }

        s_control(15, 0, 1);
        if (finalizer)
        {
            s_finalizerReference = CreateFinalizer();
            GC.Collect();
            while (Volatile.Read(ref s_finalizerStarted) == 0 && Environment.TickCount64 < deadline)
            {
                Thread.Sleep(1);
            }

            if (Volatile.Read(ref s_finalizerStarted) != 1 || s_finalizerReference.IsAlive)
            {
                return 174;
            }
        }

        return 0;
    }

    /// <summary>
    /// Holds the only worker until native fork preparation requests callback retirement.
    /// </summary>
    private static void HoldWorker()
    {
        if (s_control(13, 0, 0) != 1)
        {
            Volatile.Write(ref s_error, 1);
            return;
        }

        if (!s_finalizer)
        {
            Unregister();
        }
    }

    /// <summary>
    /// Permits a replacement worker, then blocks until the original queued callback completes.
    /// </summary>
    private static void Unregister()
    {
        using var completion = new BlockingCompletion();
        bool success = ThreadPool.SetMaxThreads(4, s_limits.MaxIo) &&
            ThreadPool.SetMinThreads(3, s_limits.MinIo) &&
            s_registrations[0].Unregister(completion) && Volatile.Read(ref s_counts[0]) == 1 &&
            s_control(6, 0, 0) == 1 && Thread.CurrentThread.IsThreadPoolThread != s_finalizer;
        Volatile.Write(ref s_result, success ? 1 : -1);
        s_control(14, 0, success ? 1 : 0);
    }

    /// <summary>
    /// Counts the original signaled wait callback on an ordinary pool worker.
    /// </summary>
    /// <param name="state">The original registration index.</param>
    /// <param name="timedOut">False for the original event signal.</param>
    private static void OnWait(object? state, bool timedOut)
    {
        int index = (int)state!;
        if (timedOut || !Thread.CurrentThread.IsThreadPoolThread || !ReferenceEquals(s_root.Target, s_counts))
        {
            Volatile.Write(ref s_error, 1);
        }

        if (index == 0 && !CompleteDependentWait())
        {
            Volatile.Write(ref s_error, 1);
        }

        Interlocked.Increment(ref s_counts[index]);
        s_control(5, index, 0);
    }

    /// <summary>
    /// Requires a timer, a new registered wait, and another pool callback during preparation.
    /// </summary>
    /// <returns>Whether the complete dependency chain and unregister notification finished.</returns>
    private static bool CompleteDependentWait()
    {
        using var signal = new AutoResetEvent(false);
        using var completed = new ManualResetEvent(false);
        using var removed = new ManualResetEvent(false);
        RegisteredWaitHandle registration = ThreadPool.RegisterWaitForSingleObject(signal,
            static (state, timedOut) =>
            {
                if (timedOut || !Thread.CurrentThread.IsThreadPoolThread)
                {
                    Volatile.Write(ref s_error, 1);
                }

                ((ManualResetEvent)state!).Set();
            }, completed, Timeout.Infinite, true);
        using var timer = new Timer(static state => ((AutoResetEvent)state!).Set(), signal, 30, Timeout.Infinite);
        bool completedSuccessfully = completed.WaitOne(2000);
        bool removedSuccessfully = registration.Unregister(removed) && removed.WaitOne(2000);
        return completedSuccessfully && removedSuccessfully;
    }

    /// <summary>
    /// Creates a finalizable object without keeping its stack lifetime alive.
    /// </summary>
    /// <returns>A weak observation of the unreachable object.</returns>
    [MethodImpl(MethodImplOptions.NoInlining)]
    private static WeakReference CreateFinalizer() => new(new Cleanup());

    /// <summary>
    /// Verifies completed blocking cleanup and all remaining original registrations in each process.
    /// </summary>
    /// <param name="nativePid">The native process identifier.</param>
    /// <param name="report">The native result sink.</param>
    /// <returns>Zero only for exact completion and healthy subsequent pool work.</returns>
    internal static int Check(int nativePid, delegate* unmanaged[Cdecl]<int, long, void> report)
    {
        if (Environment.ProcessId != nativePid || Volatile.Read(ref s_result) != 1 ||
            Volatile.Read(ref s_error) != 0 || s_counts[0] != 1 || s_control(7, 0, 0) != 1 ||
            s_registrations[0].Unregister(null) || !ReferenceEquals(s_root.Target, s_counts) ||
            (s_finalizer && (s_finalizerReference is null || s_finalizerReference.IsAlive)))
        {
            return 175;
        }

        for (int index = 1; index < s_signals.Length; index++)
        {
            if (s_counts[index] != 0 || s_control(7, index, 0) != 0)
            {
                return 176;
            }

            s_signals[index].Set();
        }

        long deadline = Environment.TickCount64 + 3000;
        for (int index = 1; index < s_signals.Length; index++)
        {
            while (s_control(6, index, 0) == 0 && Environment.TickCount64 < deadline)
            {
                Thread.Sleep(1);
            }

            using var removed = new ManualResetEvent(false);
            if (!s_registrations[index].Unregister(removed) || !removed.WaitOne(2000) ||
                s_counts[index] != 1 || s_control(6, index, 0) != 1 || s_error != 0)
            {
                return 177;
            }
        }

        foreach (AutoResetEvent signal in s_signals)
        {
            signal.Dispose();
        }

        if (!ThreadPool.SetMaxThreads(s_limits.Max, s_limits.MaxIo) ||
            !ThreadPool.SetMinThreads(s_limits.Min, s_limits.MinIo))
        {
            return 178;
        }

        Task<int> fresh = Task.Run(static () => 211);
        if (!fresh.Wait(2000) || fresh.Result != 211)
        {
            return 179;
        }

        report(170, s_counts.Length);
        s_root.Free();
        return 0;
    }

    /// <summary>
    /// Supplies the documented synchronous-unregister sentinel without owning a native handle.
    /// </summary>
    private sealed class BlockingCompletion : WaitHandle
    {
        /// <summary>
        /// Initializes the synchronous completion sentinel.
        /// </summary>
        internal BlockingCompletion()
        {
            SafeWaitHandle = new SafeWaitHandle(new IntPtr(-1), ownsHandle: false);
        }
    }

    /// <summary>
    /// Executes blocking cleanup from the actual finalizer thread.
    /// </summary>
    private sealed class Cleanup
    {
        /// <summary>
        /// Waits for preparation and unregisters the callback whose worker is still needed.
        /// </summary>
        ~Cleanup()
        {
            Volatile.Write(ref s_finalizerStarted, 1);
            if (s_control(16, 0, 0) == 1)
            {
                Unregister();
            }
            else
            {
                Volatile.Write(ref s_error, 1);
            }
        }
    }
}
