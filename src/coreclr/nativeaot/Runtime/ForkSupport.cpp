// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

#include "common.h"
#include "gcenv.h"
#include "gcheaputilities.h"
#include "slist.h"
#include "regdisplay.h"
#include "StackFrameIterator.h"
#include "thread.h"
#include "threadstore.h"
#include "threadstore.inl"
#include "thread.inl"
#include "EventPipeInterface.h"
#include "ForkSupport.h"

#if ((defined(TARGET_LINUX) && defined(TARGET_AMD64)) || \
     (defined(TARGET_OSX) && (defined(TARGET_AMD64) || defined(TARGET_ARM64)))) && !defined(DACCESS_COMPILE)

#include <atomic>
#include <pthread.h>
#include <unistd.h>
#ifdef TARGET_OSX
#include <mach/mach.h>
extern "C" int __is_threaded;
#endif

void RhEnableFinalization();
void RhDiscardFinalizationAfterFork();
bool RhJoinFinalization();
bool RhRestartFinalization();

namespace
{
    enum class ForkState : uint32_t
    {
        Disabled,
        Registering,
        Enabling,
        Idle,
        Dormant,
        PreparingManaged,
        Preparing,
        ForkingDormant,
        ParentResuming,
        ChildPending,
        ChildRepair,
        ChildResuming
    };

    static_assert(ATOMIC_INT_LOCK_FREE == 2, "Fork checkpoint state must be lock free");
    static_assert(ATOMIC_POINTER_LOCK_FREE == 2, "Fork checkpoint pointers must be lock free");

    std::atomic<ForkState> s_state(ForkState::Disabled);
    std::atomic<Thread*> s_finalizer(nullptr);
    std::atomic<uint32_t> s_request(0);
    std::atomic<uint32_t> s_acknowledged(0);
    std::atomic<uint32_t> s_threadShutdowns(0);
    std::atomic<bool> s_backgroundWorkerStarted(false);
    std::atomic<bool> s_servicesRegistered(false);
    std::atomic<bool> s_finalizerExit(false);
    // Close admission atomically with the last callback's exit, preserving queued work.
    constexpr uint32_t WorkPreparing = 1u << 31;
    constexpr uint32_t WorkRetired = 1u << 30;
    constexpr uint32_t WorkCountMask = WorkRetired - 1;
    std::atomic<uint32_t> s_work(0);
    ForkServiceCallback s_prepareServices;
    ForkServiceCallback s_resumeParentServices;
    ForkServiceCallback s_resetChildServices;
    ForkServiceCallback s_resumeChildServices;
    Thread* s_owner;
    pid_t s_hostProcess;
    uint32_t s_hostManagedDepth;
    Thread* s_inheritedFinalizer;
    bool s_hasInheritedFinalizer;
    // Runtime images recover independently. Another image can start pthreads and
    // reuse the vanished finalizer's TLS before this image's first managed entry.
    gc_alloc_context s_inheritedFinalizerAllocContext;

    [[noreturn]] void ForkFailure(const char* message)
    {
        // Do not invoke managed fail-fast or dump creation, which could fork recursively.
        PalPrintFatalError(message);
        _exit(198);
    }

    bool HasSupportedConfiguration()
    {
        return !s_backgroundWorkerStarted.load() && RhIsOwnedGCForFork(GCHeapUtilities::GetGCHeap());
    }

    void InvokeServiceCallback(ForkServiceCallback callback, const char* failure)
    {
        if (callback != nullptr && callback() != 0)
        {
            ForkFailure(failure);
        }
    }

    int32_t EnableFailure(int32_t result)
    {
        s_state.store(ForkState::Disabled);
        return result;
    }

    void CompleteChildRecovery();
    void PrepareDormant();

    bool ResetHostThreadState()
    {
#ifdef TARGET_OSX
        thread_act_array_t threads = nullptr;
        mach_msg_type_number_t count = 0;
        kern_return_t result = task_threads(mach_task_self(), &threads, &count);
        if (result != KERN_SUCCESS)
        {
            return false;
        }

        for (mach_msg_type_number_t index = 0; index < count; index++)
        {
            mach_port_deallocate(mach_task_self(), threads[index]);
        }

        vm_deallocate(mach_task_self(), reinterpret_cast<vm_address_t>(threads), count * sizeof(thread_t));
        if (count != 1)
        {
            return false;
        }

        __is_threaded = 0;
#endif
        return true;
    }

