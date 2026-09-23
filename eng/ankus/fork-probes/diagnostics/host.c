#include <dlfcn.h>
#include <errno.h>
#include <malloc.h>
#include <fcntl.h>
#include <stdbool.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

typedef void (*report_fn)(int32_t, int64_t);
typedef int32_t (*initialize_fn)(int32_t, report_fn);
typedef bool (*shutdown_fn)(bool);
typedef int (*listen_failure_fn)(const char*);
typedef int32_t (*enable_fn)(void);
typedef int (*stream_io_fn)(const char*, char, uint32_t, uint32_t);
typedef int (*connect_fn)(const char*, uint32_t);
typedef uint32_t (*listener_fn)(bool);
typedef uint64_t (*pending_response_fn)(void);
typedef int (*trace_ownership_fn)(int, int, uint64_t*);
static volatile sig_atomic_t interrupt_count;

/* Interrupt blocking system calls without terminating the probe. */
static void
interrupt_io(int signal_number)
{
    (void) signal_number;
    interrupt_count++;
}

/* Initialization callbacks have no retained native state. */
static void
report(int32_t marker, int64_t value)
{
    (void) marker;
    (void) value;
}

/* The external client owns timing and process-group cleanup. */
int
main(int argc, char **argv)
{
    if (argc == 3 && strcmp(argv[1], "--check-fd-closed") == 0)
    {
        char* end;
        long descriptor = strtol(argv[2], &end, 10);
        if (*end != '\0' || descriptor < 0 || descriptor > INT32_MAX)
        {
            return 78;
        }

        return fcntl((int) descriptor, F_GETFD) == -1 && errno == EBADF ? 0 : 79;
    }

    if (argc != 2)
    {
        return 64;
    }

    void *library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (library == NULL)
    {
        fprintf(stderr, "%s\n", dlerror());
        return 70;
    }

    initialize_fn initialize = (initialize_fn) dlsym(library, "fork_probe_initialize");
    shutdown_fn shutdown_server = (shutdown_fn) dlsym(library, "ankus_probe_diagnostics_shutdown");
    listen_failure_fn listen_failure = (listen_failure_fn) dlsym(library, "ankus_probe_listen_failure");
    enable_fn enable = (enable_fn) dlsym(library, "RhEnableForkSupport");
    stream_io_fn stream_io = (stream_io_fn) dlsym(library, "ankus_probe_stream_io");
    connect_fn connect_client = (connect_fn) dlsym(library, "ankus_probe_connect");
    listener_fn listener_checkpoint = (listener_fn) dlsym(library, "ankus_probe_listener_checkpoint");
    pending_response_fn pending_response = (pending_response_fn) dlsym(library, "ankus_probe_pending_response");
    if (initialize == NULL || shutdown_server == NULL || listen_failure == NULL || enable == NULL || initialize(getpid(), report) != 0)
    {
        return 71;
    }

    trace_ownership_fn trace_ownership = (trace_ownership_fn) dlsym(library, "ankus_probe_trace_ownership");

    printf("ready %d\n", (int) getpid());
    fflush(stdout);
    char command[256];
    while (fgets(command, sizeof(command), stdin) != NULL)
    {
        if (command[0] == 'c' || command[0] == 'n')
        {
            pid_t child = fork();
            if (child < 0)
            {
                return 72;
            }

            if (child == 0)
            {
                // Only native endpoint teardown runs in this child. No managed reentry,
                // EventPipe session teardown, or general fork support is claimed.
                _exit(shutdown_server(command[0] == 'c') ? 0 : 1);
            }

            int status = 0;
            pid_t waited;
            do
            {
                waited = waitpid(child, &status, 0);
            } while (waited < 0 && errno == EINTR);

            if (waited != child || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
            {
                return 73;
            }

            printf("child-closed %d\n", (int) child);
        }
        else if (command[0] == 'l' && command[1] == ' ')
        {
            command[strcspn(command, "\n")] = '\0';
            printf("listen-failure %d\n", listen_failure(command + 2));
        }
        else if (command[0] == 'e')
        {
            printf("enable %d\n", enable());
        }
        else if (command[0] == 's')
        {
            printf("shutdown %d\n", shutdown_server(true) ? 1 : 0);
        }
        else if (command[0] == 'q')
        {
            return 0;
        }
        else if (command[0] == 'p' || command[0] == 'r')
        {
            if (listener_checkpoint == NULL)
            {
                return 76;
            }

            printf("listener %u\n", listener_checkpoint(command[0] == 'p'));
        }
        else if (command[0] == 'o')
        {
            if (pending_response == NULL)
            {
                return 76;
            }

            printf("pending %llu\n", (unsigned long long) pending_response());
        }
        else if (command[0] == 'a')
        {
            struct mallinfo2 allocation = mallinfo2();
            printf("allocated %zu\n", allocation.uordblks);
        }
        else if (command[0] == 'm')
        {
            printf("changed %d\n", setenv("ANKUS_RESPONSE_CHANGE", "after", 1));
        }
        else if (command[0] == 'v')
        {
            const char* value = getenv("ANKUS_LISTENER_CHECKPOINT");
            printf("environment %s\n", value == NULL ? "<unset>" : value);
        }
        else if (command[0] == 'k')
        {
            char path[192];
            uint32_t timeout;
            if (connect_client == NULL || sscanf(command + 1, "%u %191s", &timeout, path) != 2)
            {
                return 75;
            }

            printf("connect-complete %d\n", connect_client(path, timeout));
        }
        else if (command[0] == 'b')
        {
            int kind;
            int failure_index;
            uint64_t observations[6] = {0};
            if (trace_ownership == NULL || sscanf(command + 1, "%d %d", &kind, &failure_index) != 2)
            {
                return 76;
            }

            int result = trace_ownership(kind, failure_index, observations);
            printf("ownership %d", result);
            for (int index = 0; index < 6; index++)
            {
                printf(" %llu", (unsigned long long) observations[index]);
            }

            printf("\n");
        }
        else if (command[0] == 'i')
        {
            char path[192];
            char direction;
            uint32_t timeout;
            uint32_t length;
            struct sigaction action = {0};
            action.sa_handler = interrupt_io;
            sigemptyset(&action.sa_mask);
            if (stream_io == NULL || sigaction(SIGALRM, &action, NULL) != 0 ||
                sscanf(command + 1, "%c %u %u %191s", &direction, &timeout, &length, path) != 4)
            {
                return 74;
            }

            printf("io-complete %d\n", stream_io(path, direction, timeout, length));
            printf("io-signals %d\n", (int) interrupt_count);
        }
        else
        {
            return 64;
        }

        fflush(stdout);
    }

    return 0;
}
