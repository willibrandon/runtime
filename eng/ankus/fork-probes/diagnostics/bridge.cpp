// Read the configured socket layout directly; no runtime implementation is replaced.
#include <eventpipe/ds-ipc-pal-socket.h>
#include <fcntl.h>
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
