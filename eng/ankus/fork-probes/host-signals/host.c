// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifdef __APPLE__
#include <mach/mach.h>
#endif

typedef int (*probe_fn)(void);
typedef void (*creation_callback)(void);
typedef void (*set_creation_callback)(creation_callback);

// PostgreSQL handles these signals, and its handlers assume they interrupt its own thread.
static const int host_signals[] = {SIGHUP, SIGINT, SIGQUIT, SIGALRM, SIGTERM, SIGUSR1, SIGUSR2, SIGURG, SIGCHLD};
#define HOST_SIGNAL_COUNT (sizeof(host_signals) / sizeof(host_signals[0]))

static pthread_t host_thread;
static volatile sig_atomic_t on_host[HOST_SIGNAL_COUNT];
static volatile sig_atomic_t off_host[HOST_SIGNAL_COUNT];
static int creations;
static int host_creations;

// A host that must route its own signals prepares before each runtime thread starts.
static void on_thread_creation(void)
{
    __atomic_add_fetch(&creations, 1, __ATOMIC_RELAXED);
    if (pthread_equal(pthread_self(), host_thread))
    {
        __atomic_add_fetch(&host_creations, 1, __ATOMIC_RELAXED);
    }
}

static void host_handler(int signal)
{
    for (size_t index = 0; index < HOST_SIGNAL_COUNT; index++)
    {
        if (host_signals[index] == signal)
        {
            if (pthread_equal(pthread_self(), host_thread))
            {
                on_host[index]++;
            }
            else
            {
                off_host[index]++;
            }
        }
    }
}

static int count_threads(void)
{
#ifdef __APPLE__
    thread_act_array_t threads;
    mach_msg_type_number_t count;
    if (task_threads(mach_task_self(), &threads, &count) != KERN_SUCCESS)
    {
        return -1;
    }

    for (mach_msg_type_number_t index = 0; index < count; index++)
    {
        mach_port_deallocate(mach_task_self(), threads[index]);
    }

    vm_deallocate(mach_task_self(), (vm_address_t)threads, count * sizeof(thread_act_t));
    return (int)count;
#else
    DIR* tasks = opendir("/proc/self/task");
    if (tasks == NULL)
    {
        return -1;
    }

    int count = 0;
    struct dirent* entry;
    while ((entry = readdir(tasks)) != NULL)
    {
        count += entry->d_name[0] != '.';
    }

    closedir(tasks);
    return count;
#endif
}

static void pause_milliseconds(long milliseconds)
{
    struct timespec remaining = {milliseconds / 1000, (milliseconds % 1000) * 1000000};
    while (nanosleep(&remaining, &remaining) == -1 && errno == EINTR)
    {
    }
}

static int run_case(const char* library_path)
{
    host_thread = pthread_self();
    sigset_t host_set;
    sigemptyset(&host_set);
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = host_handler;
    action.sa_flags = SA_RESTART;
    sigemptyset(&action.sa_mask);
    for (size_t index = 0; index < HOST_SIGNAL_COUNT; index++)
    {
        sigaddset(&host_set, host_signals[index]);
        if (sigaction(host_signals[index], &action, NULL) != 0)
        {
            return 10;
        }
    }

    void* library = dlopen(library_path, RTLD_NOW | RTLD_LOCAL);
    if (library == NULL)
    {
        fprintf(stderr, "%s\n", dlerror());
        return 11;
    }

    probe_fn start = (probe_fn)dlsym(library, "signal_probe_start");
    probe_fn collect = (probe_fn)dlsym(library, "signal_probe_collect");
    probe_fn stop = (probe_fn)dlsym(library, "signal_probe_stop");
    set_creation_callback set_callback = (set_creation_callback)dlsym(library, "RhSetThreadCreationCallback");
    if (start == NULL || collect == NULL || stop == NULL || set_callback == NULL)
    {
        return 12;
    }

    set_callback(on_thread_creation);
    if (start() != 0)
    {
        return 12;
    }

    // The host, finalizer, parked managed thread and thread-pool worker at least.
    int threads = count_threads();
    int created = __atomic_load_n(&creations, __ATOMIC_RELAXED);
    int created_on_host = __atomic_load_n(&host_creations, __ATOMIC_RELAXED);
    printf("threads: %d, creation callbacks: %d (%d on the host thread)\n", threads, created, created_on_host);
    if (threads < 4 || created < threads - 1 || created_on_host < 2)
    {
        return 13;
    }

    // While the host thread blocks a signal sent to the process, the kernel must hold it:
    // every runtime thread blocks it too. Unblocking then delivers it to the host thread.
    if (pthread_sigmask(SIG_BLOCK, &host_set, NULL) != 0)
    {
        return 14;
    }

    for (size_t index = 0; index < HOST_SIGNAL_COUNT; index++)
    {
        if (kill(getpid(), host_signals[index]) != 0)
        {
            return 15;
        }
    }

    pause_milliseconds(200);
    int failures = 0;
    for (size_t index = 0; index < HOST_SIGNAL_COUNT; index++)
    {
        if (on_host[index] != 0 || off_host[index] != 0)
        {
            printf("signal %d ran while blocked: host=%d runtime=%d\n",
                host_signals[index], (int)on_host[index], (int)off_host[index]);
            failures++;
        }
    }

    if (pthread_sigmask(SIG_UNBLOCK, &host_set, NULL) != 0)
    {
        return 16;
    }

    pause_milliseconds(50);
    for (size_t index = 0; index < HOST_SIGNAL_COUNT; index++)
    {
        if (on_host[index] != 1 || off_host[index] != 0)
        {
            printf("signal %d after unblocking: host=%d runtime=%d\n",
                host_signals[index], (int)on_host[index], (int)off_host[index]);
            failures++;
        }
    }

    int collected = collect();
    int stopped = stop();
    printf("collect=%d stop=%d\n", collected, stopped);
    return failures == 0 && collected == 0 && stopped == 0 ? 73 : 1;
}

int main(int argc, char** argv)
{
    if (argc == 3 && strcmp(argv[2], "--case") == 0)
    {
        return run_case(argv[1]);
    }

    if (argc != 2)
    {
        fprintf(stderr, "usage: %s library\n", argv[0]);
        return 2;
    }

    fflush(NULL);
    pid_t child = fork();
    if (child == -1)
    {
        return 3;
    }

    if (child == 0)
    {
        execl(argv[0], argv[0], argv[1], "--case", (char*)NULL);
        _exit(4);
    }

    // A hang is a bounded failure, never a skipped case.
    int status = 0;
    pid_t waited = 0;
    for (int elapsed = 0; elapsed < 300 && waited == 0; elapsed++)
    {
        waited = waitpid(child, &status, WNOHANG);
        if (waited == 0)
        {
            pause_milliseconds(100);
        }
    }

    if (waited == 0)
    {
        kill(child, SIGKILL);
        waitpid(child, &status, 0);
        printf("host signals: FAIL (timed out)\n");
        return 1;
    }

    int passed = waited == child && WIFEXITED(status) && WEXITSTATUS(status) == 73;
    printf("host signals: %s (wait status %d)\n", passed ? "PASS" : "FAIL", status);
    return passed ? 0 : 1;
}