    void PrepareFork()
    {
        Thread* caller = ThreadStore::RawGetCurrentThread();
        if (caller != s_owner || !caller->IsAtNativeTopOfStackForFork())
        {
            ForkFailure("NativeAOT fork support: fork requires the enabled native caller outside managed frames.\n");
        }

        ForkState expected = ForkState::Dormant;
        if (s_state.compare_exchange_strong(expected, ForkState::ForkingDormant))
        {
            return;
        }

        // A native child may fork again before its first managed entry. Complete
        // its inherited retirement before preparing a fresh checkpoint, exactly
        // as managed reentry would. Parent and grandchild then each inherit the
        // new finalizer snapshot and a fully retired set of framework services.
        if (s_state.load() == ForkState::ChildPending)
        {
            CompleteChildRecovery();
        }

        expected = ForkState::Idle;
        if (!s_state.compare_exchange_strong(expected, ForkState::PreparingManaged))
        {
            ForkFailure("NativeAOT fork support: overlapping or incomplete fork checkpoint.\n");
        }

        // Keep ordinary admission open while framework workers retire. Their managed
        // thread-exit callbacks must run before native ThreadStore removal can finish.
        InvokeServiceCallback(s_prepareServices,
            "NativeAOT fork support: managed service preparation failed.\n");

        if (!EventPipe_PrepareForFork())
        {
            ForkFailure("NativeAOT fork support: diagnostics preparation failed.\n");
        }

        if (!RhPrepareGCForFork(GCHeapUtilities::GetGCHeap(), 30000))
        {
            ForkFailure("NativeAOT fork support: GC did not retire its collectors.\n");
        }

        ThreadStore* store = GetThreadStore();
        uint32_t attempts = 0;
        while (true)
        {
            store->LockThreadStore();
            bool drained = store->HasOnlyForkThreads(caller, s_finalizer.load()) &&
                s_threadShutdowns.load() == 0;
            if (drained)
            {
                // Thread.Join is signaled before native detachment. Only the real
                // list drain admits the freeze, atomically with respect to attachment.
                s_state.store(ForkState::Preparing);
            }

            store->UnlockThreadStore();
            if (drained)
            {
                break;
            }

            if (++attempts == 10000)
            {
                ForkFailure("NativeAOT fork support: managed service threads did not detach.\n");
            }

            PalSleep(1);
        }

        uint32_t request = s_request.fetch_add(1) + 1;
        if (request == 0)
        {
            ForkFailure("NativeAOT fork support: checkpoint sequence exhausted.\n");
        }

        RhEnableFinalization();
        attempts = 0;
        while (s_acknowledged.load() != request)
        {
            if (++attempts == 10000)
            {
                ForkFailure("NativeAOT fork support: finalizer did not reach its idle checkpoint.\n");
            }

            PalSleep(1);
        }

        store->LockThreadStore();
        Thread* finalizer = s_finalizer.load();
        bool supported = HasSupportedConfiguration() && RhIsGCReadyForFork() &&
            !GCHeapUtilities::IsGCInProgress(TRUE) && s_threadShutdowns.load() == 0 &&
            store->HasOnlyForkThreads(caller, finalizer);
        if (supported)
        {
            // The finalizer is parked and its allocation context cannot change.
            // Preserve the entire EE/GC contract in ordinary image-owned storage.
            s_inheritedFinalizerAllocContext = *finalizer->GetAllocContext();
            s_hasInheritedFinalizer = true;
        }

        store->UnlockThreadStore();
        if (!supported)
        {
            ForkFailure("NativeAOT fork support: checkpoint requires only caller/finalizer and fully retired collectors.\n");
        }

        // Admission remains closed and the only other runtime thread is parked outside
        // event waits. No runtime lock or condition waiter is deliberately copied held.
        s_inheritedFinalizer = finalizer;
    }

    void ParentAfterFork()
    {
        if (s_state.load() == ForkState::ForkingDormant)
        {
            s_state.store(ForkState::Dormant);
            return;
        }

        s_inheritedFinalizer = nullptr;
        s_inheritedFinalizerAllocContext.init();
        s_hasInheritedFinalizer = false;
        // Admission reopens before the callback can perform Thread.Start handshakes.
        // The distinct state still rejects a recursive fork during service restart.
        s_state.store(ForkState::ParentResuming);
        if (!RhResumeGCForFork())
        {
            ForkFailure("NativeAOT fork support: parent GC resume failed.\n");
        }

        if (!EventPipe_ResumeParentAfterFork())
        {
            ForkFailure("NativeAOT fork support: parent diagnostics resume failed.\n");
        }

        InvokeServiceCallback(s_resumeParentServices,
            "NativeAOT fork support: managed parent service resume failed.\n");
        s_state.store(ForkState::Idle);
    }

