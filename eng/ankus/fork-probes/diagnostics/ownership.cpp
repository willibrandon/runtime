// Allocation failures are confined to the calling thread and explicitly enabled
// only around the operation under test. Every other allocation uses libc/C++.
#include <eventpipe/ep.h>
#include <eventpipe/ep-file.h>
#include <eventpipe/ep-ipc-stream.h>
#include <dlfcn.h>
#include <new>
#include <cstdlib>

static thread_local bool observe_allocations;
static thread_local int fail_index;
static thread_local uint64_t allocations;
static thread_local uint64_t failures;
static thread_local uint64_t failure_offset;

static bool fail_allocation(void* return_address)
{
    if (!observe_allocations)
    {
        return false;
    }

    if (static_cast<int>(allocations++) != fail_index)
    {
        return false;
    }

    failures++;
    Dl_info image = {};
    if (dladdr(return_address, &image) != 0)
    {
        failure_offset = static_cast<uint64_t>(static_cast<uint8_t*>(return_address) -
                                               static_cast<uint8_t*>(image.dli_fbase));
    }

    return true;
}

extern "C" void* __real__ZnwmRKSt9nothrow_t(size_t size, const std::nothrow_t& tag) noexcept;
extern "C" void* __real__ZnamRKSt9nothrow_t(size_t size, const std::nothrow_t& tag) noexcept;
extern "C" void* __real_calloc(size_t count, size_t size) noexcept;
extern "C" void* __real_malloc(size_t size) noexcept;
extern "C" void* __real_realloc(void* memory, size_t size) noexcept;
extern "C" char* __real_strdup(const char* value) noexcept;

extern "C" void* __wrap_malloc(size_t size) noexcept
{
    return fail_allocation(__builtin_return_address(0)) ? nullptr : __real_malloc(size);
}

extern "C" void* __wrap_realloc(void* memory, size_t size) noexcept
{
    return fail_allocation(__builtin_return_address(0)) ? nullptr : __real_realloc(memory, size);
}

extern "C" char* __wrap_strdup(const char* value) noexcept
{
    return fail_allocation(__builtin_return_address(0)) ? nullptr : __real_strdup(value);
}

extern "C" void* __wrap__ZnwmRKSt9nothrow_t(size_t size, const std::nothrow_t& tag) noexcept
{
    return fail_allocation(__builtin_return_address(0)) ? nullptr : __real__ZnwmRKSt9nothrow_t(size, tag);
}

extern "C" void* __wrap__ZnamRKSt9nothrow_t(size_t size, const std::nothrow_t& tag) noexcept
{
    return fail_allocation(__builtin_return_address(0)) ? nullptr : __real__ZnamRKSt9nothrow_t(size, tag);
}

extern "C" void* __wrap_calloc(size_t count, size_t size) noexcept
{
    return fail_allocation(__builtin_return_address(0)) ? nullptr : __real_calloc(count, size);
}

struct ObservedWriter
{
    StreamWriter base;
    uint64_t frees;
};

static void free_writer(void* writer)
{
    static_cast<ObservedWriter*>(writer)->frees++;
}

static bool write_bytes(void* writer, const uint8_t* bytes, uint32_t length, uint32_t* written)
{
    (void) writer;
    (void) bytes;
    *written = length;
    return true;
}

struct ObservedStream
{
    IpcStream base;
    uint64_t frees;
};

static void free_stream(void* stream)
{
    static_cast<ObservedStream*>(stream)->frees++;
}

static bool flush_stream(void* stream)
{
    (void) stream;
    return true;
}

// Results: allocations, injected failures, returned success, releases before
// caller cleanup, releases afterward. A stack-backed stream exposes double
// release without hiding it behind allocator-dependent crashes.
extern "C" __attribute__((visibility("default"))) int ankus_probe_trace_ownership(
    int kind, int failure_index, uint64_t* result)
{
    if (kind < 0 || kind > 2 || result == nullptr)
    {
        return 1;
    }

    allocations = 0;
    failures = 0;
    failure_offset = 0;
    fail_index = failure_index;
    if (kind < 2)
    {
        StreamWriterVtable callbacks = {free_writer, write_bytes};
        ObservedWriter writer = {};
        ep_stream_writer_init(&writer.base, &callbacks);
        EventPipeFile* file = nullptr;
        if (kind == 1)
        {
            file = ep_file_alloc(&writer.base, EP_SERIALIZATION_FORMAT_NETTRACE_V4);
            if (file == nullptr)
            {
                return 2;
            }
        }

        observe_allocations = true;
        if (kind == 0)
        {
            file = ep_file_alloc(&writer.base, EP_SERIALIZATION_FORMAT_NETTRACE_V4);
            result[2] = file != nullptr;
        }
        else
        {
            result[2] = ep_file_initialize_file(file);
        }

        observe_allocations = false;
        result[3] = writer.frees;
        if (file != nullptr)
        {
            ep_file_free(file);
        }
        else
        {
            ep_stream_writer_free_vcall(&writer.base);
        }

        result[4] = writer.frees;
    }
    else
    {
        IpcStreamVtable callbacks = {free_stream, nullptr, nullptr, flush_stream, flush_stream, nullptr};
        ObservedStream stream = {};
        ep_ipc_stream_init(&stream.base, &callbacks);
        EventPipeProviderConfiguration provider = {};
        ep_provider_config_init(&provider, "Ankus-Ownership-Probe", 0, EP_EVENT_LEVEL_VERBOSE, nullptr);
        EventPipeSessionOptions options;
        ep_session_options_init(&options, nullptr, 1, &provider, 1, EP_SESSION_TYPE_IPCSTREAM,
                                EP_SERIALIZATION_FORMAT_NETTRACE_V4, 0, false, &stream.base,
                                nullptr, nullptr, -1);
        observe_allocations = true;
        EventPipeSessionID session = ep_enable_3(&options);
        observe_allocations = false;
        result[2] = session != 0;
        result[3] = stream.frees;
        if (session != 0)
        {
            ep_disable(session);
        }
        else
        {
            ep_ipc_stream_free_vcall(&stream.base);
        }

        result[4] = stream.frees;
        ep_session_options_fini(&options);
        ep_provider_config_fini(&provider);
    }

    result[0] = allocations;
    result[1] = failures;
    result[5] = failure_offset;
    return 0;
}
