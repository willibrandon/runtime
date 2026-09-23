# Native AOT fork checks

These Linux x64 checks load a shared Native AOT library, initialize managed state,
and call `fork` from native code after managed calls return. Parent and child
must preserve object identity, values and pending work. Native deadlines turn
hangs into failed runs.

Build the runtime's Release nativeaot component and CoreLib first. Set
`ANKUS_AOT_SDK` to the absolute `artifacts/bin/coreclr/linux.x64.Release/aotsdk`
directory produced by those builds. The probes pin the matching .NET SDK and
compiler; use that SDK's `dotnet` command from each probe directory.

From `retained-services`:

```sh
dotnet publish -c Release -r linux-x64 -o publish -p:ForkRuntimePrototype=true -p:ForkWorkProtocol=true -p:IlcSdkPath="$ANKUS_AOT_SDK/"
cc -std=gnu17 -O2 -Wall -Wextra -Werror host.c -ldl -pthread -o host
./host "$PWD/publish/NativeForkProbe.so" --retained-timer --enable-fork
./host "$PWD/publish/NativeForkProbe.so" --retained-queue --enable-fork
./host "$PWD/publish/NativeForkProbe.so" --retained-waits --enable-fork
./host "$PWD/publish/NativeForkProbe.so" --retained-wait-retirement --enable-fork
./host "$PWD/publish/NativeForkProbe.so" --retained-queued-waits --enable-fork
./host "$PWD/publish/NativeForkProbe.so" --retained-finalizer-wait --enable-fork
./host "$PWD/publish/NativeForkProbe.so" --retained-blocking-worker --enable-fork
./host "$PWD/publish/NativeForkProbe.so" --retained-blocking-finalizer --enable-fork
```

The timer must be unfired at the native checkpoint and fire once in each process.
The queue must still contain original objects from both local and global queues;
all original items must complete once with exact values in each process.

The wait checks span two wait threads. They preserve safe and unsafe callback
contexts, original registrations and handles, one-shot and repeating signals,
a finite timeout, and cancellation before fork. Cleanup starts only after the native
runtime reports that fork preparation is draining callbacks. The final native snapshot
requires every wait thread to have exited; the thread inventory uses Linux's
`/proc/self/task`.

The queued-wait case keeps seventy original callbacks pending at the native
checkpoint, with their unregister notifications still unsignaled. Each callback
and notification must complete in both processes without resignal or reregistration.
The finalizer case performs blocking unregister from an actual unreachable object's
finalizer during preparation.

The two blocking cases unregister a wait whose callback is already queued. That
callback itself needs a new timer, another registered wait, and another worker.
Preparation must let this chain finish before stopping services. The snapshot
requires the original callback and unregister operation to be complete, while
sixty-nine other original waits remain unfired for parent and child to exercise.
`ForkWorkProtocol` exports a read-only checkpoint observation for these native tests;
it does not release callbacks, alter scheduling, or appear in consumer libraries.

The same host checks successive generations:

```sh
./host "$PWD/publish/NativeForkProbe.so" --descendants --enable-fork
./host "$PWD/publish/NativeForkProbe.so" --descendants-pending --enable-fork
```

The first case runs managed code in the child before it forks grandchildren.
The second forks before the child's first managed call, while recovery is still
pending. Both run two child processes with two grandchildren each. They check
inherited graph mutations, object and GC-handle identity, the original initializer
token, current process identity, collections, finalizers, pool work, timers and
exceptions. Each ancestor must retain its own values after its descendants exit.

From `active-gc`:

```sh
dotnet publish -c Release -r linux-x64 -o publish -p:ForkRuntimePrototype=true -p:IlcSdkPath="$ANKUS_AOT_SDK/"
cc -std=gnu17 -O2 -Wall -Wextra -Werror host.c -ldl -pthread -o host
./host "$PWD/publish/NativeForkProbe.so" --enable-fork
```

Both rounds must observe an active background collection during preparation.
A request that finishes before preparation, or falls back to a blocking collection
while collector threads start, is checked for heap integrity and finalization but
does not qualify as an active overlap. Up to eight attempts are allowed per round.
Failing to observe overlap is a failure. Parent and child check every retained
node, edge and byte before and after another background collection, then check
finalization, process identity, tasks, timers and exception handling.

Run without `DOTNET_` or `COMPlus_` overrides for `gcServer`, `gcConcurrent`,
`EnableDiagnostics` or `EnableEventPipe` to exercise the default configuration.
To exercise server GC, publish either probe with `-p:ServerGarbageCollection=true`
and pass `--server-gc` to its host. On a machine with at least four available CPUs:

```sh
DOTNET_GCMaxHeapCount=4 ./host "$PWD/publish/NativeForkProbe.so" --server-gc --enable-fork
DOTNET_GCHeapCount=4 ./host "$PWD/publish/NativeForkProbe.so" --server-gc --enable-fork
```

The first command keeps dynamic heap sizing enabled; the second selects four fixed
heaps. Setting `GCHeapCount` disables dynamic sizing, even if its mode is otherwise
enabled. The server probe requires actual server GC, checks that every collector
thread has exited at the native checkpoint, and requires the original collector
complement after recovery. Logs include active and maximum heap counts. Every managed
stage must retain the original GC mode. Background metadata must describe a completed
concurrent collection; a startup fallback must not publish an unfinished BGC record.

These checks do not establish enabled EventPipe or other platforms.
The exported fork symbols are a native test-host interface; ordinary extension
libraries must keep their runtime symbols local.
