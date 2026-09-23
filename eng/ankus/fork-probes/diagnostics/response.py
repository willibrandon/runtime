"""Preserve actual diagnostic responses across listener retirement and restart."""

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

from connect import read_line
from listener import accept_reverse, command, pause, resume
from run import HEADER, MAGIC, process_info, receive


CASES = ["blocked", "repeated", "half-close", "peer-close", "reverse", "complete", "lifetime", "native-children"]


def begin_environment(peer):
    """Read the advertised continuation size through the real process protocol."""
    peer.sendall(HEADER.pack(MAGIC, HEADER.size, 4, 2, 0))
    header = HEADER.unpack(receive(peer, HEADER.size))
    assert header == (MAGIC, HEADER.size + 6, 255, 0, 0), header
    length, reserved = struct.unpack("<IH", receive(peer, 6))
    assert 1_000_000 < length < 2_000_000 and reserved == 0, (length, reserved)
    return length


def decode_environment(body, expected):
    """Check every field boundary and exact value of all deliberately large entries."""
    count = struct.unpack_from("<I", body)[0]
    offset = 4
    values = {}
    for _ in range(count):
        length = struct.unpack_from("<I", body, offset)[0]
        offset += 4
        assert length > 0 and offset + 2 * length <= len(body)
        value = body[offset:offset + 2 * length].decode("utf-16-le")
        assert value.endswith("\0") and "\0" not in value[:-1]
        name, content = value[:-1].split("=", 1)
        assert name not in values, name
        values[name] = content
        offset += 2 * length
    assert offset == len(body)
    for name, value in expected.items():
        assert values[name] == value, name
    return hashlib.sha256(body).hexdigest()


def pause_output(process, peer, body, total, evidence):
    """Account for bytes already sent separately from the runtime's remaining bytes."""
    pause(process, 0, evidence)
    response = command(process, "o")
    assert response.startswith("pending "), response
    remaining = int(response.split()[1])
    peer.setblocking(False)
    try:
        while True:
            try:
                block = peer.recv(65536)
            except BlockingIOError:
                break
            assert block, "server closed the unfinished response"
            body.extend(block)
        assert len(body) + remaining == total, (len(body), remaining, total)
        assert remaining > 0, "test did not reach a blocked response"
        assert not select.select([peer], [], [], 0.01)[0], "retired listener continued writing"
    finally:
        peer.settimeout(3)
    evidence["output_checkpoints"].append({"received": len(body), "pending": remaining})
    return remaining


def read_environment(endpoint, expected):
    """Read a fresh response after recovery and ensure the connection is released."""
    with socket.socket(socket.AF_UNIX) as peer:
        peer.settimeout(3)
        peer.connect(str(endpoint))
        size = begin_environment(peer)
        body = receive(peer, size)
        assert peer.recv(1) == b""
        return decode_environment(body, expected)


def check_identity_version(endpoint, identity, command_id):
    """Verify all three ProcessInfo formats that own command-line snapshots."""
    with socket.socket(socket.AF_UNIX) as peer:
        peer.settimeout(3)
        peer.connect(str(endpoint))
        peer.sendall(HEADER.pack(MAGIC, HEADER.size, 4, command_id, 0))
        magic, size, command_set, response_id, reserved = HEADER.unpack(receive(peer, HEADER.size))
        assert (magic, command_set, response_id, reserved) == (MAGIC, 255, 0, 0)
        body = receive(peer, size - HEADER.size)
        offset = 0
        if command_id == 8:
            assert struct.unpack_from("<I", body)[0] == 1
            offset = 4
        assert struct.unpack_from("<Q", body, offset)[0] == identity["pid"]
        assert body[offset + 8:offset + 24].hex() == identity["cookie"]
        offset += 24
        fields = []
        for _ in range({0: 3, 4: 5, 8: 6}[command_id]):
            length = struct.unpack_from("<I", body, offset)[0]
            offset += 4
            assert length > 0 and offset + 2 * length <= len(body)
            value = body[offset:offset + 2 * length].decode("utf-16-le")
            assert value.endswith("\0") and "\0" not in value[:-1]
            fields.append(value[:-1])
            offset += 2 * length
        expected = identity["fields"][:3] if command_id == 0 else identity["fields"]
        if command_id == 8:
            expected = expected + ["linux-x64"]
        assert fields == expected, fields
        assert offset == len(body) and peer.recv(1) == b""


