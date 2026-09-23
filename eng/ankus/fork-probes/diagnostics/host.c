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
typedef int (*eventpipe_checkpoint_fn)(bool);
typedef int32_t (*managed_work_fn)(int32_t);
typedef int (*trace_ownership_fn)(int, int, uint64_t*);
typedef int (*block_sends_fn)(int);
typedef uint64_t (*blocked_send_attempts_fn)(void);
typedef int64_t (*limit_sends_fn)(int64_t);
typedef uint64_t (*constrained_send_bytes_fn)(void);
static volatile sig_atomic_t interrupt_count;
static pid_t managed_child = -1;
static int managed_child_command = -1;
static int managed_child_response = -1;

struct child_result
{
    int32_t pid;
    int32_t result;
};

static bool
write_all(int descriptor, const void *buffer, size_t length)
{
    const uint8_t *cursor = buffer;
    while (length != 0)
    {
        ssize_t count = write(descriptor, cursor, length);
        if (count < 0 && errno == EINTR)
        {
            continue;
        }

        if (count <= 0)
        {
            return false;
        }

        cursor += count;
        length -= (size_t) count;
    }

    return true;
}

static bool
read_all(int descriptor, void *buffer, size_t length)
{
    uint8_t *cursor = buffer;
    while (length != 0)
    {
        ssize_t count = read(descriptor, cursor, length);
        if (count < 0 && errno == EINTR)
        {
            continue;
        }

        if (count <= 0)
        {
            return false;
        }

        cursor += count;
        length -= (size_t) count;
    }

    return true;
}

