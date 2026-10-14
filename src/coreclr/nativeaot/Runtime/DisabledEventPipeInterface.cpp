// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

bool EventPipe_IsSupported() { return false; }

void EventPipe_Initialize() {}

bool DiagnosticServer_Initialize() { return false; }
void DiagnosticServer_PauseForDiagnosticsMonitor() {}

void EventPipe_FinishInitialize() {}

void EventPipe_ThreadShutdown() { }

void EventPipe_Shutdown() {}
bool DiagnosticServer_Shutdown() { return false; }
bool EventPipe_PrepareForFork() { return true; }
bool EventPipe_ResumeParentAfterFork() { return true; }
bool EventPipe_ResetChildAfterFork() { return true; }
bool EventPipe_ResumeChildAfterFork() { return true; }
