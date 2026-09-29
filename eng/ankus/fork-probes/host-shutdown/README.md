# Dormant host shutdown

This native supervisor checks returning from `main`, calling `exit`, and leaving
the native owner with `pthread_exit` after enabling fork support. Each fresh
process must exit with status 73 within 15 seconds. The thread-exit case also
requires managed `Thread.Join`, `IsAlive`, and mutex abandonment to observe the
owner's normal managed cleanup, followed by allocation, collection and finalizer
drain; bypassing the managed callback or leaving the collector parked cannot pass.

Build the Release Native AOT runtime and CoreLib, then publish this library with
the matching SDK directory. On macOS ARM64:

```sh
dotnet publish -c Release -r osx-arm64 -o publish -p:IlcSdkPath="$ANKUS_AOT_SDK/"
cc -std=gnu17 -O2 -Wall -Wextra -Werror host.c -pthread -o host
./host "$PWD/publish/NativeHostShutdownProbe.dylib"
```

On Linux x64, use `linux-x64`, add `-ldl` to the native link, and select the `.so`
library. The original macOS dormant runtime hangs in all three cases. Linux's
process exit does not run pthread-key destructors, so the thread-exit case is
needed there to exercise the same runtime boundary.
