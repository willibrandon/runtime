// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace NativeHostShutdownProbe;

/// <summary>
/// Observes ordinary managed cleanup when a dormant native host thread exits.
/// </summary>
public static class Probe
{
    /// <summary>
    /// Retains the native owner so another native thread can observe its managed exit.
    /// </summary>
    private static Thread? s_owner;

    /// <summary>
    /// Requires the owner's thread-exit callback to abandon a held mutex.
    /// </summary>
    private static readonly Mutex s_mutex = new();

    /// <summary>
    /// Attaches the native owner and gives it observable thread-exit work.
    /// </summary>
    /// <returns>Zero on successful initialization.</returns>
    [UnmanagedCallersOnly(EntryPoint = "shutdown_probe_initialize", CallConvs = [typeof(CallConvCdecl)])]
    public static int Initialize()
    {
        s_owner = Thread.CurrentThread;
        return s_mutex.WaitOne(0) ? 0 : 1;
    }

    /// <summary>
    /// Verifies that native thread shutdown ran the managed join and mutex cleanup.
    /// </summary>
    /// <returns>Zero only when both independently observable cleanup operations completed.</returns>
    [UnmanagedCallersOnly(EntryPoint = "shutdown_probe_verify", CallConvs = [typeof(CallConvCdecl)])]
    public static int Verify()
    {
        if (s_owner is null || !s_owner.Join(TimeSpan.FromSeconds(5)) || s_owner.IsAlive)
        {
            return 2;
        }

        try
        {
            if (s_mutex.WaitOne(0))
            {
                s_mutex.ReleaseMutex();
            }

            return 3;
        }
        catch (AbandonedMutexException)
        {
            s_mutex.ReleaseMutex();
            byte[] payload = new byte[1024 * 1024];
            payload[0] = 17;
            payload[^1] = 93;
            GC.Collect();
            GC.WaitForPendingFinalizers();
            return payload[0] == 17 && payload[^1] == 93 ? 0 : 4;
        }
    }
}