    void ChildAfterFork()
    {
        // No allocation, managed execution, event mutation, or thread creation here.
        // A running child loses its finalizer; a dormant host retired it before fork.
        if (s_state.load() != ForkState::ForkingDormant)
        {
            GetThreadStore()->RemoveFinalizerAfterFork(s_inheritedFinalizer);
        }

        s_inheritedFinalizer = nullptr;
        if (!GetThreadStore()->RefreshCallerAfterFork(s_owner))
        {
            ForkFailure("NativeAOT fork support: failed to restore child thread identity.\n");
        }

        s_finalizer.store(nullptr);
        s_acknowledged.store(0);
        s_finalizerExit.store(false);
        s_state.store(ForkState::ChildPending);
    }

    void CompleteChildRecovery()
    {
        if (ThreadStore::RawGetCurrentThread() != s_owner)
        {
            ForkFailure("NativeAOT fork support: child recovery must run on the forking thread.\n");
        }

        s_state.store(ForkState::ChildRepair);
        // Never dereference the vanished pthread's TLS during lazy recovery.
        // The durable snapshot supplies the allocation tail and unused-byte count.
        if (s_hasInheritedFinalizer)
        {
            Thread::ReleaseAllocationContextForFork(&s_inheritedFinalizerAllocContext);
            s_inheritedFinalizerAllocContext.init();
            s_hasInheritedFinalizer = false;
        }

        // Repair shared managed service state before inherited finalizable objects
        // can observe it. This callback must not start or join framework workers.
        InvokeServiceCallback(s_resetChildServices,
            "NativeAOT fork support: managed child service reset failed.\n");

        if (!EventPipe_ResetChildAfterFork())
        {
            ForkFailure("NativeAOT fork support: child diagnostics reset failed.\n");
        }

        RhDiscardFinalizationAfterFork();
        if (!RhRestartFinalization())
        {
            ForkFailure("NativeAOT fork support: failed to restart child finalization.\n");
        }

        // The inherited worker can consume a wake immediately before parking.
        // Drain inherited pending work even if no child GC or explicit wait occurs.
        RhEnableFinalization();

        // The replacement finalizer and service workers may now enter managed code.
        // Starting service workers before this point would deadlock their handshakes.
        s_state.store(ForkState::ChildResuming);
        if (!RhResumeGCForFork())
        {
            ForkFailure("NativeAOT fork support: child GC resume failed.\n");
        }

        if (!EventPipe_ResumeChildAfterFork())
        {
            ForkFailure("NativeAOT fork support: child diagnostics resume failed.\n");
        }

        InvokeServiceCallback(s_resumeChildServices,
            "NativeAOT fork support: managed child service resume failed.\n");
        s_state.store(ForkState::Idle);
    }

    void ResumeDormantHost()
    {
        s_finalizerExit.store(false);
        s_acknowledged.store(0);
        s_state.store(ForkState::ParentResuming);
        if (!RhRestartFinalization())
        {
            ForkFailure("NativeAOT fork support: failed to restart host finalization.\n");
        }

        RhEnableFinalization();
        if (!RhResumeGCForFork())
        {
            ForkFailure("NativeAOT fork support: host GC resume failed.\n");
        }

        if (!EventPipe_ResumeParentAfterFork())
        {
            ForkFailure("NativeAOT fork support: host diagnostics resume failed.\n");
        }

        InvokeServiceCallback(s_resumeParentServices,
            "NativeAOT fork support: managed host service resume failed.\n");
        s_state.store(ForkState::Idle);
    }

