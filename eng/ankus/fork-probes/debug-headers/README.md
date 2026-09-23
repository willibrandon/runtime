# Independent debugger metadata

This Linux test loads two Native AOT libraries with `RTLD_GLOBAL`, as PostgreSQL
does. Each library must expose its own initialized `DotNetRuntimeDebugHeader`.
Loading and executing the second library must leave the first descriptor and its
tables unchanged. The host also checks each table's owning image, the recorded
module base, independent runtime and GC addresses, and managed execution with GC.

Set `ANKUS_AOT_SDK` to the complete built Native AOT SDK directory. From this directory:

```sh
dotnet publish -c Release -r linux-x64 -o publish-a -p:AssemblyName=NativeDebugHeaderA -p:IlcSdkPath="$ANKUS_AOT_SDK/"
dotnet publish -c Release -r linux-x64 -o publish-b -p:AssemblyName=NativeDebugHeaderB -p:IlcSdkPath="$ANKUS_AOT_SDK/"
cc -std=gnu17 -O2 -Wall -Wextra -Werror host.c -ldl -o host
./host "$PWD/publish-a/NativeDebugHeaderA.so" "$PWD/publish-b/NativeDebugHeaderB.so"
./host "$PWD/publish-b/NativeDebugHeaderB.so" "$PWD/publish-a/NativeDebugHeaderA.so"
cp publish-a/NativeDebugHeaderA.so publish-a/NativeDebugHeaderCopy.so
./host "$PWD/publish-a/NativeDebugHeaderA.so" "$PWD/publish-a/NativeDebugHeaderCopy.so"
```

Each invocation must exit zero and print `PASS independent-debug-headers`.
The copied-image case requires a separate file, not a hard link or symbolic link.
Each host invocation starts a fresh process and has a 30-second deadline.

The original default-visibility ELF export fails when the second library's own
descriptor remains uninitialized. Protected ELF visibility preserves debugger
lookup while keeping runtime writes bound to the image that owns the descriptor.
The public descriptor format and exported name remain unchanged.
