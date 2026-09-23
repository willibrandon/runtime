// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

#include <eventpipe/ds-rt-config.h>
#include <eventpipe/ep.h>
#include <eventpipe/ep-rt-aot.h>
#include <eventpipe/ds-server.h>

bool EventPipe_IsSupported() { return true; }

void EventPipe_Initialize() { ep_init(); }

bool DiagnosticServer_Initialize() { return ds_server_init(); }
void DiagnosticServer_PauseForDiagnosticsMonitor() { ds_server_pause_for_diagnostics_monitor(); }

void EventPipe_FinishInitialize() { ep_finish_init(); }

void EventPipe_ThreadShutdown() { ep_rt_aot_thread_exited(); }

void EventPipe_Shutdown() { ep_shutdown(); }
bool DiagnosticServer_Shutdown() { return ds_server_shutdown(); }

bool EventPipe_PrepareForFork()
{
#ifdef DS_NATIVEAOT_FORK_LISTENER
    if (!ds_server_pause_listener())
        return false;

    if (ep_prepare_for_fork())
        return true;

    ds_server_resume_listener();
    return false;
#else
    return false;
#endif
}

bool EventPipe_ResumeParentAfterFork()
{
#ifdef DS_NATIVEAOT_FORK_LISTENER
    return ep_resume_after_fork() && ds_server_resume_listener();
#else
    return false;
#endif
}
