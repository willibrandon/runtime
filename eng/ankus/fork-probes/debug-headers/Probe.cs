// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace NativeDebugHeaderProbe;

/// <summary>
/// Starts each loaded image's managed runtime and exercises its heap.
/// </summary>
public static class Probe
{
    /// <summary>
    /// Collects managed storage and reports the actual managed process identity.
    /// </summary>
    /// <returns>The process identifier when the retained bytes survive collection.</returns>
    [UnmanagedCallersOnly(EntryPoint = "debug_header_probe_initialize", CallConvs = [typeof(CallConvCdecl)])]
    public static int Initialize()
    {
        byte[] payload = new byte[1024 * 1024];
        payload[0] = 17;
        payload[^1] = 93;
        GC.Collect();
        GC.WaitForPendingFinalizers();
        return payload[0] == 17 && payload[^1] == 93 ? Environment.ProcessId : -1;
    }
}
