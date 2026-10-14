// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

#include "UnixSignals.h"
#include <dlfcn.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

static struct sigaction previous_action;
static volatile sig_atomic_t inherited_termination;
static volatile sig_atomic_t child_termination;
static volatile sig_atomic_t observed_mask;
static volatile sig_atomic_t observed_stack;
static volatile sig_atomic_t observed_child;
static unsigned char alternate_stack[128 * 1024];

static void inherited_term(int)
{
    inherited_termination = 1;
}

static void child_term(int)
{
    child_termination = 1;
}

// The host forks from its signal handler and installs child handlers before
// unblocking signals. A pending termination must reach the child handler only.
static void host_handler(int)
{
    sigset_t current;
    observed_mask = sigprocmask(SIG_BLOCK, NULL, &current) == 0 &&
        sigismember(&current, SIGTERM) == 1 &&
        sigismember(&current, SIGINT) == 1 &&
        sigismember(&current, SIGUSR2) == 1;
    unsigned char stack_marker;
    uintptr_t address = (uintptr_t)&stack_marker;
    observed_stack = address >= (uintptr_t)alternate_stack &&
        address < (uintptr_t)(alternate_stack + sizeof(alternate_stack));

    pid_t child = fork();
    if (child == 0)
    {
        alarm(5);
        if (kill(getpid(), SIGTERM) != 0 || inherited_termination != 0)
        {
            _exit(21);
        }

        sigset_t pending;
        if (sigpending(&pending) != 0 || sigismember(&pending, SIGTERM) != 1)
        {
            _exit(22);
        }

        struct sigaction action = {};
        action.sa_handler = child_term;
        sigemptyset(&action.sa_mask);
        sigset_t termination;
        sigemptyset(&termination);
        sigaddset(&termination, SIGTERM);
        if (sigaction(SIGTERM, &action, NULL) != 0 ||
            sigprocmask(SIG_UNBLOCK, &termination, NULL) != 0)
        {
            _exit(23);
        }

        _exit(child_termination == 1 && inherited_termination == 0 ? 0 : 24);
    }

    int status = 0;
    pid_t waited;
    do
    {
        waited = waitpid(child, &status, 0);
    } while (waited == -1 && errno == EINTR);

    observed_child = child > 0 && waited == child && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static void host_siginfo_handler(int signal, siginfo_t*, void*)
{
    host_handler(signal);
}

static void chained_handler(int signal, siginfo_t* info, void* context)
{
    if ((previous_action.sa_flags & SA_SIGINFO) != 0)
    {
        previous_action.sa_sigaction(signal, info, context);
    }
    else
    {
        previous_action.sa_handler(signal);
    }
}

static int run_case(bool use_alternate_stack, bool use_siginfo, const char* library_path)
{
    alarm(10);
    stack_t stack = {};
    stack.ss_sp = alternate_stack;
    stack.ss_size = sizeof(alternate_stack);
    if (sigaltstack(&stack, NULL) != 0)
    {
        return 10;
    }

    sigset_t unblocked;
    sigemptyset(&unblocked);
    if (sigprocmask(SIG_SETMASK, &unblocked, NULL) != 0)
    {
        return 11;
    }

    struct sigaction termination = {};
    termination.sa_handler = inherited_term;
    sigemptyset(&termination.sa_mask);
    struct sigaction host = {};
    host.sa_flags = SA_RESTART | (use_alternate_stack ? SA_ONSTACK : 0) | (use_siginfo ? SA_SIGINFO : 0);
    if (use_siginfo)
    {
        host.sa_sigaction = host_siginfo_handler;
    }
    else
    {
        host.sa_handler = host_handler;
    }

    sigemptyset(&host.sa_mask);
    sigaddset(&host.sa_mask, SIGTERM);
    sigaddset(&host.sa_mask, SIGINT);
    sigaddset(&host.sa_mask, SIGUSR2);
    int activation_signal = INJECT_ACTIVATION_SIGNAL;
    if (sigaction(SIGTERM, &termination, NULL) != 0 ||
        sigaction(activation_signal, &host, NULL) != 0)
    {
        return 12;
    }

    if (library_path == NULL)
    {
        if (!AddSignalHandler(activation_signal, chained_handler, &previous_action))
        {
            return 14;
        }
    }
    else
    {
        // Load the ordinary Native AOT host-shutdown probe after registering
        // the host handler. Its actual activation handler must chain safely.
        previous_action = host;
        void* library = dlopen(library_path, RTLD_NOW | RTLD_LOCAL);
        if (library == NULL)
        {
            fprintf(stderr, "%s\n", dlerror());
            return 15;
        }

        typedef int (*probe_fn)(void);
        probe_fn initialize = (probe_fn)dlsym(library, "shutdown_probe_initialize");
        probe_fn enable = (probe_fn)dlsym(library, "RhEnableForkSupport");
        if (initialize == NULL || enable == NULL || initialize() != 0 || enable() != 1)
        {
            return 16;
        }
    }

    if (raise(activation_signal) != 0)
    {
        return 17;
    }

    bool chained_passed = observed_mask == 1 && observed_stack == use_alternate_stack && observed_child == 1;
    printf("chained host: mask=%d stack=%d child=%d\n", (int)observed_mask, (int)observed_stack, (int)observed_child);
    RestoreSignalHandler(activation_signal, &previous_action);
    observed_mask = observed_child = 0;
    if (raise(activation_signal) != 0)
    {
        return 13;
    }

    bool restored_passed = observed_mask == 1 && observed_stack == use_alternate_stack && observed_child == 1;
    printf("restored host: mask=%d stack=%d child=%d\n", (int)observed_mask, (int)observed_stack, (int)observed_child);
    return chained_passed && restored_passed ? 0 : 1;
}

int main(int argc, char** argv)
{
    if (argc > 2)
    {
        fprintf(stderr, "usage: %s [NativeHostShutdownProbe library]\n", argv[0]);
        return 2;
    }

    int failures = 0;
    for (int mode = 0; mode < 4; mode++)
    {
        fflush(NULL);
        pid_t child = fork();
        if (child == 0)
        {
            int result = run_case((mode & 1) != 0, (mode & 2) != 0, argc == 2 ? argv[1] : NULL);
            fflush(NULL);
            _exit(result);
        }

        int status = 0;
        pid_t waited;
        do
        {
            waited = waitpid(child, &status, 0);
        } while (waited == -1 && errno == EINTR);

        bool passed = child > 0 && waited == child && WIFEXITED(status) && WEXITSTATUS(status) == 0;
        printf("alternate-stack=%d siginfo=%d: %s (status=%d)\n", mode & 1, (mode >> 1) & 1, passed ? "PASS" : "FAIL", status);
        failures += !passed;
    }

    return failures == 0 ? 0 : 1;
}
