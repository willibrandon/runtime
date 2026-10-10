// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

#ifndef __UNIX_SIGNALS_H__
#define __UNIX_SIGNALS_H__

#include <signal.h>

#ifdef SIGRTMIN
#define INJECT_ACTIVATION_SIGNAL SIGRTMIN
#else
// Native hosts send SIGUSR1 to their own processes; PostgreSQL uses it for
// interprocess notifications that its main thread must handle. Without
// real-time signals, use SIGIO: hosts rarely request it and it is ignored by default.
#define INJECT_ACTIVATION_SIGNAL SIGIO
#endif

typedef void (*SignalHandler)(int code, siginfo_t* siginfo, void* context);

bool AddSignalHandler(int signal, SignalHandler handler, struct sigaction* previousAction);
void RestoreSignalHandler(int signal_id, struct sigaction* previousAction);

#endif // __UNIX_SIGNALS_H__
