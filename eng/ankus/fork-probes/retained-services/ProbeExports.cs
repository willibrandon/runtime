using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace NativeForkProbe;

/// <summary>
/// Exposes bounded observations of an initialized Native AOT runtime before and after a native host forks.
/// </summary>
public static unsafe class ProbeExports
{
    /// <summary>
    /// Keeps a cyclic graph reachable through a managed dictionary and a separate GC handle.
    /// </summary>
    private static readonly Dictionary<string, GraphNode> s_graph = [];

    /// <summary>
    /// Retains a bounded array whose independent child mutation must not change its parent's copy.
    /// </summary>
    private static readonly int[] s_numbers = new int[256];

    /// <summary>
    /// Roots the same object graph through the runtime's handle table.
    /// </summary>
    private static GCHandle s_rootHandle;

    /// <summary>
    /// Distinguishes inherited state from a fresh initialization attempt.
    /// </summary>
    private static Guid s_token;

    /// <summary>
    /// Captures the native parent's process identifier during initialization.
    /// </summary>
    private static int s_parentPid;

    /// <summary>
    /// Captures and warms the framework's process identifier property in the parent.
    /// </summary>
    private static int s_managedPidSnapshot;

    /// <summary>
    /// Counts explicit initializer calls so replay cannot masquerade as preserved state.
    /// </summary>
    private static int s_initializations;

    /// <summary>
    /// Creates parent state before the native host warms facilities and forks outside all managed frames.
    /// </summary>
    /// <param name="nativePid">The host's independent getpid result.</param>
    /// <param name="report">A process-lifetime native marker callback with no retained managed delegate.</param>
    /// <returns>Zero for successful one-time initialization; a nonzero observation otherwise.</returns>
    [UnmanagedCallersOnly(EntryPoint = "fork_probe_initialize", CallConvs = [typeof(CallConvCdecl)])]
    public static int Initialize(int nativePid, delegate* unmanaged[Cdecl]<int, long, void> report)
    {
        try
        {
            if (Interlocked.Increment(ref s_initializations) != 1)
            {
                return 1;
            }

            s_parentPid = nativePid;
            s_managedPidSnapshot = Environment.ProcessId;
            s_token = Guid.NewGuid();
            var root = new GraphNode(17);
            root.Next = root;
            s_graph.Add("root", root);
            s_rootHandle = GCHandle.Alloc(root);
            for (int index = 0; index < s_numbers.Length; index++)
            {
                s_numbers[index] = index * 7 + 3;
            }

            report(1, s_managedPidSnapshot);
            report(2, s_initializations);
            return s_managedPidSnapshot == nativePid ? 0 : 2;
        }
        catch (Exception error)
        {
            report(900, error.HResult);
            return 90;
        }
    }

    /// <summary>
    /// Returns the original process and nondeterministic token for the native host to retain across fork.
    /// </summary>
    /// <param name="field">Zero selects the managed PID snapshot; one and two select token words.</param>
    /// <returns>The selected parent snapshot value, or the initialization count for field three.</returns>
    [UnmanagedCallersOnly(EntryPoint = "fork_probe_snapshot", CallConvs = [typeof(CallConvCdecl)])]
    public static long Snapshot(int field)
        => field switch
        {
            0 => s_managedPidSnapshot,
            1 => TokenWord(0),
            2 => TokenWord(1),
            _ => s_initializations,
        };

