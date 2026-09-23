"""Retire and restart the real listener while preserving partial IPC requests."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import select
import signal
import socket
import struct
import subprocess
import tempfile
import time

from connect import read_line
from run import HEADER, MAGIC, process_info, process_info_response, process_info_stream, receive


CASES = ["idle", "partial-header", "partial-payload", "header-eof", "payload-eof", "invalid-size", "invalid-magic",
         "reverse-header", "reverse-payload"]


def command(process, value):
    """Run a native lifecycle action while the diagnostics thread is independent."""
    process.stdin.write((value + "\n").encode())
    return read_line(process)


def listener_threads(pid):
    """Observe actual kernel threads, not a runtime-reported success flag."""
    result = []
    for path in Path(f"/proc/{pid}/task").glob("*/comm"):
        try:
            if path.read_text().strip() == ".NET EventPipe":
                result.append(int(path.parent.name))
        except FileNotFoundError:
            pass  # A thread can finish between directory enumeration and open.
    return result


def resume(process):
    """Wait for the replacement listener to start before the next observation."""
    assert command(process, "r") == "listener 0"
    deadline = time.monotonic() + 2
    while time.monotonic() < deadline:
        threads = listener_threads(process.pid)
        if len(threads) == 1:
            return threads[0]
        time.sleep(0.002)
    raise AssertionError("replacement listener did not start")


def pause(process, expected, evidence):
    """Require both real thread retirement and retention of already consumed bytes."""
    deadline = time.monotonic() + 2
    while time.monotonic() < deadline:
        before = listener_threads(process.pid)
        response = command(process, "p")
        assert response.startswith("listener "), response
        count = int(response.split()[1])
        assert not listener_threads(process.pid), "listener thread survived the join"
        assert count <= expected, (count, expected)
        if count == expected:
            evidence["checkpoints"].append({"received": count, "retired_threads": before})
            return
        # Do not assume a fragment was consumed merely because send returned.
        # Resume until the paused reader's actual count proves that boundary.
        resume(process)
        time.sleep(0.002)
    raise AssertionError(f"reader did not consume {expected} bytes")


def encoded_string(value):
    """Encode the documented counted UTF-16 protocol representation."""
    data = value.encode("utf-16-le") + b"\0\0"
    return struct.pack("<I", len(data) // 2) + data


def status_response(peer, expected_code):
    """Read the complete success/error reply with no unconsumed trailing bytes."""
    magic, size, command_set, command_id, reserved = HEADER.unpack(receive(peer, HEADER.size))
    assert (magic, size, command_set, command_id, reserved) == (MAGIC, 24, 255, 0 if expected_code == 0 else 255, 0)
    code = struct.unpack("<I", receive(peer, 4))[0]
    assert code == expected_code, (code, expected_code)
    assert peer.recv(1) == b"", "response connection was not closed"
    return code


def accept_reverse(monitor, pid, identity):
    """Check the runtime's real reverse-connection advertisement before sending data."""
    peer, _ = monitor.accept()
    try:
        peer.settimeout(3)
        magic, cookie, advertised_pid, reserved = struct.unpack("<8s16sQH", receive(peer, 34))
        assert (magic, advertised_pid, reserved) == (b"ADVR_V1\0", pid, 0)
        assert cookie.hex() == identity["cookie"]
        return peer
    except BaseException:
        peer.close()
        raise