    void PrepareDormant()
    {
        Thread* caller = ThreadStore::RawGetCurrentThread();
        if (caller != s_owner || !caller->IsAtNativeTopOfStackForFork())
        {
            ForkFailure("NativeAOT fork support: host retirement requires its native owner.\n");
        }

        ForkState expected = ForkState::Idle;
        if (!s_state.compare_exchange_strong(expected, ForkState::PreparingManaged))
        {
            ForkFailure("NativeAOT fork support: host retirement overlapped another checkpoint.\n");
        }

        InvokeServiceCallback(s_prepareServices,
            "NativeAOT fork support: managed host service preparation failed.\n");
        if (!EventPipe_PrepareForFork())
        {
            ForkFailure("NativeAOT fork support: host diagnostics preparation failed.\n");
        }

        if (!RhPrepareGCForFork(GCHeapUtilities::GetGCHeap(), 30000))
        {
            ForkFailure("NativeAOT fork support: host GC did not retire its collectors.\n");
        }

        ThreadStore* store = GetThreadStore();
        uint32_t attempts = 0;
        while (true)
        {
            store->LockThreadStore();
            bool drained = store->HasOnlyForkThreads(caller, s_finalizer.load()) &&
                s_threadShutdowns.load() == 0;
            if (drained)
            {
                s_state.store(ForkState::Preparing);
            }

            store->UnlockThreadStore();
            if (drained)
            {
                break;
            }

            if (++attempts == 10000)
            {
                ForkFailure("NativeAOT fork support: host service threads did not detach.\n");
            }

            PalSleep(1);
        }

        uint32_t request = s_request.fetch_add(1) + 1;
        if (request == 0)
        {
            ForkFailure("NativeAOT fork support: checkpoint sequence exhausted.\n");
        }

        s_finalizerExit.store(true);
        RhEnableFinalization();
        attempts = 0;
        while (s_acknowledged.load() != request)
        {
            if (++attempts == 10000)
            {
                ForkFailure("NativeAOT fork support: host finalizer did not acknowledge retirement.\n");
            }

            PalSleep(1);
        }

        attempts = 0;
        while (true)
        {
            store->LockThreadStore();
            bool retired = s_finalizer.load() == nullptr && s_threadShutdowns.load() == 0 &&
                store->HasOnlyForkCaller(caller);
            store->UnlockThreadStore();
            if (retired)
            {
                break;
            }

            if (++attempts == 10000)
            {
                ForkFailure("NativeAOT fork support: host finalizer did not detach.\n");
            }

            PalSleep(1);
        }

        if (!RhJoinFinalization())
        {
            ForkFailure("NativeAOT fork support: host finalizer did not finish native teardown.\n");
        }

        if (!HasSupportedConfiguration() || !RhIsGCReadyForFork() ||
            GCHeapUtilities::IsGCInProgress(TRUE))
        {
            ForkFailure("NativeAOT fork support: host runtime did not become dormant.\n");
        }

        // Darwin wakes pthread_join before __bsdthread_terminate has removed the
        // kernel thread from task_threads. Wait for that asynchronous teardown
        // before resetting libpthread's single-threaded state.
        attempts = 0;
        while (!ResetHostThreadState())
        {
            if (++attempts == 10000)
            {
                ForkFailure("NativeAOT fork support: host native threads did not finish teardown.\n");
            }

            PalSleep(1);
        }

        s_state.store(ForkState::Dormant);
    }
}

extern "C" int32_t RhTryEnterForkWork()
{
    uint32_t observed = s_work.load();
    do
    {
        if ((observed & WorkRetired) != 0)
        {
            return 0;
        }

        if ((observed & WorkCountMask) == WorkCountMask)
        {
            ForkFailure("NativeAOT fork support: callback accounting overflowed.\n");
        }
    } while (!s_work.compare_exchange_weak(observed, observed + 1));

    return 1;
}

extern "C" void RhExitForkWork()
{
    uint32_t observed = s_work.load();
    uint32_t updated;
    do
    {
        if ((observed & WorkCountMask) == 0 || (observed & WorkRetired) != 0)
        {
            ForkFailure("NativeAOT fork support: callback accounting underflowed.\n");
        }

        updated = observed - 1;
        if (updated == WorkPreparing)
        {
            updated |= WorkRetired;
        }
    } while (!s_work.compare_exchange_weak(observed, updated));
}

extern "C" void RhRequestForkWorkRetirement()
{
    uint32_t observed = s_work.load();
    uint32_t updated;
    do
    {
        updated = observed | WorkPreparing;
        if ((observed & WorkCountMask) == 0)
        {
            updated |= WorkRetired;
        }
    } while (!s_work.compare_exchange_weak(observed, updated));
}

extern "C" __attribute__((visibility("default"))) int32_t RhGetForkWorkState()
{
    uint32_t state = s_work.load();
    return (state & WorkRetired) != 0 ? 2 : (state & WorkPreparing) != 0 ? 1 : 0;
}

