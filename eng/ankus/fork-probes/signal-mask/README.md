# Chained host signal masks

This native regression links the real `AddSignalHandler` and
`RestoreSignalHandler` objects from the built Native AOT runtime archive. It
checks ordinary and alternate-stack handlers, with both signal-handler ABIs.
Each chained handler must retain its signal mask and expected stack. A child
forked by that handler queues `SIGTERM`, installs its own handler and then
unblocks the signal. Only the child's handler may receive the termination.
Restoring the original host handler must preserve the same behavior.

After building the Release Native AOT runtime on Linux x64:

```sh
c++ -std=c++17 -O2 -Wall -Wextra -Werror \
  -I../../../../src/coreclr/nativeaot/Runtime/unix \
  host.cpp "$ANKUS_AOT_SDK/libRuntime.WorkstationGC.a" -ldl -o host
./host
```

On macOS, omit `-ldl`. Pass the absolute path of the published
`host-shutdown/NativeHostShutdownProbe` library to also exercise the real
runtime initialization, activation handler and fork callbacks:

```sh
./host /absolute/path/NativeHostShutdownProbe.so
```

Use the `.dylib` library on macOS. Both ordinary-stack cases fail with the
original runtime: the mask is lost, and termination reaches the inherited
handler before child setup. Alternate-stack cases provide a control for the
existing behavior. This focused native check complements the complete
PostgreSQL worker suite; it does not replace full runtime/backend validation.
