# Diagnostics endpoint cleanup checks

This Linux x64 component probe loads Native AOT with `EventSourceSupport=true`.
An external client sends real `ProcessInfo2` requests and verifies the PID, runtime
cookie, module identity and exact protocol field boundaries. Native children close
their inherited endpoint using both normal-close and shutdown paths. Each close
must preserve the parent's endpoint and identity; the creating process must still
remove its own endpoint at shutdown.

A separate case forces `listen` to fail, reuses the released descriptor for a new
file, then frees the failed listener. The new descriptor must remain open. This
checks cleanup ownership instead of relying on a later crash.

From the runtime root, after building Native AOT Release:

```sh
mkdir -p eng/ankus/fork-probes/diagnostics/obj
c++ -std=c++17 -O2 -fPIC -Wall -Wextra -Werror \
  -DFEATURE_NATIVEAOT -DFEATURE_PERFTRACING -DEP_NO_RT_DEPENDENCY \
  -Isrc/native \
  -Iartifacts/obj/coreclr/linux.x64.Release/nativeaot/Runtime/eventpipe/inc \
  -c eng/ankus/fork-probes/diagnostics/bridge.cpp \
  -o eng/ankus/fork-probes/diagnostics/obj/diagnostics-bridge.o
cd eng/ankus/fork-probes/diagnostics
cc -std=gnu17 -O2 -Wall -Wextra -Werror host.c -ldl -o host
dotnet publish -c Release -r linux-x64 -o publish -p:IlcSdkPath="$ANKUS_AOT_SDK/"
python3 run.py --host ./host --library publish/NativeDiagnosticsProbe.so --output results
```

Set `ANKUS_AOT_SDK` to the SDK directory produced by the owned runtime build. Use a
new results directory for each run. `--case ownership`, `--case ownership-close`
and `--case listen-failure` select the individual regressions. The runner bounds
its host, owns its temporary socket directory, and reaps its child processes.

The compiler's existing native-library EventSource warning remains visible. These
checks exercise native endpoint cleanup only: no managed code runs in the forked
children, and enabled-diagnostics fork support still returns its existing rejection.
Trace sessions, sampling, listener retirement/restart, multiple runtime instances
and managed child execution remain part of the unfinished diagnostics work.