extern "C" int32_t RhIsForkWorkRetired()
{
    return (s_work.load() & WorkRetired) != 0;
}

extern "C" void RhResumeForkWork()
{
    uint32_t expected = WorkPreparing | WorkRetired;
    if (!s_work.compare_exchange_strong(expected, 0))
    {
        ForkFailure("NativeAOT fork support: callback resume requires completed retirement.\n");
    }
}

extern "C" __attribute__((visibility("default"))) int32_t RhRegisterForkServiceCallbacks(
    ForkServiceCallback prepare, ForkServiceCallback parent,
    ForkServiceCallback childReset, ForkServiceCallback childResume)
{
    if (ThreadStore::GetCurrentThreadIfAvailable() == nullptr)
    {
        return -1;
    }

    if (prepare == nullptr || parent == nullptr || childReset == nullptr || childResume == nullptr)
    {
        return -8;
    }

    // Publication is permanent. Identical repeated registration may be observed
    // during managed subsystem initialization without replacing active callbacks.
    if (s_servicesRegistered.load())
    {
        return s_prepareServices == prepare && s_resumeParentServices == parent &&
            s_resetChildServices == childReset && s_resumeChildServices == childResume ? 1 : -8;
    }

    ForkState previous = s_state.load();
    if ((previous != ForkState::Disabled && previous != ForkState::Idle) ||
        !s_state.compare_exchange_strong(previous, ForkState::Registering))
    {
        return -2;
    }

    // Another completed registration can precede this state acquisition after
    // the first publication check. Never replace an already published tuple.
    if (s_servicesRegistered.load())
    {
        bool identical = s_prepareServices == prepare && s_resumeParentServices == parent &&
            s_resetChildServices == childReset && s_resumeChildServices == childResume;
        s_state.store(previous);
        return identical ? 1 : -8;
    }

    s_prepareServices = prepare;
    s_resumeParentServices = parent;
    s_resetChildServices = childReset;
    s_resumeChildServices = childResume;
    s_servicesRegistered.store(true);
    s_state.store(previous);
    return 1;
}

extern "C" __attribute__((visibility("default"))) int32_t RhEnableForkSupport()
{
    Thread* caller = ThreadStore::GetCurrentThreadIfAvailable();
    if (caller == nullptr || !caller->IsAtNativeTopOfStackForFork())
    {
        return -1;
    }

    ForkState expected = ForkState::Disabled;
    if (!s_state.compare_exchange_strong(expected, ForkState::Enabling))
    {
        return (expected == ForkState::Idle || expected == ForkState::Dormant) && caller == s_owner ? 1 : -2;
    }

    if (!HasSupportedConfiguration())
    {
        return EnableFailure(-3);
    }

    uint32_t attempts = 0;
    while (s_finalizer.load() == nullptr)
    {
        if (++attempts == 10000)
        {
            return EnableFailure(-4);
        }

        PalSleep(1);
    }

    ThreadStore* store = GetThreadStore();
    store->LockThreadStore();
    // Registered services retire their workers in PrepareFork. Without callbacks,
    // admit only the caller, finalizer, and optional owned background GC worker.
    bool supported = s_servicesRegistered.load() || store->HasOnlyForkThreads(caller, s_finalizer.load(), true);
    store->UnlockThreadStore();
    if (!supported)
    {
        return EnableFailure(-5);
    }

    if (pthread_atfork(PrepareFork, ParentAfterFork, ChildAfterFork) != 0)
    {
        return EnableFailure(-6);
    }

    s_owner = caller;
    s_hostProcess = getpid();
    s_state.store(ForkState::Idle);
    PrepareDormant();
    return 1;
}

extern "C" __attribute__((visibility("default"))) int32_t RhEnterForkHost()
{
    ForkState state = s_state.load();
    if (state == ForkState::Disabled || state == ForkState::Registering || state == ForkState::Enabling ||
        getpid() != s_hostProcess)
    {
        return 1;
    }

    if (ThreadStore::RawGetCurrentThread() != s_owner)
    {
        return -1;
    }

    if (s_hostManagedDepth != 0)
    {
        s_hostManagedDepth++;
        return 1;
    }

    if (state != ForkState::Dormant || !s_owner->IsAtNativeTopOfStackForFork())
    {
        return -2;
    }

    ResumeDormantHost();
    s_hostManagedDepth = 1;
    return 1;
}

