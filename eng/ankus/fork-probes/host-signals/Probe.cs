// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace NativeHostSignalProbe;

/// <summary>
/// Starts each kind of runtime thread a native host can receive signals beside.
/// </summary>
public static class Probe
{
    /// <summary>
    /// Releases the parked managed and thread-pool threads.
    /// </summary>
    private static readonly ManualResetEventSlim s_release = new();

    /// <summary>
    /// Counts the parked threads that have started.
    /// </summary>
    private static readonly CountdownEvent s_started = new(2);

    /// <summary>
    /// Retains the parked managed thread so it can be joined.
    /// </summary>
    private static Thread? s_parked;

    /// <summary>
    /// Keeps the spinning thread in managed code until collection completes.
    /// </summary>
    private static volatile bool s_spin;

    /// <summary>
    /// Counts iterations of the spinning thread.
    /// </summary>
    private static long s_iterations;

    /// <summary>
    /// Starts a managed thread, a thread-pool worker and a background collection.
    /// </summary>
    /// <returns>Zero once both parked threads are running.</returns>
    [UnmanagedCallersOnly(EntryPoint = "signal_probe_start", CallConvs = [typeof(CallConvCdecl)])]
    public static int Start()
    {
        s_parked = new Thread(Park) { IsBackground = true };
        s_parked.Start();
        ThreadPool.UnsafeQueueUserWorkItem(static _ => Park(), null);

        // A background collection starts the collector's own thread.
        GC.Collect(2, GCCollectionMode.Forced, blocking: false);
        return s_started.Wait(TimeSpan.FromSeconds(5)) ? 0 : 1;
    }

    /// <summary>
    /// Collects while another thread spins in managed code, which requires the activation signal.
    /// </summary>
    /// <returns>Zero when the collection completes and the spinning thread exits.</returns>
    [UnmanagedCallersOnly(EntryPoint = "signal_probe_collect", CallConvs = [typeof(CallConvCdecl)])]
    public static int Collect()
    {
        s_spin = true;
        Thread spinner = new(Spin) { IsBackground = true };
        spinner.Start();
        while (Volatile.Read(ref s_iterations) == 0)
        {
            Thread.Yield();
        }

        int collections = GC.CollectionCount(2);
        GC.Collect();
        bool collected = GC.CollectionCount(2) > collections;
        s_spin = false;
        return spinner.Join(TimeSpan.FromSeconds(5)) && collected ? 0 : 2;
    }

    /// <summary>
    /// Releases and joins the parked managed thread.
    /// </summary>
    /// <returns>Zero when the parked managed thread exits.</returns>
    [UnmanagedCallersOnly(EntryPoint = "signal_probe_stop", CallConvs = [typeof(CallConvCdecl)])]
    public static int Stop()
    {
        s_release.Set();
        return s_parked is not null && s_parked.Join(TimeSpan.FromSeconds(5)) ? 0 : 3;
    }

    /// <summary>
    /// Reports that the thread started, then waits for release.
    /// </summary>
    private static void Park()
    {
        s_started.Signal();
        s_release.Wait();
    }

    /// <summary>
    /// Loops without calls or allocations until collection completes.
    /// </summary>
    private static void Spin()
    {
        long iterations = 0;
        while (s_spin)
        {
            Volatile.Write(ref s_iterations, ++iterations);
        }
    }
}
