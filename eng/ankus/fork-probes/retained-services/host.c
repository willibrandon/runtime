#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
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
typedef int32_t (*lineage_fn)(int32_t native_pid, int32_t parent_pid,
                             int64_t token_low, int64_t token_high, int32_t expected,
                             int32_t replacement, report_fn report);
typedef int64_t (*control_fn)(int32_t command, int32_t index, int64_t value);
typedef int32_t (*retained_prepare_fn)(int32_t mode, int32_t round, control_fn control, report_fn report);
typedef int32_t (*retained_run_fn)(int32_t mode, int32_t round, int32_t native_pid,
                                 int32_t parent_pid, report_fn report);

/* Atfork observations never fall back to a hidden atomic lock in a copied process. */
_Static_assert(ATOMIC_INT_LOCK_FREE == 2, "The retained-service witness requires lock-free int atomics.");

enum retained_mode
{
    RETAINED_NONE = 0,
    RETAINED_TIMER = 1,
    RETAINED_QUEUE = 2
};

enum retained_size
{
    RETAINED_GLOBAL_COUNT = 16,
    RETAINED_ITEM_COUNT = 80
};

/* Command numbers match the managed callback's fixed-width protocol. */
enum retained_command
{
    RETAINED_NOW = 1,
    RETAINED_TIMER_FIRED = 2,
    RETAINED_WORKER_READY = 3,
    RETAINED_WAIT_RELEASE = 4,
    RETAINED_ITEM_DONE = 5,
    RETAINED_CURRENT_COUNT = 6,
    RETAINED_SNAPSHOT_COUNT = 7,
    RETAINED_SNAPSHOT_VALID = 8,
    RETAINED_READ_READY = 9,
    RETAINED_SET_DEADLINE = 10,
    RETAINED_SET_TOKEN = 11,
    RETAINED_READ_TOKEN = 12
};

/* Host settings never change managed runtime configuration or the process environment. */
struct options
{
    const char *library;
    bool minimal;
    bool enable;
    int rounds;
    enum retained_mode retained;
    int descendants;
};

/* All storage is allocated before fork; callback counts become independent child copies. */
static struct
{
    atomic_int counts[RETAINED_ITEM_COUNT];
    atomic_int ready;
    atomic_int release;
    int snapshot_counts[RETAINED_ITEM_COUNT];
    int pending_global;
    int pending_local;
    int snapshot_valid;
    int64_t snapshot_time;
    int64_t timer_deadline;
    int64_t token[2];
    enum retained_mode mode;
    bool armed;
} retained;

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
static const char *stage_names[] = {"invalid", "graph", "pid", "gc-finalizer", "thread-pool", "timer", "exception"};

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

/* Sleeps only on a native thread already executing the probe callback, with an elapsed-time bound. */
static bool
pause_milliseconds(int duration)
{
    struct timespec pause = {duration / 1000, (duration % 1000) * 1000000L};
    while (nanosleep(&pause, &pause) != 0)
    {
        if (errno != EINTR)
        {
            return false;
        }
    }

    return true;
}

/* Managed callbacks use this native-only ABI; it performs no PostgreSQL calls or managed reentry. */
static int64_t
retained_control(int32_t command, int32_t index, int64_t value)
{
    switch (command)
    {
        case RETAINED_NOW:
            return milliseconds();

        case RETAINED_TIMER_FIRED:
            return atomic_fetch_add_explicit(&retained.counts[0], 1, memory_order_acq_rel) + 1;

        case RETAINED_WORKER_READY:
            atomic_store_explicit(&retained.ready, 1, memory_order_release);
            return 1;

        case RETAINED_WAIT_RELEASE:
        {
            int64_t deadline = milliseconds() + 4000;
            while (atomic_load_explicit(&retained.release, memory_order_acquire) == 0)
            {
                if (milliseconds() >= deadline || !pause_milliseconds(1))
                {
                    return 0;
                }
            }

            return 1;
        }

        case RETAINED_ITEM_DONE:
            if (index < 0 || index >= RETAINED_ITEM_COUNT || !pause_milliseconds(8))
            {
                return -1;
            }

            return atomic_fetch_add_explicit(&retained.counts[index], 1, memory_order_acq_rel) + 1;

        case RETAINED_CURRENT_COUNT:
            return index >= 0 && index < RETAINED_ITEM_COUNT
                ? atomic_load_explicit(&retained.counts[index], memory_order_acquire) : -1;

        case RETAINED_SNAPSHOT_COUNT:
            return index >= 0 && index < RETAINED_ITEM_COUNT ? retained.snapshot_counts[index] : -1;

        case RETAINED_SNAPSHOT_VALID:
            return retained.snapshot_valid;

        case RETAINED_READ_READY:
            return atomic_load_explicit(&retained.ready, memory_order_acquire);

        case RETAINED_SET_DEADLINE:
            retained.timer_deadline = value;
            return 1;

        case RETAINED_SET_TOKEN:
            if (index < 0 || index >= 2)
            {
                return -1;
            }

            retained.token[index] = value;
            return 1;

        case RETAINED_READ_TOKEN:
            return index >= 0 && index < 2 ? retained.token[index] : 0;

        default:
            return -1;
    }
}

