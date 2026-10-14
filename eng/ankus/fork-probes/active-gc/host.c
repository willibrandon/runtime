#include <dlfcn.h>
#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* Fixed-width signatures match the Cdecl UnmanagedCallersOnly exports. */
typedef void (*report_fn)(int32_t marker, int64_t value);
typedef int32_t (*initialize_fn)(int32_t native_pid, report_fn report);
typedef int64_t (*snapshot_fn)(int32_t field);
typedef int32_t (*run_fn)(int32_t stage, int32_t native_pid, int32_t parent_pid,
                         int64_t token_low, int64_t token_high, report_fn report);
typedef int32_t (*enable_fn)(void);
typedef uint64_t (*observation_fn)(void);

/* Host settings never change managed runtime configuration or the process environment. */
struct options
{
    const char *library;
    bool minimal;
    bool enable;
    bool expect_server_gc;
    int rounds;
};

/* JSON output uses bounded stack storage and write, without inherited stdio buffers. */
struct output_line
{
    char data[1024];
    size_t length;
};

/* These native-only labels are copied into children and identify interrupted stages. */
static const char *current_role = "supervisor";
static const char *current_stage = "startup";
static int current_round = 0;
static const char *stage_names[] = {
    "invalid", "graph", "pid", "gc-finalizer", "thread-pool", "timer", "exception",
    "active-heap-initialize", "active-request", "active-completed", "active-heap-recheck", "blocking-or-completed-request",
    "adopt-fork-parent"
};

/* Appends trusted fixed labels into the bounded JSON record. */
static void
append_text(struct output_line *line, const char *value)
{
    while (*value != '\0' && line->length < sizeof(line->data))
    {
        line->data[line->length++] = *value++;
    }
}

/* Formats all signed 64-bit values, including the two nondeterministic token words. */
static void
append_number(struct output_line *line, int64_t value)
{
    char digits[20];
    size_t count = 0;
    uint64_t magnitude;
    if (value < 0)
    {
        append_text(line, "-");
        magnitude = (uint64_t) (-(value + 1)) + 1;
    }
    else
    {
        magnitude = (uint64_t) value;
    }

    do
    {
        digits[count++] = (char) ('0' + magnitude % 10);
        magnitude /= 10;
    } while (magnitude != 0);

    while (count > 0 && line->length < sizeof(line->data))
    {
        line->data[line->length++] = digits[--count];
    }
}

/* Emits one complete machine-readable record without invoking managed code. */
static void
emit(const char *event, int32_t marker, int64_t value)
{
    struct output_line line = {{0}, 0};
    append_text(&line, "{\"role\":\"");
    append_text(&line, current_role);
    append_text(&line, "\",\"stage\":\"");
    append_text(&line, current_stage);
    append_text(&line, "\",\"event\":\"");
    append_text(&line, event);
    append_text(&line, "\",\"round\":");
    append_number(&line, current_round);
    append_text(&line, ",\"pid\":");
    append_number(&line, (int64_t) getpid());
    append_text(&line, ",\"marker\":");
    append_number(&line, marker);
    append_text(&line, ",\"value\":");
    append_number(&line, value);
    append_text(&line, "}\n");
    size_t written = 0;
    while (written < line.length)
    {
        ssize_t result = write(STDOUT_FILENO, line.data + written, line.length - written);
        if (result < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }

            _exit(74);
        }

        if (result == 0)
        {
            _exit(74);
        }

        written += (size_t) result;
    }
}

#include "../collector-inventory.h"

/* The synchronous managed callback never retains this process-lifetime native function pointer. */
static void
report(int32_t marker, int64_t value)
{
    emit("managed-marker", marker, value);
}

