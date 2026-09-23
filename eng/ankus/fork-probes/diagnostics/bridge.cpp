// Read the configured socket layout directly; no runtime implementation is replaced.
#include <eventpipe/ds-ipc-pal-socket.h>
#include <fcntl.h>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>

extern bool DiagnosticServer_Shutdown();
extern void ds_ipc_stream_factory_close_ports(ds_ipc_error_callback_func callback);

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
    bool success;
    if (direction == 'f')
    {
        int descriptor = -1;
        success = ds_ipc_stream_read_fd(stream, &descriptor);
        if (success)
        {
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
    std::fflush(stdout);
    std::free(buffer);
    ds_ipc_stream_free(stream);
    ds_ipc_free(ipc);
    return 0;
}