/* Registered before runtime enable: prepare-handler reversal makes this run AFTER service retirement. */
static void
capture_retained_snapshot(void)
{
    if (!retained.armed)
    {
        return;
    }

    retained.snapshot_time = milliseconds();
    retained.snapshot_valid = 1;
    retained.pending_global = 0;
    retained.pending_local = 0;
    for (int index = 0; index < RETAINED_ITEM_COUNT; index++)
    {
        int count = atomic_load_explicit(&retained.counts[index], memory_order_acquire);
        retained.snapshot_counts[index] = count;
        if (count < 0 || count > 1)
        {
            retained.snapshot_valid = 0;
        }

        if (count == 0)
        {
            if (index < RETAINED_GLOBAL_COUNT)
            {
                retained.pending_global++;
            }
            else
            {
                retained.pending_local++;
            }
        }
    }

    if (retained.mode == RETAINED_TIMER)
    {
        if (retained.snapshot_counts[0] != 0 || retained.snapshot_time >= retained.timer_deadline)
        {
            retained.snapshot_valid = 0;
        }
    }
    else if (retained.mode == RETAINED_QUEUE)
    {
        if (retained.pending_global == 0 || retained.pending_local == 0 ||
            atomic_load_explicit(&retained.ready, memory_order_acquire) != 1)
        {
            retained.snapshot_valid = 0;
        }
    }
    else
    {
        retained.snapshot_valid = 0;
    }
}

/* Registered after runtime enable: this releases the seeded worker BEFORE runtime service retirement. */
static void
release_retained_worker(void)
{
    if (retained.armed && retained.mode == RETAINED_QUEUE)
    {
        atomic_store_explicit(&retained.release, 1, memory_order_release);
    }
}

/* Starts a new parent setup only after the previous process-local callbacks and root checks completed. */
static void
reset_retained(enum retained_mode mode)
{
    retained.armed = false;
    retained.mode = mode;
    retained.snapshot_valid = 0;
    retained.snapshot_time = 0;
    retained.timer_deadline = 0;
    retained.pending_global = 0;
    retained.pending_local = 0;
    retained.token[0] = 0;
    retained.token[1] = 0;
    atomic_store_explicit(&retained.ready, 0, memory_order_release);
    atomic_store_explicit(&retained.release, 0, memory_order_release);
    for (int index = 0; index < RETAINED_ITEM_COUNT; index++)
    {
        atomic_store_explicit(&retained.counts[index], 0, memory_order_release);
        retained.snapshot_counts[index] = 0;
    }
}

/* Emits the immutable prepare snapshot before first managed entry can resume child services. */
static void
emit_retained_snapshot(void)
{
    emit("snapshot-valid", 0, retained.snapshot_valid);
    if (retained.mode == RETAINED_TIMER)
    {
        emit("snapshot-timer-count", 0, retained.snapshot_counts[0]);
        emit("snapshot-deadline-remaining-ms", 0, retained.timer_deadline - retained.snapshot_time);
    }
    else
    {
        emit("snapshot-pending-global", 0, retained.pending_global);
        emit("snapshot-pending-local", 0, retained.pending_local);
    }
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
    return result;
}

/* Validates copied mutations without replaying initialization or sharing writable managed objects. */
static int
check_lineage(lineage_fn lineage, pid_t parent, int64_t token_low, int64_t token_high,
              int expected, int replacement, const char *role)
{
    current_role = role;
    current_stage = "lineage";
    emit("begin", expected, replacement);
    int result = lineage((int32_t) getpid(), (int32_t) parent, token_low, token_high,
                         expected, replacement, report);
    emit("result", 0, result);
    return result;
}

