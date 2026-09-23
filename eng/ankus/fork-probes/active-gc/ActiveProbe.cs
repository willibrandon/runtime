using System.Runtime.InteropServices;

namespace NativeForkProbe;

/// <summary>
/// Adds bounded retained-heap observations around a collection requested immediately before native fork.
/// </summary>
public static unsafe partial class ProbeExports
{
    /// <summary>
    /// Retains independently indexed nodes with two pointer edges per object.
    /// </summary>
    private static ActiveNode[] s_activeNodes = [];

    /// <summary>
    /// Retains sixteen MiB of independently checkable byte patterns across both collections.
    /// </summary>
    private static byte[][] s_activeBytes = [];

    /// <summary>
    /// Roots a middle graph node separately through the handle table for the process lifetime.
    /// </summary>
    private static GCHandle s_activeHandle;

    /// <summary>
    /// Captures the completed background index before the immediately preceding request.
    /// </summary>
    private static long s_activePreviousIndex;

    /// <summary>
    /// Retains the result of a newly unreachable finalizable object for each requested collection.
    /// </summary>
    private static Completion? s_activeCompletion;

    /// <summary>
    /// Observes that the finalizable target becomes unreachable independently of its completion value.
    /// </summary>
    private static WeakReference? s_activeWeak;

    /// <summary>
    /// Builds and promotes a pointer-rich heap without changing collector settings or selection logic.
    /// </summary>
    /// <param name="report">Reports the actual bounded node count and retained byte count.</param>
    /// <returns>Zero after one successful setup; otherwise a setup or data-integrity failure.</returns>
    private static int InitializeActiveHeap(delegate* unmanaged[Cdecl]<int, long, void> report)
    {
        if (s_activeNodes.Length != 0)
        {
            return 70;
        }

        const int nodeCount = 1 << 20;
        ActiveNode[] nodes = new ActiveNode[nodeCount];
        for (int index = 0; index < nodes.Length; index++)
        {
            nodes[index] = new ActiveNode(index * 17 + 11);
        }

        for (int index = 0; index < nodes.Length; index++)
        {
            nodes[index].Next = nodes[(index + 1) & (nodeCount - 1)];
            nodes[index].Other = nodes[index ^ (nodeCount - 1)];
        }

        byte[][] bytes = new byte[256][];
        for (int index = 0; index < bytes.Length; index++)
        {
            bytes[index] = new byte[65536];
            Array.Fill(bytes[index], (byte)index);
        }

        s_activeNodes = nodes;
        s_activeBytes = bytes;
        s_activeHandle = GCHandle.Alloc(nodes[nodeCount / 2]);
        GC.Collect(GC.MaxGeneration, GCCollectionMode.Forced, blocking: true, compacting: true);
        report(70, nodes.Length);
        report(71, bytes.Length * 65536);
        return CheckActiveHeap(report);
    }

    /// <summary>
    /// Requests an ordinary nonblocking full collection and immediately returns to the native fork caller.
    /// </summary>
    /// <param name="report">Reports the completed background index before requesting collection.</param>
    /// <returns>Zero when the request returns normally, without asserting that collection was concurrent.</returns>
    private static int RequestActiveCollection(delegate* unmanaged[Cdecl]<int, long, void> report)
    {
        if (s_activeNodes.Length != 1 << 20)
        {
            return 80;
        }

        var completion = new Completion();
        s_activeCompletion = completion;
        s_activeWeak = CreateFinalizable(completion);
        s_activePreviousIndex = GC.GetGCMemoryInfo(GCKind.Background).Index;
        report(80, s_activePreviousIndex);
        GC.Collect(GC.MaxGeneration, GCCollectionMode.Forced, blocking: false, compacting: false);
        return 0;
    }

    /// <summary>
    /// Requires the requested collection to finish and preserves inherited state before a subsequent new collection.
    /// </summary>
    /// <param name="nativePid">The independently observed current native process identifier.</param>
    /// <param name="parentPid">The native parent's original process identifier.</param>
    /// <param name="tokenLow">The native parent's retained first token word.</param>
    /// <param name="tokenHigh">The native parent's retained second token word.</param>
    /// <param name="report">Reports completed collection metadata and finalizer values.</param>
    /// <returns>Zero for actual concurrent collection, finalization, and complete inherited heap integrity.</returns>
    private static int CheckActiveCollection(int nativePid, int parentPid, long tokenLow, long tokenHigh,
        delegate* unmanaged[Cdecl]<int, long, void> report)
    {
        GCMemoryInfo background = GC.GetGCMemoryInfo(GCKind.Background);
        report(81, background.Index);
        report(82, background.Concurrent ? 1 : 0);
        if (background.Index <= s_activePreviousIndex || !background.Concurrent || background.Generation != GC.MaxGeneration)
        {
            return 81;
        }

        Completion? completion = s_activeCompletion;
        WeakReference? weak = s_activeWeak;
        if (completion is null || weak is null || !completion.Wait() || completion.Value != 73 || weak.IsAlive)
        {
            return 82;
        }

        report(83, completion.Value);
        int integrity = CheckActiveHeap(report);
        return integrity == 0
            ? CheckGraph(nativePid, parentPid, tokenLow, tokenHigh, report, mutateChild: false)
            : integrity;
    }

    /// <summary>
    /// Reads every retained node, both independent edges, the separate handle, and every retained byte.
    /// </summary>
    /// <param name="report">Reports the number of nodes and bytes whose exact values were checked.</param>
    /// <returns>Zero only when all independent data and identity predicates match.</returns>
    private static int CheckActiveHeap(delegate* unmanaged[Cdecl]<int, long, void> report)
    {
        ActiveNode[] nodes = s_activeNodes;
        byte[][] bytes = s_activeBytes;
        const int nodeCount = 1 << 20;
        if (nodes.Length != nodeCount || bytes.Length != 256 || !s_activeHandle.IsAllocated ||
            !ReferenceEquals(s_activeHandle.Target, nodes[nodeCount / 2]))
        {
            return 100;
        }

        for (int index = 0; index < nodes.Length; index++)
        {
            ActiveNode node = nodes[index];
            if (node.Marker != index * 17 + 11 || !ReferenceEquals(node.Next, nodes[(index + 1) & (nodeCount - 1)]) ||
                !ReferenceEquals(node.Other, nodes[index ^ (nodeCount - 1)]))
            {
                return 101;
            }
        }

        for (int index = 0; index < bytes.Length; index++)
        {
            byte[] block = bytes[index];
            if (block.Length != 65536)
            {
                return 102;
            }

            foreach (byte value in block)
            {
                if (value != (byte)index)
                {
                    return 103;
                }
            }
        }

        report(100, nodes.Length);
        report(101, bytes.Length * 65536);
        return 0;
    }

    /// <summary>
    /// Supplies an independently identifiable object with two retained references for real marking work.
    /// </summary>
    /// <param name="marker">The independently computed index-derived marker.</param>
    private sealed class ActiveNode(int marker)
    {
        /// <summary>
        /// Gets the immutable value that must survive collection and fork.
        /// </summary>
        internal int Marker { get; } = marker;

        /// <summary>
        /// Gets or sets the successor edge in the retained cycle.
        /// </summary>
        internal ActiveNode? Next { get; set; }

        /// <summary>
        /// Gets or sets the independently indexed complementary edge.
        /// </summary>
        internal ActiveNode? Other { get; set; }
    }
}