    /// <summary>
    /// Checks inherited mutations and changes them only in the current process across successive generations.
    /// </summary>
    /// <param name="nativePid">The current native process identifier.</param>
    /// <param name="parentPid">The process that originally initialized the graph.</param>
    /// <param name="tokenLow">The original token's first word.</param>
    /// <param name="tokenHigh">The original token's second word.</param>
    /// <param name="expected">The graph marker inherited from the immediate ancestor.</param>
    /// <param name="replacement">The next process-local graph marker.</param>
    /// <param name="report">Reports actual graph and process values to the native host.</param>
    /// <returns>Zero only when identity, every array value, and the original initialization remain intact.</returns>
    [UnmanagedCallersOnly(EntryPoint = "fork_probe_lineage", CallConvs = [typeof(CallConvCdecl)])]
    public static int Lineage(int nativePid, int parentPid, long tokenLow, long tokenHigh,
        int expected, int replacement, delegate* unmanaged[Cdecl]<int, long, void> report)
    {
        try
        {
            GraphNode root = s_graph["root"];
            report(70, root.Marker);
            report(71, Environment.ProcessId);
            if (s_initializations != 1 || s_parentPid != parentPid || s_managedPidSnapshot != parentPid ||
                Environment.ProcessId != nativePid || TokenWord(0) != tokenLow || TokenWord(1) != tokenHigh ||
                root.Marker != expected || !ReferenceEquals(root, root.Next) ||
                !s_rootHandle.IsAllocated || !ReferenceEquals(root, s_rootHandle.Target))
            {
                return 70;
            }

            for (int index = 0; index < s_numbers.Length; index++)
            {
                if (s_numbers[index] != index * 7 + expected - 14)
                {
                    return 71;
                }
            }

            root.Marker = replacement;
            for (int index = 0; index < s_numbers.Length; index++)
            {
                s_numbers[index] = index * 7 + replacement - 14;
            }

            report(72, root.Marker);
            report(73, s_initializations);
            return 0;
        }
        catch (Exception error)
        {
            report(900, error.HResult);
            return 90;
        }
    }

    /// <summary>
    /// Runs one independent observation, returning errors to native code without allowing an exception to escape.
    /// </summary>
    /// <param name="stage">Graph, PID, collection/finalization, thread pool, timer, or exception stage.</param>
    /// <param name="nativePid">The current host process's getpid result.</param>
    /// <param name="parentPid">The original warmed parent process identifier.</param>
    /// <param name="tokenLow">The token word retained in native parent storage.</param>
    /// <param name="tokenHigh">The second token word retained in native parent storage.</param>
    /// <param name="report">The native marker callback, valid for this synchronous invocation.</param>
    /// <returns>Zero when the observation matches the required process behavior; otherwise a stage-specific failure.</returns>
    [UnmanagedCallersOnly(EntryPoint = "fork_probe_run", CallConvs = [typeof(CallConvCdecl)])]
    public static int Run(int stage, int nativePid, int parentPid, long tokenLow, long tokenHigh,
        delegate* unmanaged[Cdecl]<int, long, void> report)
    {
        try
        {
            return stage switch
            {
                1 => CheckGraph(nativePid, parentPid, tokenLow, tokenHigh, report),
                2 => CheckPid(nativePid, report),
                3 => CheckCollection(report),
                4 => CheckThreadPool(report),
                5 => CheckTimer(report),
                6 => CheckException(report),
                _ => 99,
            };
        }
        catch (Exception error)
        {
            report(900, error.HResult);
            return 90;
        }
    }

    /// <summary>
    /// Checks object identity, a cycle, GC handle identity, token inheritance, and parent-independent mutation.
    /// </summary>
    /// <param name="nativePid">The current process identifier from native code.</param>
    /// <param name="parentPid">The expected initializer process.</param>
    /// <param name="tokenLow">The expected first token word.</param>
    /// <param name="tokenHigh">The expected second token word.</param>
    /// <param name="report">Reports values before and after the child-only mutation.</param>
    /// <returns>Zero when the complete bounded snapshot remains intact.</returns>
    private static int CheckGraph(int nativePid, int parentPid, long tokenLow, long tokenHigh,
        delegate* unmanaged[Cdecl]<int, long, void> report)
    {
        GraphNode root = s_graph["root"];
        report(10, root.Marker);
        if (s_initializations != 1 || s_parentPid != parentPid || s_managedPidSnapshot != parentPid ||
            TokenWord(0) != tokenLow || TokenWord(1) != tokenHigh || root.Marker != 17 ||
            !ReferenceEquals(root, root.Next) || !s_rootHandle.IsAllocated || !ReferenceEquals(root, s_rootHandle.Target))
        {
            return 10;
        }

        for (int index = 0; index < s_numbers.Length; index++)
        {
            if (s_numbers[index] != index * 7 + 3)
            {
                return 11;
            }
        }

        if (nativePid != parentPid)
        {
            root.Marker = 731;
            s_numbers[0] = -73;
            report(11, s_graph["root"].Marker);
            return s_graph["root"].Marker == 731 && s_numbers[0] == -73 ? 0 : 12;
        }

        report(11, root.Marker);
        return 0;
    }