static void
managed_child_loop(int command_descriptor, int response_descriptor,
                   managed_work_fn managed_work, shutdown_fn shutdown_server,
                   block_sends_fn block_sends)
{
    struct child_result result = {(int32_t) getpid(), managed_work(25)};
    if (block_sends != NULL)
    {
        block_sends(0);
    }

    if (!write_all(response_descriptor, &result, sizeof(result)))
    {
        _exit(80);
    }

    char command;
    while (read_all(command_descriptor, &command, sizeof(command)))
    {
        if (command == 'g')
        {
            result.result = managed_work(50);
            if (!write_all(response_descriptor, &result, sizeof(result)))
            {
                _exit(81);
            }
        }
        else if (command == 'q')
        {
            result.result = shutdown_server(true) ? 0 : 1;
            write_all(response_descriptor, &result, sizeof(result));
            _exit(result.result);
        }
        else
        {
            _exit(82);
        }
    }

    _exit(83);
}

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
    eventpipe_checkpoint_fn eventpipe_checkpoint = (eventpipe_checkpoint_fn) dlsym(library, "ankus_probe_eventpipe_checkpoint");
    managed_work_fn managed_work = (managed_work_fn) dlsym(library, "fork_probe_managed_work");
    if (initialize == NULL || shutdown_server == NULL || listen_failure == NULL || enable == NULL || initialize(getpid(), report) != 0)
    {
        return 71;
    }

    trace_ownership_fn trace_ownership = (trace_ownership_fn) dlsym(library, "ankus_probe_trace_ownership");
    block_sends_fn block_sends = (block_sends_fn) dlsym(library, "ankus_probe_block_sends");
    blocked_send_attempts_fn blocked_send_attempts =
        (blocked_send_attempts_fn) dlsym(library, "ankus_probe_blocked_send_attempts");
    limit_sends_fn limit_sends = (limit_sends_fn) dlsym(library, "ankus_probe_limit_sends");
    constrained_send_bytes_fn constrained_send_bytes =
        (constrained_send_bytes_fn) dlsym(library, "ankus_probe_constrained_send_bytes");

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
        else if (command[0] == 'z')
        {
            int commands[2];
            int responses[2];
            if (managed_child != -1 || managed_work == NULL ||
                pipe(commands) != 0 || pipe(responses) != 0)
            {
                return 78;
            }

            pid_t child = fork();
            if (child < 0)
            {
                return 72;
            }

            if (child == 0)
            {
                close(commands[1]);
                close(responses[0]);
                managed_child_loop(commands[0], responses[1], managed_work, shutdown_server, block_sends);
            }

            close(commands[0]);
            close(responses[1]);
            struct child_result result;
            if (!read_all(responses[0], &result, sizeof(result)))
            {
                return 79;
            }

            managed_child = child;
            managed_child_command = commands[1];
            managed_child_response = responses[0];
            printf("fork-child %d %d\n", result.pid, result.result);
        }
        else if (command[0] == 'h')
        {
            struct child_result result;
            char work = 'g';
            if (managed_child == -1 || !write_all(managed_child_command, &work, sizeof(work)) ||
                !read_all(managed_child_response, &result, sizeof(result)))
            {
                int status = 0;
                pid_t waited = managed_child == -1 ? -1 : waitpid(managed_child, &status, 0);
                fprintf(stderr, "child-response-failed pid=%d waited=%d status=%d signal=%d exit=%d\n",
                        (int) managed_child, (int) waited, status,
                        waited > 0 && WIFSIGNALED(status) ? WTERMSIG(status) : 0,
                        waited > 0 && WIFEXITED(status) ? WEXITSTATUS(status) : -1);
                return 79;
            }

            printf("child-work %d %d\n", result.pid, result.result);
        }
        else if (command[0] == 'j')
        {
            struct child_result result;
            char stop = 'q';
            if (managed_child == -1 || !write_all(managed_child_command, &stop, sizeof(stop)) ||
                !read_all(managed_child_response, &result, sizeof(result)))
            {
                return 79;
            }

            int status;
            pid_t waited;
            do
            {
                waited = waitpid(managed_child, &status, 0);
            } while (waited < 0 && errno == EINTR);

            pid_t child = managed_child;
            close(managed_child_command);
            close(managed_child_response);
            managed_child = -1;
            managed_child_command = -1;
            managed_child_response = -1;
            int exit_code = waited == child && WIFEXITED(status) ? WEXITSTATUS(status) : -1;
            printf("child-stopped %d %d %d\n", result.pid, result.result, exit_code);
        }
        else if (command[0] == 'f')
        {
            int pause;
            if (eventpipe_checkpoint == NULL || sscanf(command + 1, "%d", &pause) != 1)
            {
                return 76;
            }

            printf("fork-checkpoint %d\n", eventpipe_checkpoint(pause != 0));
        }
        else if (command[0] == 'g')
        {
            int milliseconds;
            if (managed_work == NULL || sscanf(command + 1, "%d", &milliseconds) != 1)
            {
                return 76;
            }

            printf("managed-work %d\n", managed_work(milliseconds));
        }
        else if (command[0] == 's')
        {
            printf("shutdown %d\n", shutdown_server(true) ? 1 : 0);
        }
        else if (command[0] == 'q')
        {
            if (managed_child != -1)
            {
                char stop = 'q';
                struct child_result result;
                write_all(managed_child_command, &stop, sizeof(stop));
                read_all(managed_child_response, &result, sizeof(result));
                waitpid(managed_child, NULL, 0);
            }

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
        else if (command[0] == 'w')
        {
            int block;
            if (block_sends == NULL || sscanf(command + 1, "%d", &block) != 1)
            {
                return 76;
            }

            printf("send-blocked %d\n", block_sends(block));
        }
        else if (command[0] == 'x')
        {
            if (blocked_send_attempts == NULL || constrained_send_bytes == NULL)
            {
                return 76;
            }

            printf("blocked-sends %llu constrained-bytes %llu\n",
                   (unsigned long long) blocked_send_attempts(),
                   (unsigned long long) constrained_send_bytes());
        }
        else if (command[0] == 'y')
        {
            long long bytes;
            if (limit_sends == NULL || sscanf(command + 1, "%lld", &bytes) != 1 || bytes < 0)
            {
                return 77;
            }

            printf("send-limit %lld\n", (long long) limit_sends((int64_t) bytes));
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
