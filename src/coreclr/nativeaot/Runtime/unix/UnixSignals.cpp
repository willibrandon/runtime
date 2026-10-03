// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

#include "CommonTypes.h"
#include "PalLimitedContext.h"
#include "CommonMacros.h"
#include "config.h"

#include "UnixSignals.h"

// Add handler for hardware exception signal
bool AddSignalHandler(int signal, SignalHandler handler, struct sigaction* previousAction)
{
    struct sigaction newAction;

    newAction.sa_flags = SA_RESTART;
    newAction.sa_handler = NULL;
    newAction.sa_sigaction = handler;
    newAction.sa_flags |= SA_SIGINFO;

    if (sigaction(signal, NULL, previousAction) == -1)
    {
        ASSERT_UNCONDITIONALLY("Failed to get previous signal handler");
        return false;
    }

    // Chaining the host's handler must preserve the signals it blocks, even
    // when that handler runs on the ordinary stack. A host can rely on this
    // mask while forking and installing the child's signal handlers.
    newAction.sa_mask = previousAction->sa_mask;

    if (previousAction->sa_flags & SA_ONSTACK)
    {
        // If the previous signal handler uses an alternate stack, we need to use it too
        // so that when we chain-call the previous handler, it is called on the kind of
        // stack it expects.
        newAction.sa_flags |= SA_ONSTACK;
    }

    if (sigaction(signal, &newAction, previousAction) == -1)
    {
        ASSERT_UNCONDITIONALLY("Failed to install signal handler");
        return false;
    }

    return true;
}

// Restore original handler for hardware exception signal
void RestoreSignalHandler(int signal_id, struct sigaction *previousAction)
{
    if (-1 == sigaction(signal_id, previousAction, NULL))
    {
        ASSERT_UNCONDITIONALLY("RestoreSignalHandler: sigaction() call failed");
    }
}