    /// <summary>
    /// Compares the warmed framework process cache with the native process identifier after fork.
    /// </summary>
    /// <param name="nativePid">The independent native getpid result.</param>
    /// <param name="report">Reports the managed value and the expected native value.</param>
    /// <returns>Zero only when the framework reports the current process.</returns>
    private static int CheckPid(int nativePid, delegate* unmanaged[Cdecl]<int, long, void> report)
    {
        int managedPid = Environment.ProcessId;
        report(20, managedPid);
        report(21, nativePid);
        return managedPid == nativePid ? 0 : 20;
    }

    /// <summary>
    /// Forces full collection and requires a newly unreachable object's finalizer to complete within two seconds.
    /// </summary>
    /// <param name="report">Identifies progress around the potentially blocking collection and finalizer wait.</param>
    /// <returns>Zero only when collection returns and actual finalization completes.</returns>
    private static int CheckCollection(delegate* unmanaged[Cdecl]<int, long, void> report)
    {
        var completion = new Completion();
        WeakReference unreachable = CreateFinalizable(completion);
        byte[][] retained = new byte[32][];
        for (int index = 0; index < retained.Length; index++)
        {
            retained[index] = new byte[16384];
            retained[index][0] = (byte)index;
        }

        report(30, 32 * 16384);
        GC.Collect(GC.MaxGeneration, GCCollectionMode.Forced, blocking: true, compacting: true);
        report(31, 0);
        bool finalized = completion.Wait();
        report(32, finalized ? completion.Value : -1);
        GC.KeepAlive(retained);
        GC.KeepAlive(s_graph);
        return finalized && completion.Value == 73 && !unreachable.IsAlive ? 0 : 30;
    }

    /// <summary>
    /// Makes a finalizable object unreachable after a non-inlined frame returns.
    /// </summary>
    /// <param name="completion">The independent finalizer completion observation.</param>
    /// <returns>A short weak reference that must be cleared by collection.</returns>
    [MethodImpl(MethodImplOptions.NoInlining)]
    private static WeakReference CreateFinalizable(Completion completion)
    {
        var value = new FinalizationProbe(completion);
        return new WeakReference(value);
    }

    /// <summary>
    /// Queues fresh work after the parent's worker pool has already initialized.
    /// </summary>
    /// <param name="report">Reports enqueue acceptance and actual completion.</param>
    /// <returns>Zero only when the queued callback produces its exact result within two seconds.</returns>
    private static int CheckThreadPool(delegate* unmanaged[Cdecl]<int, long, void> report)
    {
        var completion = new Completion();
        report(40, 0);
        bool queued = ThreadPool.QueueUserWorkItem(static state => state.Complete(42), completion, preferLocal: false);
        report(41, queued ? 1 : 0);
        bool completed = queued && completion.Wait();
        report(42, completed ? completion.Value : -1);
        return completed && completion.Value == 42 ? 0 : 40;
    }

    /// <summary>
    /// Schedules a new timer after the parent's timer manager has already initialized.
    /// </summary>
    /// <param name="report">Reports timer creation and callback completion.</param>
    /// <returns>Zero only when the callback produces its expected value within two seconds.</returns>
    private static int CheckTimer(delegate* unmanaged[Cdecl]<int, long, void> report)
    {
        var completion = new Completion();
        report(50, 0);
        using var timer = new Timer(static state => ((Completion)state!).Complete(91), completion, 25, Timeout.Infinite);
        report(51, 0);
        bool completed = completion.Wait();
        report(52, completed ? completion.Value : -1);
        return completed && completion.Value == 91 ? 0 : 50;
    }

