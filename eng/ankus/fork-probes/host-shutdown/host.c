// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

typedef int (*probe_fn)(void);
static probe_fn verify_exit;

// The observer enters managed code only after the owner starts its native exit.
// The fork admission gate must reopen for both the exit callback and observer.
static void* observe_exit(void* unused)
{
    (void)unused;
    int result = verify_exit();
    exit(result == 0 ? 73 : result);
}

static int run_case(const char* library_path, const char* mode)
{
    // A deadlock is a bounded, non-successful process outcome, never a skipped case.
    alarm(15);
    void* library = dlopen(library_path, RTLD_NOW | RTLD_LOCAL);
    if (library == NULL)
    {
        fprintf(stderr, "%s\n", dlerror());
        return 10;
    }

    probe_fn initialize = (probe_fn)dlsym(library, "shutdown_probe_initialize");
    probe_fn enable = (probe_fn)dlsym(library, "RhEnableForkSupport");
    verify_exit = (probe_fn)dlsym(library, "shutdown_probe_verify");
    if (initialize == NULL || enable == NULL || verify_exit == NULL || initialize() != 0 || enable() != 1)
    {
        return 11;
    }

    if (strcmp(mode, "native-thread-exit") == 0)
    {
        pthread_t observer;
        if (pthread_create(&observer, NULL, observe_exit, NULL) != 0)
        {
            return 12;
        }

        pthread_exit(NULL);
    }

    if (strcmp(mode, "native-process-exit") == 0)
    {
        exit(73);
    }

    if (strcmp(mode, "native-main-return") == 0)
    {
        return 73;
    }

    return 13;
}

int main(int argc, char** argv)
{
    if (argc == 3)
    {
        return run_case(argv[1], argv[2]);
    }

    if (argc != 2)
    {
        fprintf(stderr, "usage: %s library\n", argv[0]);
        return 14;
    }

    const char* modes[] = {"native-main-return", "native-process-exit", "native-thread-exit"};
    int failures = 0;
    for (size_t index = 0; index < sizeof(modes) / sizeof(modes[0]); index++)
    {
        pid_t child = fork();
        if (child == -1)
        {
            return 15;
        }

        if (child == 0)
        {
            execl(argv[0], argv[0], argv[1], modes[index], (char*)NULL);
            _exit(16);
        }

        int result;
        pid_t waited;
        do
        {
            waited = waitpid(child, &result, 0);
        } while (waited == -1 && errno == EINTR);

        int passed = waited == child && WIFEXITED(result) && WEXITSTATUS(result) == 73;
        printf("%s: %s (wait status %d)\n", modes[index], passed ? "PASS" : "FAIL", waited == child ? result : -1);
        failures += !passed;
    }

    return failures == 0 ? 0 : 1;
}