/* Reads the supervisor's monotonic clock without any managed timing services. */
static int64_t
milliseconds(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
    {
        emit("clock-error", 0, errno);
        _exit(70);
    }

    return (int64_t) now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

/* Reaps one child with a deadline; only the outer supervisor kills the complete worker group. */
static int
wait_bounded(pid_t child, int seconds, bool group)
{
    int status = 0;
    int64_t deadline = milliseconds() + seconds * 1000;
    for (;;)
    {
        pid_t result = waitpid(child, &status, WNOHANG);
        if (result == child)
        {
            if (WIFEXITED(status))
            {
                return WEXITSTATUS(status);
            }

            return WIFSIGNALED(status) ? 128 + WTERMSIG(status) : 70;
        }

        if (result < 0 && errno != EINTR)
        {
            emit("wait-error", 0, errno);
            return 70;
        }

        if (milliseconds() >= deadline)
        {
            emit("timeout", 0, child);
            if (kill(group ? -child : child, SIGKILL) != 0 && errno != ESRCH)
            {
                emit("kill-error", 0, errno);
                return 70;
            }

            int64_t reap_deadline = milliseconds() + 2000;
            while (waitpid(child, &status, WNOHANG) != child)
            {
                if (milliseconds() >= reap_deadline)
                {
                    emit("reap-timeout", 0, child);
                    return 125;
                }

                struct timespec pause = {0, 10000000};
                nanosleep(&pause, NULL);
            }

            return 124;
        }

        struct timespec pause = {0, 10000000};
        nanosleep(&pause, NULL);
    }
}

/* Labels and runs one managed observation, returning to a native frame before any subsequent fork. */
static int
invoke(run_fn run, int stage, pid_t parent, int64_t token_low, int64_t token_high, const char *role)
{
    current_role = role;
    current_stage = stage_names[stage];
    emit("begin", 0, stage);
    int result = run(stage, (int32_t) getpid(), (int32_t) parent, token_low, token_high, report);
    emit("result", 0, result);
    if (!check_collector_inventory())
    {
        return 183;
    }

    return result;
}

/* Loads once, warms managed services, then forks from native code with no managed frame on the calling stack. */
static int
run_worker(struct options options)
{
    current_role = "parent";
    current_stage = "load";
    emit("begin", 0, options.minimal ? 1 : 0);
    void *library = dlopen(options.library, RTLD_NOW | RTLD_LOCAL);
    if (library == NULL)
    {
        emit("dlopen-error", 0, 1);
        return 70;
    }

    initialize_fn initialize = (initialize_fn) dlsym(library, "fork_probe_initialize");
    snapshot_fn snapshot = (snapshot_fn) dlsym(library, "fork_probe_snapshot");
    run_fn run = (run_fn) dlsym(library, "fork_probe_run");
    if (initialize == NULL || snapshot == NULL || run == NULL)
    {
        emit("missing-export", 0, 1);
        return 70;
    }

    pid_t parent = getpid();
    current_stage = "initialize";
    int initialized = initialize((int32_t) parent, report);
    emit("result", 0, initialized);
    if (initialized != 0)
    {
        return 2;
    }

    emit("server-gc", 0, snapshot(4));
    if (options.expect_server_gc && snapshot(4) != 1)
    {
        return 4;
    }

    int64_t token_low = snapshot(1);
    int64_t token_high = snapshot(2);
    emit("snapshot-pid", 0, snapshot(0));
    emit("snapshot-token-low", 0, token_low);
    emit("snapshot-token-high", 0, token_high);
    emit("snapshot-initializations", 0, snapshot(3));
    int warm_stages = options.minimal ? 3 : 6;
    for (int stage = 1; stage <= warm_stages; stage++)
    {
        if (invoke(run, stage, parent, token_low, token_high, "parent-warm") != 0)
        {
            return 2;
        }
    }

    if (options.enable &&
        (!initialize_collector_inventory(library, snapshot(4) == 1) || !check_collector_inventory()))
    {
        return 180;
    }

    if (options.enable)
    {
        current_role = "parent";
        current_stage = "enable-fork";
        enable_fn enable = (enable_fn) dlsym(library, "RhEnableForkSupport");
        if (enable == NULL)
        {
            emit("missing-export", 0, 1);
            return 3;
        }

        int enabled = enable();
        emit("result", 0, enabled);
        if (enabled != 1)
        {
            return 3;
        }

        current_stage = "backend-fork";
        pid_t backend = fork();
        if (backend < 0)
        {
            emit("fork-error", 0, errno);
            return 70;
        }

        if (backend != 0)
        {
            int result = wait_bounded(backend, 180, false);
            emit("backend-status", 0, result);
            return result;
        }

        current_role = "backend";
        if (invoke(run, 12, parent, token_low, token_high, "backend-adopt") != 0)
        {
            return 2;
        }

        parent = getpid();
    }

    observation_fn observations = (observation_fn) dlsym(library, "RhGetForkGCActiveObservationCount");
    if (!options.enable || observations == NULL)
    {
        emit("missing-active-capability", 0, 1);
        return 3;
    }

    if (invoke(run, 7, parent, token_low, token_high, "parent-setup") != 0)
    {
        return 2;
    }

    int failures = 0;
    int qualified_rounds = 0;
    for (int round = 1; round <= options.rounds; round++)
    {
        current_round = round;
        bool qualified = false;
        for (int attempt = 1; attempt <= 8; attempt++)
        {
            current_role = "parent";
            current_stage = "active-request";
            emit("attempt", 0, attempt);
            uint64_t before = observations();
            emit("active-count-before", 0, (int64_t) before);
            int requested = run(8, (int32_t) getpid(), (int32_t) parent, token_low, token_high, report);
            if (requested != 0)
            {
                emit("request-failure", 0, requested);
                return 2;
            }

            // No reporting or managed calls intervene between this request and native fork.
            pid_t child = fork();
            if (child < 0)
            {
                emit("fork-error", 0, errno);
                return 70;
            }

            uint64_t after = observations();
            uint64_t observed = after - before;
            if (child == 0)
            {
                current_role = "child";
                current_stage = "active-checkpoint";
                emit("active-count-after", 0, (int64_t) after);
                emit("active-observations", 0, (int64_t) observed);
                if (observed > 1)
                {
                    _exit(1);
                }

                // These checks share one child: inherited integrity, new BGC, rechecked data,
                // then fresh callbacks. Stage three performs the sole child graph mutation.
                const int child_stages[] = {observed == 1 ? 9 : 11, 3, 10, 2, 4, 5, 6};
                for (size_t index = 0; index < sizeof(child_stages) / sizeof(child_stages[0]); index++)
                {
                    if (invoke(run, child_stages[index], parent, token_low, token_high, "child") != 0)
                    {
                        _exit(1);
                    }
                }

                _exit(0);
            }

            current_stage = "active-checkpoint";
            emit("active-count-after", 0, (int64_t) after);
            emit("active-observations", 0, (int64_t) observed);
            if (observed > 1)
            {
                failures++;
            }

            int result = wait_bounded(child, 20, false);
            emit("child-status", 0, result);
            if (result != 0)
            {
                failures++;
            }

            const int parent_stages[] = {observed == 1 ? 9 : 11, 3, 10, 2, 4, 5, 6};
            for (size_t index = 0; index < sizeof(parent_stages) / sizeof(parent_stages[0]); index++)
            {
                int stage = parent_stages[index];
                if (options.minimal && (stage == 4 || stage == 5))
                {
                    continue;
                }

                if (invoke(run, stage, parent, token_low, token_high, "parent-after-child") != 0)
                {
                    failures++;
                }
            }

            current_stage = "active-checkpoint";
            emit("qualified-attempt", 0, observed == 1 ? 1 : 0);
            if (failures != 0)
            {
                return 1;
            }

            if (observed == 1)
            {
                qualified = true;
                qualified_rounds++;
                break;
            }
        }

        if (!qualified)
        {
            emit("no-active-overlap", 0, round);
            return 4;
        }
    }

    current_stage = "active-checkpoint";
    emit("qualified-rounds", 0, qualified_rounds);
    if (qualified_rounds != options.rounds)
    {
        return 4;
    }

    // Once all forks are finished, check every parent facility even in minimal mode.
    for (int stage = 1; stage <= 6; stage++)
    {
        if (invoke(run, stage, parent, token_low, token_high, "parent-final") != 0)
        {
            failures++;
        }
    }

    if (invoke(run, 10, parent, token_low, token_high, "parent-final") != 0)
    {
        failures++;
    }

    current_role = "parent";
    current_stage = "summary";
    emit("failures", 0, failures);
    // Do not unload a Native AOT library; the native caller exits the worker with _exit.
    return failures == 0 ? 0 : 1;
}

/* Supervises the entire managed parent, so a pre-fork warm-up or parent-after-child hang is also bounded. */
int
main(int argc, char **argv)
{
    if (argc < 2)
    {
        emit("usage-error", 0, 64);
        return 64;
    }

    struct options options = {argv[1], false, false, false, 2};
    for (int argument = 2; argument < argc; argument++)
    {
        if (strcmp(argv[argument], "--minimal") == 0)
        {
            options.minimal = true;
        }
        else if (strcmp(argv[argument], "--server-gc") == 0)
        {
            options.expect_server_gc = true;
        }
        else if (strcmp(argv[argument], "--enable-fork") == 0)
        {
            options.enable = true;
        }
        else
        {
            emit("usage-error", 0, 64);
            return 64;
        }
    }

    pid_t worker = fork();
    if (worker < 0)
    {
        emit("fork-error", 0, errno);
        return 70;
    }

    if (worker == 0)
    {
        if (setpgid(0, 0) != 0)
        {
            emit("setpgid-error", 0, errno);
            _exit(70);
        }

        _exit(run_worker(options));
    }

    if (setpgid(worker, worker) != 0 && errno != EACCES && errno != ESRCH)
    {
        emit("setpgid-error", 0, errno);
        kill(worker, SIGKILL);
        return 70;
    }

    int result = wait_bounded(worker, 180, true);
    current_stage = "summary";
    emit("worker-status", 0, result);
    return result;
}
