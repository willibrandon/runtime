"""Verify tracing request ownership through the actual diagnostic socket protocol."""

import argparse
import array
import concurrent.futures
import json
import os
from pathlib import Path
import signal
import socket
import struct
import subprocess
import tempfile

from connect import read_line
from listener import command, encoded_string, pause, resume, status_response
from run import HEADER, MAGIC, process_info, receive


CASES = [f"collect-{version}-malformed" for version in range(1, 6)] + [
    "stop-empty", "stop-short", "stop-long", "stop-unknown", "descriptor-eof",
    "descriptor-extra", "descriptor-invalid", "session-lifetime", "session-capacity",
    "allocation-file", "allocation-serializer", "allocation-session"]
BAD_ENCODING = 0x80131384
FAIL = 0x80004005


def request(peer, command_id, body):
    """Send a complete frame; continuation bytes are sent separately."""
    peer.sendall(HEADER.pack(MAGIC, HEADER.size + len(body), 2, command_id, 0) + body)


def collect_body(user_events=False):
    """Encode CollectTracing5 without sampling, rundown, or stack walking."""
    provider = struct.pack("<QI", 0, 5) + encoded_string("Ankus-Ownership-Probe")
    provider += struct.pack("<IBI", 0, 1, 0)  # No arguments or event-ID filter.
    if user_events:
        provider += encoded_string("ankus_ownership_probe") + struct.pack("<I", 0)
        return struct.pack("<IQI", 1, 0, 1) + provider
    return struct.pack("<IIIQBI", 0, 1, 1, 0, 0, 1) + provider


def connect(endpoint):
    """Keep failed calls bounded even when testing the unpatched runtime."""
    peer = socket.socket(socket.AF_UNIX)
    peer.settimeout(3)
    peer.connect(str(endpoint))
    return peer


def session_reply(peer):
    """Read the documented eight-byte session ID response."""
    assert HEADER.unpack(receive(peer, HEADER.size)) == (MAGIC, 28, 255, 0, 0)
    return struct.unpack("<Q", receive(peer, 8))[0]


def stop(endpoint, session_id):
    """Stop exactly the requested session and require reply connection cleanup."""
    with connect(endpoint) as peer:
        request(peer, 1, struct.pack("<Q", session_id))
        assert session_reply(peer) == session_id
        assert peer.recv(1) == b""


def drain_trace(peer):
    """Drain a real streaming session while its stop command flushes the file."""
    data = bytearray()
    while True:
        block = peer.recv(65536)
        if not block:
            break
        data.extend(block)
        assert len(data) < 1_000_000
    assert data.startswith(b"Nettrace"), data[:32]
    assert data.endswith(b"\x01"), "trace did not end with its serialization terminator"
    return len(data)


def probe_environment(directory):
    """Remove inherited runtime switches and place the socket in owned storage."""
    env = dict(os.environ)
    for name in list(env):
        if name.startswith(("DOTNET_", "COMPlus_")) and any(
                word in name for word in ["Diagnostic", "EventPipe", "gcServer", "gcConcurrent"]):
            env.pop(name)
    env["TMPDIR"] = directory
    return env


def parse_ownership(process, kind, fail_index):
    """Decode one native ownership observation without hiding individual fields."""
    response = command(process, f"b {kind} {fail_index}")
    fields = response.split()
    assert fields[:2] == ["ownership", "0"], response
    values = list(map(int, fields[2:]))
    assert len(values) == 6, values
    return values