def check_lifetime(process, endpoint, expected, identity, evidence):
    """Observe libc's live allocation accounting after successful and failed replies."""
    samples = []
    for index in range(68):
        if index % 2 == 0:
            read_environment(endpoint, expected)
        else:
            with socket.socket(socket.AF_UNIX) as peer:
                peer.settimeout(3)
                peer.connect(str(endpoint))
                total = begin_environment(peer)
                pause_output(process, peer, bytearray(), total, evidence)
            resume(process)
        check_identity_version(endpoint, identity, [0, 4, 8][index % 3])
        pause(process, 0, evidence)
        assert command(process, "o") == "pending 0"
        observation = command(process, "a")
        assert observation.startswith("allocated "), observation
        if index >= 4:
            samples.append(int(observation.split()[1]))
        resume(process)
    evidence["live_native_allocations"] = samples
    evidence["live_native_growth"] = max(samples) - min(samples)
    assert max(samples) - min(samples) < 4096, "completed responses retained native allocations"


def run_case(args, case):
    """Own all processes and sockets, including a stalled client of the old runtime."""
    evidence = {"case": case, "checkpoints": [], "output_checkpoints": []}
    with tempfile.TemporaryDirectory(prefix="diag-response-", dir="/tmp") as directory:
        env = dict(os.environ)
        for name in list(env):
            if name.startswith(("DOTNET_", "COMPlus_")) and any(
                    word in name for word in ["Diagnostic", "EventPipe", "gcServer", "gcConcurrent"]):
                env.pop(name)
        expected = {f"ANKUS_RESPONSE_{index:02}": f"{index}-λ-🌲-" + "0123456789" * 2048 for index in range(32)}
        expected["ANKUS_RESPONSE_CHANGE"] = "before"
        env.update(expected)
        env["TMPDIR"] = directory
        stderr_path = args.output / f"{case}.stderr.log"
        with socket.socket(socket.AF_UNIX) as monitor, stderr_path.open("w") as stderr:
            if case == "reverse":
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
                evidence["identity"] = identity
                fd_count = len(list(Path(f"/proc/{process.pid}/fd").iterdir()))
                evidence["fds_before"] = fd_count
                peer = accept_reverse(monitor, process.pid, identity) if case == "reverse" else socket.socket(socket.AF_UNIX)
                with peer:
                    peer.settimeout(3)
                    if case != "reverse":
                        peer.connect(str(endpoint))
                    total = begin_environment(peer)
                    body = bytearray()
                    if case == "complete":
                        body.extend(receive(peer, total))
                        assert peer.recv(1) == b""
                        pause(process, 0, evidence)
                        assert command(process, "o") == "pending 0"
                    else:
                        pending = pause_output(process, peer, body, total, evidence)
                        if case == "repeated":
                            for _ in range(3):
                                resume(process)
                                body.extend(receive(peer, 1024))
                                current = pause_output(process, peer, body, total, evidence)
                                assert current < pending, "restarted writer made no progress"
                                pending = current
                    assert command(process, "m") == "changed 0"
                    if case == "native-children":
                        evidence["children"] = []
                        for operation in ["c", "n"]:
                            result = command(process, operation)
                            assert result.startswith("child-closed "), result
                            evidence["children"].append(int(result.split()[1]))
                            assert command(process, "o") == f"pending {pending}"
                            assert endpoint.stat().st_ino == inode
                    if case == "peer-close":
                        peer.close()
                    elif case == "half-close":
                        peer.shutdown(socket.SHUT_WR)
                    resume(process)
                    if case != "peer-close":
                        body.extend(receive(peer, total - len(body)))
                        assert peer.recv(1) == b"", "response socket was not released"
                        evidence["response_sha256"] = decode_environment(body, expected)
                        evidence["response_length"] = len(body)
                assert process_info(endpoint, process.pid) == identity
                assert endpoint.stat().st_ino == inode
                expected["ANKUS_RESPONSE_CHANGE"] = "after"
                evidence["fresh_response_sha256"] = read_environment(endpoint, expected)
                # Pausing joins the thread and settles ownership before counting.
                pause(process, 0, evidence)
                assert command(process, "o") == "pending 0"
                evidence["fds_after"] = len(list(Path(f"/proc/{process.pid}/fd").iterdir()))
                assert evidence["fds_after"] == fd_count, (fd_count, evidence["fds_after"])
                resume(process)
                assert process_info(endpoint, process.pid) == identity
                if case == "lifetime":
                    check_lifetime(process, endpoint, expected, identity, evidence)
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
    print(f"{case}: passed", flush=True)


def main():
    """Keep every attempted run and require a new evidence directory."""
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
