"""Exercise reverse diagnostic connections with a full Unix listener backlog."""

import argparse
import errno
import json
import os
from pathlib import Path
import select
import signal
import socket
import subprocess
import tempfile
import threading
import time


CASES = ["backlog", "zero-backlog", "ready", "missing", "refused", "infinite-backlog"]
MESSAGE = bytes([0, 1, 2, 127, 128, 254, 255])


def read_line(process):
    """Bound the native operation independently of the runtime's own timeout."""
    data = bytearray()
    deadline = time.monotonic() + 3
    while not data.endswith(b"\n"):
        remaining = deadline - time.monotonic()
        assert remaining > 0 and select.select([process.stdout], [], [], remaining)[0], "connect host timed out"
        byte = os.read(process.stdout.fileno(), 1)
        assert byte, f"host exited with {process.poll()}"
        data.extend(byte)
    return data.decode().strip()


def exchange(process, path, timeout, expected, evidence):
    """Verify connection status, timeout classification and descriptor ownership."""
    before = len(list(Path(f"/proc/{process.pid}/fd").iterdir()))
    started = time.monotonic()
    process.stdin.write(f"k {timeout} {path}\n".encode())
    result = read_line(process)
    elapsed = (time.monotonic() - started) * 1000
    assert read_line(process) == "connect-complete 0"
    after = len(list(Path(f"/proc/{process.pid}/fd").iterdir()))
    evidence["attempts"].append({"result": result, "elapsed_ms": elapsed, "fds_before": before, "fds_after": after})
    assert result == expected, result
    assert before == after, "connect attempt leaked a descriptor"
    assert elapsed < 1500, elapsed


def consume(server):
    """Verify a real connection and every byte sent through the returned stream."""
    peer, _ = server.accept()
    with peer:
        peer.settimeout(2)
        data = bytearray()
        while True:
            block = peer.recv(64)
            if not block:
                break
            data.extend(block)
        assert data == MESSAGE, data


def run_case(args, case):
    """Own the listener, backlog occupants, native process and cleanup."""
    evidence = {"case": case, "attempts": []}
    with tempfile.TemporaryDirectory(prefix="diag-connect-", dir="/tmp") as directory:
        env = dict(os.environ)
        for name in list(env):
            if name.startswith(("DOTNET_", "COMPlus_")) and any(
                    word in name for word in ["Diagnostic", "EventPipe", "gcServer", "gcConcurrent"]):
                env.pop(name)
        env["TMPDIR"] = directory
        endpoint = Path(directory) / "client.socket"
        stderr_path = args.output / f"{case}.stderr.log"
        with socket.socket(socket.AF_UNIX) as server, stderr_path.open("w") as stderr:
            server.settimeout(2)
            if case != "missing":
                server.bind(str(endpoint))
            if case not in ("missing", "refused"):
                server.listen(0)
            fillers = []
            drain_thread = None
            drain_errors = []
            process = subprocess.Popen([str(args.host.resolve()), str(args.library.resolve())],
                                       stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=stderr,
                                       bufsize=0, env=env, start_new_session=True)
            try:
                assert read_line(process) == f"ready {process.pid}"
                if "backlog" in case:
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
                        raise AssertionError("could not fill the bounded listener backlog")
                    assert fillers
                    evidence["backlog_occupants"] = len(fillers)

                    def drain():
                        """Release all known queued clients without accepting the runtime connection."""
                        try:
                            for _ in fillers:
                                peer, _ = server.accept()
                                peer.close()
                        except Exception as error:
                            drain_errors.append(repr(error))

                    if case == "infinite-backlog":
                        def delayed_drain():
                            time.sleep(0.1)
                            drain()
                        drain_thread = threading.Thread(target=delayed_drain)
                        drain_thread.start()
                        exchange(process, endpoint, 0xffffffff, "connect-result 1 0 0", evidence)
                        drain_thread.join(timeout=3)
                        assert not drain_thread.is_alive() and not drain_errors, drain_errors
                        consume(server)
                    else:
                        for _ in range(5):
                            exchange(process, endpoint, 0 if case == "zero-backlog" else 100,
                                     f"connect-result 0 0 {errno.EAGAIN}", evidence)
                        drain()
                        assert not drain_errors, drain_errors
                        exchange(process, endpoint, 100, "connect-result 1 0 0", evidence)
                        consume(server)
                elif case in ("missing", "refused"):
                    code = errno.ENOENT if case == "missing" else errno.ECONNREFUSED
                    exchange(process, endpoint, 100, f"connect-result 0 0 {code}", evidence)
                else:
                    for timeout in (0, 100, 0xffffffff):
                        exchange(process, endpoint, timeout, "connect-result 1 0 0", evidence)
                        consume(server)
                process.stdin.write(b"q\n")
                process.wait(timeout=3)
                assert process.returncode == 0
                assert stderr_path.stat().st_size == 0
            finally:
                if process.poll() is None:
                    os.killpg(process.pid, signal.SIGKILL)
                    process.wait(timeout=3)
                if drain_thread is not None:
                    drain_thread.join(timeout=3)
                for filler in fillers:
                    filler.close()
                evidence["exit"] = process.returncode
                (args.output / f"{case}.json").write_text(json.dumps(evidence, indent=2) + "\n")
    print(f"{case}: passed", flush=True)


def main():
    """Save before/after evidence in a new directory for each run."""
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", type=Path, required=True)
    parser.add_argument("--library", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--case", choices=["all"] + CASES, default="all")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    for case in CASES if args.case == "all" else [args.case]:
        run_case(args, case)


if __name__ == "__main__":
    main()