def allocation_observation(args, case, kind, fail_index, label, evidence):
    """Use a fresh runtime so one-time allocations are included at every index."""
    with tempfile.TemporaryDirectory(prefix="diag-tracing-allocation-", dir="/tmp") as directory:
        stderr_path = args.output / f"{case}-{label}.stderr.log"
        with stderr_path.open("w") as stderr:
            process = subprocess.Popen([str(args.host.resolve()), str(args.library.resolve())],
                                       stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=stderr,
                                       bufsize=0, env=probe_environment(directory), start_new_session=True)
            observation = {
                "fail_index": fail_index,
                "process": process.pid,
                "result": None,
                "recovery": None,
                "checkpoints": [],
                "exit": None,
            }
            evidence["requests"].append(observation)
            try:
                assert read_line(process) == f"ready {process.pid}"
                endpoints = list(Path(directory).glob(f"dotnet-diagnostic-{process.pid}-*-socket"))
                assert len(endpoints) == 1
                endpoint = endpoints[0]
                identity = process_info(endpoint, process.pid)
                fd_count = len(list(Path(f"/proc/{process.pid}/fd").iterdir()))
                values = parse_ownership(process, kind, fail_index)
                observation["result"] = values
                assert values[3:5] == [0, 1], (
                    "resource freed before ownership transfer or freed more than once")
                assert process_info(endpoint, process.pid) == identity
                recovery = None
                if fail_index >= 0:
                    assert values[1] == 1, "allocation failure was not injected"
                    if kind < 2:
                        assert values[2] == 0, "failed allocation was reported as successful initialization"
                    recovery = parse_ownership(process, kind, -1)
                    assert recovery[1:3] == [0, 1], "operation did not recover after allocation failure"
                    assert process_info(endpoint, process.pid) == identity
                checkpoint = {"checkpoints": []}
                pause(process, 0, checkpoint)
                assert len(list(Path(f"/proc/{process.pid}/fd").iterdir())) == fd_count
                resume(process)
                assert command(process, "s") == "shutdown 1"
                assert not endpoint.exists()
                observation["recovery"] = recovery
                observation["checkpoints"] = checkpoint["checkpoints"]
            finally:
                if process.poll() is None:
                    process.stdin.write(b"q\n")
                    try:
                        process.communicate(timeout=5)
                    except subprocess.TimeoutExpired:
                        os.killpg(process.pid, signal.SIGKILL)
                        process.communicate(timeout=5)
                observation["exit"] = process.returncode
            assert process.returncode == 0
            assert stderr_path.stat().st_size == 0
    return values


def run_allocation_case(args, case):
    """Fail every observed allocation in isolation, then prove immediate recovery."""
    kind = {"allocation-file": 0, "allocation-serializer": 1, "allocation-session": 2}[case]
    evidence = {"case": case, "requests": []}
    try:
        baseline = allocation_observation(args, case, kind, -1, "baseline", evidence)
        assert baseline[0] > 0 and baseline[1:3] == [0, 1], baseline
        for fail_index in range(baseline[0]):
            allocation_observation(args, case, kind, fail_index, f"failure-{fail_index}", evidence)
    finally:
        (args.output / f"{case}.json").write_text(json.dumps(evidence, indent=2) + "\n")
    print(f"tracing {case} passed", flush=True)