def run_case(args, case):
    """Exercise one listener state in its own process and temporary socket directory."""
    evidence = {"case": case, "checkpoints": []}
    with tempfile.TemporaryDirectory(prefix="diag-listener-", dir="/tmp") as directory:
        env = dict(os.environ)
        for name in list(env):
            if name.startswith(("DOTNET_", "COMPlus_")) and any(
                    word in name for word in ["Diagnostic", "EventPipe", "gcServer", "gcConcurrent"]):
                env.pop(name)
        env.pop("ANKUS_LISTENER_CHECKPOINT", None)
        env["TMPDIR"] = directory
        stderr_path = args.output / f"{case}.stderr.log"
        with socket.socket(socket.AF_UNIX) as monitor, stderr_path.open("w") as stderr:
            is_reverse = case.startswith("reverse-")
            if is_reverse:
                monitor_path = Path(directory) / "monitor.socket"
                monitor.bind(str(monitor_path))
                monitor.listen(2)
                monitor.settimeout(3)
                env["DOTNET_DiagnosticPorts"] = f"{monitor_path},connect,nosuspend"
            process = subprocess.Popen([str(args.host.resolve()), str(args.library.resolve())],
                                       stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=stderr,
                                       bufsize=0, env=env, start_new_session=True)
            try:
                assert read_line(process) == f"ready {process.pid}"
                endpoints = list(Path(directory).glob(f"dotnet-diagnostic-{process.pid}-*-socket"))
                assert len(endpoints) == 1
                endpoint = endpoints[0]
                inode = endpoint.stat().st_ino
                identity = process_info(endpoint, process.pid)
                assert command(process, "v") == "environment <unset>"
                if case == "idle":
                    pause(process, 0, evidence)
                    baseline_fds = len(list(Path(f"/proc/{process.pid}/fd").iterdir()))
                    for _ in range(20):
                        assert command(process, "p") == "listener 0"  # Pausing twice is harmless.
                        with socket.socket(socket.AF_UNIX) as peer:
                            peer.settimeout(3)
                            peer.connect(str(endpoint))
                            peer.sendall(HEADER.pack(MAGIC, HEADER.size, 4, 4, 0))
                            assert not select.select([peer], [], [], 0.01)[0], "paused listener answered a request"
                            resume(process)
                            assert process_info_response(peer, process.pid) == identity
                        pause(process, 0, evidence)
                        assert len(list(Path(f"/proc/{process.pid}/fd").iterdir())) == baseline_fds
                    resume(process)
                else:
                    value = "saved-λ-🌲-" + "0123456789" * 1024
                    body = encoded_string("ANKUS_LISTENER_CHECKPOINT") + encoded_string(value)
                    is_payload = case in ("partial-payload", "payload-eof", "reverse-payload")
                    packet = HEADER.pack(MAGIC, HEADER.size + (len(body) if is_payload else 0), 4, 3 if is_payload else 4, 0)
                    if is_payload:
                        packet += body
                        boundaries = [1, 7, 19, 20, 21, 23, len(packet) // 2, len(packet) - 1]
                    else:
                        boundaries = [1, 7, 19]
                    if case == "invalid-size":
                        packet = HEADER.pack(MAGIC, HEADER.size - 1, 4, 4, 0)
                    elif case == "invalid-magic":
                        packet = b"?" + packet[1:]
                    peer = accept_reverse(monitor, process.pid, identity) if is_reverse else socket.socket(socket.AF_UNIX)
                    with peer:
                        peer.settimeout(3)
                        if is_reverse:
                            pause(process, 0, evidence)
                            resume(process)
                        else:
                            peer.connect(str(endpoint))
                        offset = 0
                        for boundary in boundaries:
                            peer.sendall(packet[offset:boundary])
                            offset = boundary
                            pause(process, boundary, evidence)
                            assert endpoint.stat().st_ino == inode
                            assert not select.select([peer], [], [], 0.01)[0], "partial request was dispatched"
                            assert command(process, "v") == "environment <unset>"
                            if boundary != boundaries[-1]:
                                resume(process)
                        if case.endswith("-eof"):
                            peer.shutdown(socket.SHUT_WR)
                        else:
                            peer.sendall(packet[offset:])
                        assert not select.select([peer], [], [], 0.01)[0], "paused listener dispatched the completed input"
                        assert command(process, "v") == "environment <unset>"
                        resume(process)
                        if case in ("partial-header", "reverse-header"):
                            assert process_info_response(peer, process.pid) == identity
                        else:
                            expected_code = 0 if case in ("partial-payload", "reverse-payload") else 0x80131386 if case == "invalid-magic" else 0x80131384
                            evidence["status"] = status_response(peer, expected_code)
                    if case in ("partial-payload", "reverse-payload"):
                        assert command(process, "v") == "environment " + value
                        evidence["value_sha256"] = hashlib.sha256(value.encode()).hexdigest()
                    else:
                        assert command(process, "v") == "environment <unset>"
                    if is_reverse:
                        with accept_reverse(monitor, process.pid, identity) as reconnected:
                            assert process_info_stream(reconnected, process.pid) == identity
                        evidence["reconnected"] = True
                assert process_info(endpoint, process.pid) == identity
                assert endpoint.stat().st_ino == inode
                evidence["identity"] = identity
                process.stdin.write(b"q\n")
                process.wait(timeout=3)
                assert process.returncode == 0 and not endpoint.exists()
                assert stderr_path.stat().st_size == 0
            finally:
                if process.poll() is None:
                    os.killpg(process.pid, signal.SIGKILL)
                    process.wait(timeout=3)
                evidence["exit"] = process.returncode
                (args.output / f"{case}.json").write_text(json.dumps(evidence, indent=2) + "\n")
    print(f"{case}: passed ({len(evidence['checkpoints'])} retired checkpoints)", flush=True)


def main():
    """Require a new evidence directory and keep each host under a supervisor deadline."""
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
