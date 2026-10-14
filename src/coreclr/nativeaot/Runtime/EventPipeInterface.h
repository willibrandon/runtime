// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

#ifndef EVENTPIPE_INTERFACE_H
#define EVENTPIPE_INTERFACE_H

// Reports the linked diagnostics implementation, independently of environment settings.
bool EventPipe_IsSupported();

// Initialize EventPipe
void EventPipe_Initialize();

// Initialize DS
bool DiagnosticServer_Initialize();
void DiagnosticServer_PauseForDiagnosticsMonitor();

void EventPipe_FinishInitialize();

void EventPipe_ThreadShutdown();

void EventPipe_Shutdown();
bool DiagnosticServer_Shutdown();

// Retire and recreate diagnostics workers while preserving parent sessions.
bool EventPipe_PrepareForFork();
bool EventPipe_ResumeParentAfterFork();
bool EventPipe_ResetChildAfterFork();
bool EventPipe_ResumeChildAfterFork();

void EventTracing_Initialize();
#endif //EVENTPIPE_INTERFACE_H