def run_case(args, case):
    """Check each request's response, cleanup, and subsequent process query."""
    evidence = {"case": case, "checkpoints": [], "requests": []}
    with tempfile.TemporaryDirectory(prefix="diag-tracing-", dir="/tmp") as directory:
        with (args.output / f"{case}.stderr.log").open("w") as stderr:
            process = subprocess.Popen([str(args.host.resolve()), str(args.library.resolve())],
                                       stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=stderr,
                                       bufsize=0, env=probe_environment(directory), start_new_session=True)
            evidence["pid"] = process.pid
            try:
                assert read_line(process) == f"ready {process.pid}"
                endpoints = list(Path(directory).glob(f"dotnet-diagnostic-{process.pid}-*-socket"))
                assert len(endpoints) == 1
                endpoint = endpoints[0]
                identity = process_info(endpoint, process.pid)
                fd_count = len(list(Path(f"/proc/{process.pid}/fd").iterdir()))
                evidence["fds_before"] = fd_count
                if case.startswith("session-"):
                    count = 64 if case == "session-capacity" else 1
                    rounds = 1 if count == 64 else 20
                    for _ in range(rounds):
                        peers = []
                        try:
                            with concurrent.futures.ThreadPoolExecutor(max_workers=count) as readers:
                                sessions = []
                                for _ in range(count):
                                    peer = connect(endpoint)
                                    peers.append(peer)
                                    request(peer, 6, collect_body())
                                    session_id = session_reply(peer)
                                    assert session_id != 0 and session_id not in [item[0] for item in sessions]
                                    sessions.append((session_id, readers.submit(drain_trace, peer)))
                                if count == 64:
                                    # Admission fails before a session can take ownership of the received FD.
                                    with connect(endpoint) as peer, open("/dev/null", "rb") as source:
                                        request(peer, 6, collect_body(True))
                                        peer.sendmsg([b"x"], [(socket.SOL_SOCKET, socket.SCM_RIGHTS,
                                                              array.array("i", [source.fileno()]))])
                                        status_response(peer, FAIL)
                                for session_id, reader in sessions:
                                    stop(endpoint, session_id)
                                    evidence["requests"].append({"session": session_id, "bytes": reader.result(timeout=4)})
                        finally:
                            for peer in peers:
                                peer.close()
                else:
                    for index in range(16):
                        with connect(endpoint) as peer:
                            if case.startswith("collect-"):
                                version = int(case.split("-")[1])
                                request(peer, version + 1, b"\0")
                                status_response(peer, BAD_ENCODING)
                            elif case.startswith("stop-"):
                                length = {"stop-empty": 0, "stop-short": 7, "stop-long": 9, "stop-unknown": 8}[case]
                                request(peer, 1, bytes(length))
                                if length == 8:
                                    assert session_reply(peer) == 0
                                    assert peer.recv(1) == b""
                                else:
                                    status_response(peer, BAD_ENCODING)
                            else:
                                request(peer, 6, collect_body(True))
                                if case == "descriptor-eof":
                                    peer.shutdown(socket.SHUT_WR)
                                else:
                                    with open("/dev/null", "rb") as source:
                                        count = 2 if case == "descriptor-extra" else 1
                                        peer.sendmsg([b"x"], [(socket.SOL_SOCKET, socket.SCM_RIGHTS,
                                                              array.array("i", [source.fileno()] * count))])
                                status_response(peer, FAIL if case == "descriptor-invalid" else BAD_ENCODING)
                        assert process_info(endpoint, process.pid) == identity
                        evidence["requests"].append(index)
                assert process_info(endpoint, process.pid) == identity
                pause(process, 0, evidence)
                evidence["fds_after"] = len(list(Path(f"/proc/{process.pid}/fd").iterdir()))
                assert evidence["fds_after"] == fd_count, evidence
                resume(process)
                assert command(process, "s") == "shutdown 1"
                assert not endpoint.exists()
            finally:
                if process.poll() is None:
                    process.stdin.write(b"q\n")
                    try:
                        process.communicate(timeout=5)
                    except subprocess.TimeoutExpired:
                        os.killpg(process.pid, signal.SIGKILL)
                        process.communicate(timeout=5)
                evidence["exit"] = process.returncode
                (args.output / f"{case}.json").write_text(json.dumps(evidence, indent=2) + "\n")
            assert process.returncode == 0
            assert (args.output / f"{case}.stderr.log").stat().st_size == 0
    print(f"tracing {case} passed", flush=True)


def main():
    """Run independently selectable request failures and session lifetime checks."""
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", type=Path, required=True)
    parser.add_argument("--library", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--case", choices=["all", *CASES], default="all")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    for case in CASES if args.case == "all" else [args.case]:
        if case.startswith("allocation-"):
            run_allocation_case(args, case)
        else:
            run_case(args, case)


if __name__ == "__main__":
    main()
