"""Keep the default diagnostics port responsive while a reverse port is full."""

import argparse
import errno
import json
import os
from pathlib import Path
import signal
import socket
import struct
import subprocess
import tempfile

from connect import read_line
from run import process_info, process_info_stream, receive


def main():
    """Verify actual diagnostic advertisement, default-port service and reconnection."""
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", type=Path, required=True)
    parser.add_argument("--library", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    evidence = {"reverse_connections": []}
    with tempfile.TemporaryDirectory(prefix="diag-reverse-", dir="/tmp") as directory:
        endpoint = Path(directory) / "monitor.socket"
        env = dict(os.environ)
        for name in list(env):
            if name.startswith(("DOTNET_", "COMPlus_")) and any(
                    word in name for word in ["Diagnostic", "EventPipe", "gcServer", "gcConcurrent"]):
                env.pop(name)
        env["TMPDIR"] = directory
        env["DOTNET_DiagnosticPorts"] = f"{endpoint},connect,nosuspend"
        stderr_path = args.output / "stderr.log"
        with socket.socket(socket.AF_UNIX) as server, stderr_path.open("w") as stderr:
            server.bind(str(endpoint))
            server.listen(0)
            server.settimeout(3)
            fillers = []
            try:
                for _ in range(16):
                    filler = socket.socket(socket.AF_UNIX)
                    filler.setblocking(False)
                    try:
                        filler.connect(str(endpoint))
                        fillers.append(filler)
                    except BlockingIOError as error:
                        filler.close()
                        assert error.errno == errno.EAGAIN
                        break
                else:
                    raise AssertionError("listener backlog did not fill")
                assert fillers
                evidence["backlog_occupants"] = len(fillers)
                process = subprocess.Popen([str(args.host.resolve()), str(args.library.resolve())],
                                           stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=stderr,
                                           bufsize=0, env=env, start_new_session=True)
                try:
                    assert read_line(process) == f"ready {process.pid}"
                    endpoints = list(Path(directory).glob(f"dotnet-diagnostic-{process.pid}-*-socket"))
                    assert len(endpoints) == 1
                    default = endpoints[0]
                    before = process_info(default, process.pid)
                    evidence["default_while_full"] = before
                    for _ in fillers:
                        queued, _ = server.accept()
                        queued.close()
                    for index in range(3):
                        reverse, _ = server.accept()
                        with reverse:
                            reverse.settimeout(3)
                            magic, cookie, pid, reserved = struct.unpack("<8s16sQH", receive(reverse, 34))
                            assert magic == b"ADVR_V1\0" and pid == process.pid and reserved == 0
                            assert cookie.hex() == before["cookie"]
                            info = process_info_stream(reverse, process.pid)
                            assert info == before
                            evidence["reverse_connections"].append({"index": index, "process": info})
                        assert process_info(default, process.pid) == before
                    process.stdin.write(b"q\n")
                    process.wait(timeout=3)
                    assert process.returncode == 0
                    assert not default.exists()
                    assert stderr_path.stat().st_size == 0
                finally:
                    if process.poll() is None:
                        os.killpg(process.pid, signal.SIGKILL)
                        process.wait(timeout=3)
                    evidence["exit"] = process.returncode
                    (args.output / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
            finally:
                for filler in fillers:
                    filler.close()
    print("default port remained responsive; three reverse connections passed", flush=True)


if __name__ == "__main__":
    main()
