# Host signals stay on host threads

The kernel delivers a signal sent to a process to any thread that does not block
it. A native host such as PostgreSQL expects its handlers to interrupt its own
thread, so runtime threads must leave those signals alone.

This supervisor installs handlers for nine signals PostgreSQL uses, then loads a
Native AOT library that starts the finalizer, a parked managed thread, a parked
thread-pool worker and a background collection. With the signals blocked on the
host thread, it sends each one to the process. Every signal must stay pending
and then run on the host thread once unblocked. A managed thread spinning without
calls must still be suspended for a collection, which needs the runtime's
activation signal. The host also registers `RhSetThreadCreationCallback` before
the first managed call; it must run before every runtime thread starts, on the
creating thread. The case runs in a fresh process with a 30-second deadline.

On macOS, `sigprocmask` changes every thread's mask, so a host that calls it
unblocks these signals on runtime threads again. Such a host uses the creation
callback to route its own handlers to its thread instead.

After building the Release Native AOT runtime and CoreLib, on Linux x64:

```sh
dotnet publish -c Release -r linux-x64 -o publish -p:IlcSdkPath="$ANKUS_AOT_SDK/"
cc -std=gnu17 -O2 -Wall -Wextra -Werror host.c -ldl -pthread -o host
./host "$PWD/publish/NativeHostSignalProbe.so"
```

On macOS, use `osx-arm64` or `osx-x64`, omit `-ldl` and select the `.dylib`
library. Before the correction, runtime threads ran every handler on Linux, and
PostgreSQL 13's latch could miss a wakeup on macOS.
