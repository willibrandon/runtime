# Diagnostics listener and transport checks

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
python3 io.py --host ./host --library publish/NativeDiagnosticsProbe.so --output io-results
python3 connect.py --host ./host --library publish/NativeDiagnosticsProbe.so --output connect-results
python3 reverse.py --host ./host --library publish/NativeDiagnosticsProbe.so --output reverse-results
python3 listener.py --host ./host --library publish/NativeDiagnosticsProbe.so --output listener-results
```

Set `ANKUS_AOT_SDK` to the SDK directory produced by the owned runtime build. Use a
new results directory for each run. `--case ownership`, `--case ownership-close`
and `--case listen-failure` select the individual regressions. The runner bounds
its host, owns its temporary socket directory, and reaps its child processes.

`io.py` checks actual socket transfers against an independently controlled client.
Partial reads, slow progress, interrupted waits and blocked writes must respect a
single 300 ms deadline. Other cases verify complete bytes, EOF, zero-byte transfers,
zero-timeout reads, infinite waits and delayed file-descriptor passing. Signals
target the native host thread, and the probe reports how many it handled. The
supervisor kills only its own process group if the runtime stops responding.
Use `--case partial-read` or another listed case to reproduce one failure against
an earlier SDK. All cases use the socket implementation linked into the actual
Native AOT library; the fixture does not replace its transfer or timing functions.

Descriptor cases reject extra or truncated descriptor lists and check the native
process's open-descriptor count after cleanup. A valid descriptor must carry its
exact file bytes, have close-on-exec set, and be absent after a native child runs
`exec`. That child performs no managed calls.

`connect.py` fills a Unix listener's connection queue, verifies that finite
connection attempts return without hanging or leaking descriptors, then empties
the queue and checks a successful retry. It also covers zero/infinite waits,
missing endpoints and refused connections. `reverse.py` configures an actual
runtime diagnostic port against a full listener. The default diagnostic endpoint
must keep answering `ProcessInfo2` requests. After the queue is emptied, three
successive reverse connections must advertise the same runtime identity and
answer complete protocol requests.

`listener.py` stops and joins the real diagnostic listener, verifies that its
kernel thread has exited, and starts a replacement. It checks idle connections,
partial headers and payloads, malformed requests, and clients closing during a
pause. Requests queued during a pause must wait for restart. Completed requests
must retain exact values, process identity and endpoint ownership. Reverse
connections also preserve partial requests and reconnect afterward. Twenty idle
cycles check that stopping and restarting does not leak descriptors.

The compiler's existing native-library EventSource warning remains visible. These
checks do not enable managed fork support with diagnostics. Native cleanup tests
fork without managed child calls; listener tests stop and restart without forking.
Commands already sending responses, active trace sessions, sampling, child
recovery and multiple runtime instances still need fork-aware lifetimes.