/* Exercises replacement finalizers, fresh pool workers, timers, process identity, and exceptions. */
static int
check_services(run_fn run, pid_t parent, int64_t token_low, int64_t token_high, const char *role)
{
    for (int stage = 2; stage <= 6; stage++)
    {
        if (invoke(run, stage, parent, token_low, token_high, role) != 0)
        {
            return 1;
        }
    }

    return 0;
}

/* Mode two forks a grandchild before the child's first managed call; mode one repairs and warms first. */
static int
run_descendant_child(int mode, lineage_fn lineage, run_fn run, pid_t parent,
                     int64_t token_low, int64_t token_high)
{
    int expected = 17;
    if (mode == 1)
    {
        if (check_lineage(lineage, parent, token_low, token_high, 17, 131, "child-warm") != 0 ||
            check_services(run, parent, token_low, token_high, "child-warm") != 0)
        {
            return 1;
        }

        expected = 131;
    }

    for (int sibling = 0; sibling < 2; sibling++)
    {
        current_role = "child";
        current_stage = "grandchild-fork";
        emit("before-native-fork", mode, sibling);
        pid_t grandchild = fork();
        if (grandchild < 0)
        {
            emit("fork-error", 0, errno);
            return 70;
        }

        if (grandchild == 0)
        {
            int replacement = 997 + sibling;
            int result = check_lineage(lineage, parent, token_low, token_high,
                                       expected, replacement, "grandchild");
            if (result == 0)
            {
                result = check_services(run, parent, token_low, token_high, "grandchild");
            }

            if (result == 0)
            {
                result = check_lineage(lineage, parent, token_low, token_high,
                                       replacement, replacement, "grandchild-after-gc");
            }

            _exit(result == 0 ? 0 : 1);
        }

        int result = wait_bounded(grandchild, 12, false);
        emit("grandchild-status", sibling, result);
        if (result != 0 ||
            check_lineage(lineage, parent, token_low, token_high, expected, 131, "child-after-grandchild") != 0 ||
            check_services(run, parent, token_low, token_high, "child-after-grandchild") != 0)
        {
            return 1;
        }

        expected = 131;
    }

    return 0;
}

/* Two siblings and two independent child generations catch stale recovery state and parent corruption. */
static int
run_descendants(struct options options, void *library, run_fn run, pid_t parent,
                int64_t token_low, int64_t token_high)
{
    lineage_fn lineage = (lineage_fn) dlsym(library, "fork_probe_lineage");
    if (lineage == NULL)
    {
        emit("missing-export", 0, 3);
        return 70;
    }

    for (int round = 1; round <= options.rounds; round++)
    {
        current_round = round;
        current_role = "parent";
        current_stage = "child-fork";
        emit("before-native-fork", options.descendants, 0);
        pid_t child = fork();
        if (child < 0)
        {
            emit("fork-error", 0, errno);
            return 70;
        }

        if (child == 0)
        {
            _exit(run_descendant_child(options.descendants, lineage, run, parent,
                                       token_low, token_high));
        }

        int result = wait_bounded(child, 35, false);
        emit("child-status", 0, result);
        if (result != 0 ||
            check_lineage(lineage, parent, token_low, token_high, 17, 17, "parent-after-descendants") != 0 ||
            check_services(run, parent, token_low, token_high, "parent-after-descendants") != 0)
        {
            return 1;
        }
    }

    current_role = "parent";
    current_stage = "summary";
    emit("failures", 0, 0);
    return 0;
}

