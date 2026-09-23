#include <dlfcn.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

typedef void (*report_fn)(int32_t, int64_t);
typedef int32_t (*initialize_fn)(int32_t, report_fn);
typedef bool (*shutdown_fn)(bool);
typedef int (*listen_failure_fn)(const char*);
typedef int32_t (*enable_fn)(void);

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
    if (initialize == NULL || shutdown_server == NULL || listen_failure == NULL || enable == NULL || initialize(getpid(), report) != 0)
    {
        return 71;
    }

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
        else
        {
            return 64;
        }

        fflush(stdout);
    }

    return 0;
}
