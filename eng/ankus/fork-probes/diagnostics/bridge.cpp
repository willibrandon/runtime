// Read the configured socket layout directly; no runtime implementation is replaced.
#include <eventpipe/ds-ipc-pal-socket.h>
#include <fcntl.h>
#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <sys/wait.h>
#include <unistd.h>

static uint32_t connect_error_code;

// Capture the transport error before any cleanup can replace errno.
static void capture_connect_error(const char* message, uint32_t code)
{
    (void) message;
    connect_error_code = code;
}

// Only native exec runs in this child; this does not test managed fork reentry.
static bool descriptor_closes_on_exec(int descriptor)
{
    char number[32];
    std::snprintf(number, sizeof(number), "%d", descriptor);
    pid_t child = fork();
    if (child < 0)
    {
        return false;
    }

    if (child == 0)
    {
        execl("/proc/self/exe", "host", "--check-fd-closed", number, nullptr);
        _exit(79);
    }

    int status;
    pid_t waited;
    do
    {
        waited = waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);

    return waited == child && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

extern bool DiagnosticServer_Shutdown();
extern void ds_ipc_stream_factory_close_ports(ds_ipc_error_callback_func callback);
extern bool ds_server_pause_listener();
extern bool ds_server_resume_listener();
extern uint32_t ds_server_paused_input_bytes();

extern "C" __attribute__((visibility("default"))) uint32_t ankus_probe_listener_checkpoint(bool pause)
{
    if (pause)
    {
        return ds_server_pause_listener() ? ds_server_paused_input_bytes() : UINT32_MAX;
    }

    return ds_server_resume_listener() ? 0 : UINT32_MAX;
}

extern "C" __attribute__((visibility("default"))) bool ankus_probe_diagnostics_shutdown(bool shutdown)
{
    if (shutdown)
    {
        return DiagnosticServer_Shutdown();
    }

    ds_ipc_stream_factory_close_ports(nullptr);
    return true;
}

// Force listen to fail, reuse the released descriptor, then prove free preserves its new owner.
extern "C" __attribute__((visibility("default"))) int ankus_probe_listen_failure(const char* path)
{
    DiagnosticsIpc* ipc = ds_ipc_alloc(path, DS_IPC_CONNECTION_MODE_LISTEN, nullptr);
    if (ipc == nullptr)
    {
        return 1;
    }

    int descriptor = ipc->server_socket;
    int source = open("/dev/null", O_RDONLY | O_CLOEXEC);
    if (source < 0 || dup2(source, descriptor) != descriptor)
    {
        if (source >= 0)
        {
            close(source);
        }

        ds_ipc_free(ipc);
        return 2;
    }

    close(source);
    if (ds_ipc_listen(ipc, nullptr))
    {
        ds_ipc_free(ipc);
        return 3;
    }

    int sentinel = open("/dev/null", O_RDONLY | O_CLOEXEC);
    if (sentinel != descriptor)
    {
        if (sentinel >= 0)
        {
            close(sentinel);
        }

        ds_ipc_free(ipc);
        return 4;
    }

    ds_ipc_free(ipc);
    bool preserved = fcntl(sentinel, F_GETFD) >= 0;
    if (preserved)
    {
        close(sentinel);
    }

    return preserved ? 0 : 5;
}

// Exercise the real socket PAL through a separately controlled Unix socket client.
extern "C" __attribute__((visibility("default"))) int ankus_probe_stream_io(
    const char* path, char direction, uint32_t timeout, uint32_t length)
{
    DiagnosticsIpc* ipc = ds_ipc_alloc(path, DS_IPC_CONNECTION_MODE_LISTEN, nullptr);
    if (ipc == nullptr || !ds_ipc_listen(ipc, nullptr))
    {
        ds_ipc_free(ipc);
        return 1;
    }

    std::puts("io-listening");
    std::fflush(stdout);
    DiagnosticsIpcStream* stream = ds_ipc_accept(ipc, nullptr);
    if (stream == nullptr)
    {
        ds_ipc_free(ipc);
        return 2;
    }

    // Let the peer queue data before a zero-timeout operation, without timing races.
    std::puts("io-connected");
    std::fflush(stdout);
    char start[8];
    if (std::fgets(start, sizeof(start), stdin) == nullptr || start[0] != 'g')
    {
        ds_ipc_stream_free(stream);
        ds_ipc_free(ipc);
        return 4;
    }

    uint8_t* buffer = static_cast<uint8_t*>(std::malloc(length == 0 ? 1 : length));
    if (buffer == nullptr)
    {
        ds_ipc_stream_free(stream);
        ds_ipc_free(ipc);
        return 3;
    }

    for (uint32_t i = 0; i < length; i++)
    {
        buffer[i] = direction == 'w' ? static_cast<uint8_t>(i % 251) : 0;
    }

    uint32_t transferred = 0;
    int descriptor = -1;
    int descriptor_flags = -1;
    int exec_closed = -1;
    bool success;
    if (direction == 'f')
    {
        success = ds_ipc_stream_read_fd(stream, &descriptor);
        if (success)
        {
            descriptor_flags = fcntl(descriptor, F_GETFD);
            exec_closed = descriptor_closes_on_exec(descriptor) ? 1 : 0;
            ssize_t count = pread(descriptor, buffer, length, 0);
            close(descriptor);
            success = count == length;
            transferred = success ? length : 0;
        }
    }
    else
    {
        success = direction == 'w'
            ? ds_ipc_stream_write(stream, buffer, length, &transferred, timeout)
            : ds_ipc_stream_read(stream, buffer, length, &transferred, timeout);
    }

    bool valid = true;
    for (uint32_t i = 0; i < transferred; i++)
    {
        valid = valid && buffer[i] == static_cast<uint8_t>(i % 251);
    }

    std::printf("io-result %d %u %d\n", success ? 1 : 0, transferred, valid ? 1 : 0);
    if (direction == 'f')
    {
        std::printf("io-descriptor %d %d %d\n", descriptor >= 0 ? 1 : 0, descriptor_flags, exec_closed);
    }

    std::fflush(stdout);
    std::free(buffer);
    ds_ipc_stream_free(stream);
    ds_ipc_free(ipc);
    return 0;
}

// Keep the listener and its backlog under the external client's control.
extern "C" __attribute__((visibility("default"))) int ankus_probe_connect(const char* path, uint32_t timeout)
{
    DiagnosticsIpc* ipc = ds_ipc_alloc(path, DS_IPC_CONNECTION_MODE_CONNECT, nullptr);
    if (ipc == nullptr)
    {
        return 1;
    }

    bool timed_out = true;
    connect_error_code = 0;
    DiagnosticsIpcStream* stream = ds_ipc_connect(ipc, timeout, capture_connect_error, &timed_out);
    std::printf("connect-result %d %d %u\n", stream != nullptr ? 1 : 0, timed_out ? 1 : 0, connect_error_code);
    std::fflush(stdout);
    bool valid = true;
    if (stream != nullptr)
    {
        const uint8_t message[] = {0, 1, 2, 127, 128, 254, 255};
        uint32_t written = 0;
        valid = ds_ipc_stream_write(stream, message, sizeof(message), &written, 1000) && written == sizeof(message);
    }

    ds_ipc_stream_free(stream);
    ds_ipc_free(ipc);
    return valid ? 0 : 2;
}