/* Forks only after a complete original setup; incomplete pending snapshots are failures, never skips. */
static int
run_retained(struct options options, retained_prepare_fn prepare, retained_run_fn run, pid_t parent)
{
    int failures = 0;
    for (int round = 1; round <= options.rounds; round++)
    {
        current_round = round;
        current_role = "parent-prepare";
        current_stage = options.retained == RETAINED_TIMER ? "retained-timer" : "retained-queue";
        reset_retained(options.retained);
        emit("begin", 0, options.retained);
        int result = prepare(options.retained, round, retained_control, report);
        emit("result", 0, result);
        if (result != 0)
        {
            failures++;
            break;
        }

        retained.armed = true;
        emit("before-native-fork", 0, options.retained);
        pid_t child = fork();
        retained.armed = false;
        if (child < 0)
        {
            emit("fork-error", 0, errno);
            return 70;
        }

        if (child == 0)
        {
            current_role = "child";
            emit_retained_snapshot();
            emit("begin", 0, options.retained);
            result = run(options.retained, round, (int32_t) getpid(), (int32_t) parent, report);
            emit("result", 0, result);
            _exit(result == 0 ? 0 : 1);
        }

        current_role = "parent-after-child";
        emit_retained_snapshot();
        result = wait_bounded(child, 12, false);
        emit("child-status", 0, result);
        if (result != 0)
        {
            failures++;
        }

        emit("begin", 0, options.retained);
        result = run(options.retained, round, (int32_t) getpid(), (int32_t) parent, report);
        emit("result", 0, result);
        if (result != 0)
        {
            failures++;
        }

        if (failures != 0)
        {
            break;
        }
    }

    current_role = "parent";
    current_stage = "summary";
    emit("failures", 0, failures);
    return failures == 0 ? 0 : 1;
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

    retained_prepare_fn retained_prepare = NULL;
    retained_run_fn retained_run = NULL;
    if (options.retained != RETAINED_NONE)
    {
        retained_prepare = (retained_prepare_fn) dlsym(library, "fork_probe_retained_prepare");
        retained_run = (retained_run_fn) dlsym(library, "fork_probe_retained_run");
        if (retained_prepare == NULL || retained_run == NULL)
        {
            emit("missing-export", 0, 2);
            return 70;
        }

        int registered = pthread_atfork(capture_retained_snapshot, NULL, NULL);
        if (registered != 0)
        {
            emit("atfork-error", 0, registered);
            return 70;
        }
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
    }

    if (options.retained != RETAINED_NONE)
    {
        int registered = pthread_atfork(release_retained_worker, NULL, NULL);
        if (registered != 0)
        {
            emit("atfork-error", 0, registered);
            return 70;
        }

        return run_retained(options, retained_prepare, retained_run, parent);
    }

    if (options.descendants != 0)
    {
        return run_descendants(options, library, run, parent, token_low, token_high);
    }

    int failures = 0;
    for (int round = 1; round <= options.rounds; round++)
    {
        current_round = round;
        for (int stage = 1; stage <= 6; stage++)
        {
            current_role = "parent";
            current_stage = stage_names[stage];
            emit("before-native-fork", 0, 0);
            pid_t child = fork();
            if (child < 0)
            {
                emit("fork-error", 0, errno);
                return 70;
            }

            if (child == 0)
            {
                int result = invoke(run, stage, parent, token_low, token_high, "child");
                _exit(result == 0 ? 0 : 1);
            }

            int result = wait_bounded(child, 6, false);
            emit("child-status", 0, result);
            if (result != 0)
            {
                failures++;
            }

            // Minimal mode must not start parent pool/timer workers before the next fork.
            int parent_stage = options.minimal && stage > 3 ? 1 : stage;
            if (invoke(run, parent_stage, parent, token_low, token_high, "parent-after-child") != 0)
            {
                failures++;
            }
        }
    }

    // Once all forks are finished, check every parent facility even in minimal mode.
    for (int stage = 1; stage <= 6; stage++)
    {
        if (invoke(run, stage, parent, token_low, token_high, "parent-final") != 0)
        {
            failures++;
        }
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

    struct options options = {argv[1], false, false, 2, RETAINED_NONE, 0};
    for (int argument = 2; argument < argc; argument++)
    {
        if (strcmp(argv[argument], "--minimal") == 0)
        {
            options.minimal = true;
        }
        else if (strcmp(argv[argument], "--enable-fork") == 0)
        {
            options.enable = true;
        }
        else if (strcmp(argv[argument], "--retained-timer") == 0 && options.retained == RETAINED_NONE)
        {
            options.retained = RETAINED_TIMER;
        }
        else if (strcmp(argv[argument], "--retained-queue") == 0 && options.retained == RETAINED_NONE)
        {
            options.retained = RETAINED_QUEUE;
        }
        else if (strcmp(argv[argument], "--descendants") == 0 && options.descendants == 0)
        {
            options.descendants = 1;
        }
        else if (strcmp(argv[argument], "--descendants-pending") == 0 && options.descendants == 0)
        {
            options.descendants = 2;
        }
        else
        {
            emit("usage-error", 0, 64);
            return 64;
        }
    }

    if (options.retained != RETAINED_NONE && !options.enable)
    {
        emit("usage-error", 0, 64);
        return 64;
    }

    if (options.descendants != 0 && (!options.enable || options.minimal || options.retained != RETAINED_NONE))
    {
        emit("usage-error", 0, 64);
        return 64;
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

    int result = wait_bounded(worker, 120, true);
    current_stage = "summary";
    emit("worker-status", 0, result);
    return result;
}
