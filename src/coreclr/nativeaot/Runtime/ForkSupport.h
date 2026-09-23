// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

#ifndef __FORK_SUPPORT_H__
#define __FORK_SUPPORT_H__

class Thread;
class IGCHeap;

// Each managed service callback catches exceptions and returns zero on success.
using ForkServiceCallback = int32_t (*)();

// Experimental Unix fork checkpoint. No production hosting contract is implied.
void RhForkCaptureStartupConfiguration();
void RhForkRecordBackgroundWorker();
void RhForkBeforeManagedEntry();
bool RhForkIsAdmissionClosed();
void RhForkFinalizerCheckpoint();
void RhForkRegisterFinalizer(Thread* thread);
void RhForkThreadShutdownStarted();
void RhForkThreadShutdownCompleted();

// Owned workstation collector entry points; the generic collector ABI is unchanged.
extern "C" bool RhIsOwnedGCForFork(IGCHeap* heap);
extern "C" bool RhPrepareGCForFork(IGCHeap* heap, uint32_t timeoutMilliseconds);
extern "C" bool RhIsGCReadyForFork();
extern "C" bool RhResumeGCForFork();

#endif // __FORK_SUPPORT_H__
