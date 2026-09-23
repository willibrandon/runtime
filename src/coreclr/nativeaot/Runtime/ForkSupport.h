// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

#ifndef __FORK_SUPPORT_H__
#define __FORK_SUPPORT_H__

class Thread;
class IGCHeap;

// Each managed service callback catches exceptions and returns zero on success.
using ForkServiceCallback = int32_t (*)();

// Unix fork checkpoint for hosts that initialize managed code before forking.
void RhForkRecordBackgroundWorker();
void RhForkBeforeManagedEntry();
bool RhForkIsAdmissionClosed();
bool RhForkFinalizerCheckpoint();
void RhForkRegisterFinalizer(Thread* thread);
void RhForkThreadShutdownStarted();
void RhForkThreadShutdownCompleted();

// Shared admission covers pool callbacks, execution-context cleanup and finalizer passes.
extern "C" int32_t RhTryEnterForkWork();
extern "C" void RhExitForkWork();
extern "C" void RhRequestForkWorkRetirement();
extern "C" int32_t RhIsForkWorkRetired();
extern "C" void RhResumeForkWork();

// Owned workstation collector entry points; the generic collector ABI is unchanged.
extern "C" bool RhIsOwnedGCForFork(IGCHeap* heap);
extern "C" bool RhPrepareGCForFork(IGCHeap* heap, uint32_t timeoutMilliseconds);
extern "C" bool RhIsGCReadyForFork();
extern "C" bool RhResumeGCForFork();

#endif // __FORK_SUPPORT_H__