    /// <summary>
    /// Allocates and throws a fresh exception while proving both catch and finally behavior.
    /// </summary>
    /// <param name="report">Reports the independently checked exception and finally state.</param>
    /// <returns>Zero only when the original exception and finally mutation are preserved.</returns>
    private static int CheckException(delegate* unmanaged[Cdecl]<int, long, void> report)
    {
        bool caught = false;
        int finalValue = 0;
        report(60, 0);
        try
        {
            ThrowFreshException();
        }
        catch (InvalidOperationException error)
        {
            caught = error.Message == "fresh fork probe exception" && error.InnerException is null;
        }
        finally
        {
            finalValue = 731;
        }

        report(61, caught ? finalValue : -1);
        return caught && finalValue == 731 ? 0 : 60;
    }

    /// <summary>
    /// Separates the exception's allocation and throw from its caller's catch frame.
    /// </summary>
    [MethodImpl(MethodImplOptions.NoInlining)]
    private static void ThrowFreshException() => throw new InvalidOperationException("fresh fork probe exception");

    /// <summary>
    /// Reads one fixed-width word from the initialized token without a heap allocation.
    /// </summary>
    /// <param name="index">The zero-based 64-bit word index.</param>
    /// <returns>The exact token bytes represented as a signed 64-bit word.</returns>
    private static long TokenWord(int index)
    {
        Span<byte> bytes = stackalloc byte[16];
        s_token.TryWriteBytes(bytes);
        return BitConverter.ToInt64(bytes.Slice(index * sizeof(long), sizeof(long)));
    }

    /// <summary>
    /// Supplies a cycle and independently mutable state in the warmed managed heap.
    /// </summary>
    /// <param name="marker">The original parent marker.</param>
    private sealed class GraphNode(int marker)
    {
        /// <summary>
        /// Gets or sets the process-local mutation marker.
        /// </summary>
        internal int Marker { get; set; } = marker;

        /// <summary>
        /// Gets or sets the graph cycle's next reference.
        /// </summary>
        internal GraphNode? Next { get; set; }
    }

    /// <summary>
    /// Observes callbacks without inheriting a managed wait handle or disposing state still used by a late callback.
    /// </summary>
    private sealed class Completion
    {
        /// <summary>
        /// Stores the callback's exact result before publication.
        /// </summary>
        private int _value;

        /// <summary>
        /// Publishes callback completion through an acquire/release flag.
        /// </summary>
        private int _completed;

        /// <summary>
        /// Gets the callback result after a successful wait.
        /// </summary>
        internal int Value => Volatile.Read(ref _value);

        /// <summary>
        /// Publishes a completed callback's result without allocating or calling into native reporting.
        /// </summary>
        /// <param name="value">The result that the main probe thread must observe.</param>
        internal void Complete(int value)
        {
            Volatile.Write(ref _value, value);
            Volatile.Write(ref _completed, 1);
        }

        /// <summary>
        /// Polls for at most two seconds of requested sleeps; the native supervisor also bounds scheduler delays.
        /// </summary>
        /// <returns>Whether the callback completed before the bounded polling budget elapsed.</returns>
        internal bool Wait()
        {
            for (int attempt = 0; attempt < 200; attempt++)
            {
                if (Volatile.Read(ref _completed) != 0)
                {
                    return true;
                }

                Thread.Sleep(10);
            }

            return Volatile.Read(ref _completed) != 0;
        }
    }

    /// <summary>
    /// Produces an observable callback only when the runtime actually runs a newly unreachable object's finalizer.
    /// </summary>
    /// <param name="completion">The result holder kept live by the invoking probe.</param>
    private sealed class FinalizationProbe(Completion completion)
    {
        /// <summary>
        /// Signals actual finalizer execution without native resources or a disposable wait handle.
        /// </summary>
        ~FinalizationProbe() => completion.Complete(73);
    }
}