extern "C" __attribute__((visibility("default"))) int32_t RhExitForkHost()
{
    ForkState state = s_state.load();
    if (state == ForkState::Disabled || state == ForkState::Registering || state == ForkState::Enabling ||
        getpid() != s_hostProcess)
    {
        return 1;
    }

    if (ThreadStore::RawGetCurrentThread() != s_owner || s_hostManagedDepth == 0)
    {
        return -1;
    }

    s_hostManagedDepth--;
    if (s_hostManagedDepth != 0)
    {
        return 1;
    }

    if (state != ForkState::Idle || !s_owner->IsAtNativeTopOfStackForFork())
    {
        return -2;
    }

    PrepareDormant();
    return 1;
}

void RhForkBeforeManagedEntry()
{
    while (true)
    {
        ForkState state = s_state.load();
        if (state == ForkState::Disabled || state == ForkState::Registering ||
            state == ForkState::Enabling || state == ForkState::Idle ||
            state == ForkState::PreparingManaged || state == ForkState::ParentResuming ||
            state == ForkState::ChildResuming)
        {
            return;
        }

        if (state == ForkState::ChildPending)
        {
            CompleteChildRecovery();
            return;
        }

        if (state == ForkState::ChildRepair && ThreadStore::RawGetCurrentThread() == s_owner)
        {
            return;
        }

        // An in-flight finalizer can perform nested reverse P/Invokes before it
        // reaches the explicit root-free checkpoint. Blocking those would prevent
        // the finalizer from finishing and acknowledging preparation.
        if (state == ForkState::Preparing && ThreadStore::RawGetCurrentThread() == s_finalizer.load())
        {
            return;
        }

        PalSleep(1);
    }
}

bool RhForkFinalizerCheckpoint()
{
    while (s_state.load() == ForkState::Preparing)
    {
        s_acknowledged.store(s_request.load());
        if (s_finalizerExit.load())
        {
            return false;
        }

        PalSleep(1);
    }

    return true;
}

void RhForkRegisterFinalizer(Thread* thread)
{
    s_finalizer.store(thread);
}

void RhForkThreadShutdownStarted()
{
    if (s_threadShutdowns.fetch_add(1) == UINT32_MAX)
    {
        ForkFailure("NativeAOT fork support: thread shutdown accounting overflowed.\n");
    }
}

void RhForkThreadShutdownCompleted()
{
    if (s_finalizerExit.load() && ThreadStore::RawGetCurrentThread() == s_finalizer.load())
    {
        s_finalizer.store(nullptr);
    }

    if (s_threadShutdowns.fetch_sub(1) == 0)
    {
        ForkFailure("NativeAOT fork support: thread shutdown accounting underflowed.\n");
    }
}

bool RhForkIsAdmissionClosed()
{
    ForkState state = s_state.load();
    return state == ForkState::Dormant || state == ForkState::Preparing ||
        state == ForkState::ForkingDormant || state == ForkState::ChildPending ||
        (state == ForkState::ChildRepair && ThreadStore::RawGetCurrentThread() != s_owner);
}

void RhForkRecordBackgroundWorker()
{
    s_backgroundWorkerStarted.store(true);
}

#else

extern "C" int32_t RhTryEnterForkWork() { return 1; }
extern "C" void RhExitForkWork() { }
extern "C" void RhRequestForkWorkRetirement() { }
extern "C" int32_t RhIsForkWorkRetired() { return 1; }
extern "C" int32_t RhGetForkWorkState() { return 0; }
extern "C" void RhResumeForkWork() { }

extern "C" int32_t RhRegisterForkServiceCallbacks(
    ForkServiceCallback, ForkServiceCallback, ForkServiceCallback, ForkServiceCallback)
{
    return -7;
}

extern "C" int32_t RhEnableForkSupport()
{
    return -7;
}

extern "C" int32_t RhEnterForkHost()
{
    return 1;
}

extern "C" int32_t RhExitForkHost()
{
    return 1;
}

void RhForkBeforeManagedEntry()
{
}

bool RhForkFinalizerCheckpoint()
{
    return true;
}

void RhForkRegisterFinalizer(Thread*)
{
}

void RhForkThreadShutdownStarted()
{
}

void RhForkThreadShutdownCompleted()
{
}

bool RhForkIsAdmissionClosed()
{
    return false;
}

void RhForkRecordBackgroundWorker()
{
}

#endif
