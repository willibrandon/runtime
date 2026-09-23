"""Verify tracing request ownership through the actual diagnostic socket protocol."""

import argparse
import array
import concurrent.futures
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
from listener import command, encoded_string, listener_threads, pause, resume, status_response
from run import HEADER, MAGIC, process_info, receive


CASES = [f"collect-{version}-malformed" for version in range(1, 6)] + [
    "stop-empty", "stop-short", "stop-long", "stop-unknown", "descriptor-eof",
    "descriptor-extra", "descriptor-invalid", "descriptor-pause", "start-response-pause",
    "active-writer-pause", "active-sampling-pause", "session-lifetime", "session-capacity",
    "allocation-file", "allocation-serializer", "allocation-session"]
BAD_ENCODING = 0x80131384
FAIL = 0x80004005


def request(peer, command_id, body):
    """Send a complete frame; continuation bytes are sent separately."""
    peer.sendall(HEADER.pack(MAGIC, HEADER.size + len(body), 2, command_id, 0) + body)


def collect_body(user_events=False, sampling=False):
    """Encode CollectTracing5 without sampling, rundown, or stack walking."""
    provider_name = "Microsoft-DotNETCore-SampleProfiler" if sampling else "Ankus-Ownership-Probe"
    provider = struct.pack("<QI", 0, 5) + encoded_string(provider_name)
    provider += struct.pack("<IBI", 0, 0, 0)  # No arguments; exclude no event IDs.
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


def pause_once(process, evidence):
    """Pause at the current boundary and prove the listener thread has retired."""
    before = listener_threads(process.pid)
    response = command(process, "p")
    assert response.startswith("listener "), response
    count = int(response.split()[1])
    deadline = time.monotonic() + 2
    remaining = listener_threads(process.pid)
    while remaining:
        snapshots = {}
        for tid in remaining:
            try:
                snapshot = Path(f"/proc/{process.pid}/task/{tid}/stat").read_text()
                flags = int(snapshot.rpartition(") ")[2].split()[6])
                assert flags & 4, "joined listener has not entered kernel exit"
                snapshots[str(tid)] = snapshot
            except FileNotFoundError:
                snapshots[str(tid)] = "exited before stat read"
        evidence.setdefault("kernel_exit_observations", []).append(snapshots)
        assert time.monotonic() < deadline, "joined listener remained in the kernel task list"
        time.sleep(0.001)
        remaining = listener_threads(process.pid)
    evidence["checkpoints"].append({"received": count, "retired_threads": before})
    return count


def set_send_blocked(process, blocked):
    """Control the fixture's link-time send boundary without changing runtime code."""
    expected = 1 if blocked else 0
    assert command(process, f"w {expected}") == f"send-blocked {expected}"


def process_threads(process):
    """Return every live task and its kernel-visible name."""
    tasks = {}
    for path in Path(f"/proc/{process.pid}/task").glob("*/comm"):
        try:
            tasks[int(path.parent.name)] = path.read_text().strip()
        except FileNotFoundError:
            pass
    return tasks


def wait_process_threads(process, expected):
    """Wait for the exact process task count and retain their identities."""
    deadline = time.monotonic() + 3
    while time.monotonic() < deadline:
        threads = process_threads(process)
        if len(threads) == expected:
            return threads
        time.sleep(0.002)
    raise AssertionError(f"expected {expected} process threads: {process_threads(process)}")


def run_active_worker_pause(process, endpoint, evidence, sampling):
    """Stop and recreate a live trace writer and optional sampler without ending its session."""
    baseline = process_threads(process)
    with connect(endpoint) as peer:
        request(peer, 6, collect_body(sampling=sampling))
        session_id = session_reply(peer)
        with concurrent.futures.ThreadPoolExecutor(max_workers=1) as readers:
            reader = readers.submit(drain_trace, peer)
            expected = len(baseline) + (2 if sampling else 1)
            before = wait_process_threads(process, expected)
            if sampling:
                assert command(process, "g 50") == "managed-work 0"

            cycles = []
            for index in range(20):
                assert command(process, "f 1") == "fork-checkpoint 1"
                paused = wait_process_threads(process, len(baseline) - 1)
                assert command(process, "f 0") == "fork-checkpoint 1"
                after = wait_process_threads(process, expected)
                cycles.append({"index": index, "paused": paused, "resumed": after})
                if sampling:
                    assert command(process, "g 10") == "managed-work 0"

            stop(endpoint, session_id)
            bytes_received = reader.result(timeout=4)
            if sampling:
                assert bytes_received > 738, "sampling thread produced no trace events"

            evidence["requests"].append({
                "session": session_id,
                "sampling": sampling,
                "threads_baseline": baseline,
                "threads_before": before,
                "cycles": cycles,
                "bytes": bytes_received,
            })


def run_descriptor_pause(process, endpoint, evidence):
    """Pause repeatedly while CollectTracing waits for its ancillary descriptor."""
    body = collect_body(True)
    with connect(endpoint) as peer:
        request(peer, 6, body)
        expected = HEADER.size + len(body)
        pause(process, expected, evidence)
        assert command(process, "o") == "pending 0"
        resume(process)
        pause(process, expected, evidence)
        assert command(process, "o") == "pending 0"
        with open("/dev/null", "rb") as source:
            peer.sendmsg([b"x"], [(socket.SOL_SOCKET, socket.SCM_RIGHTS,
                                    array.array("i", [source.fileno()]))])
        assert not select.select([peer], [], [], 0.1)[0], "paused command produced a response"
        resume(process)
        status_response(peer, FAIL)
    evidence["requests"].append({"bytes": expected, "pauses": 2})


def run_start_response_pause(process, endpoint, evidence):
    """Pause a successful trace after enable but before its start reply is delivered."""
    body = collect_body()
    blocked = False
    paused = False
    with connect(endpoint) as peer:
        try:
            set_send_blocked(process, True)
            blocked = True
            request(peer, 6, body)
            deadline = time.monotonic() + 2
            while True:
                time.sleep(0.005)
                pause_once(process, evidence)
                paused = True
                response = command(process, "o")
                assert response.startswith("pending "), response
                pending = int(response.split()[1])
                evidence.setdefault("pending_responses", []).append(pending)
                if pending == HEADER.size + 8:
                    break
                assert time.monotonic() < deadline, "start response did not reach the resumable handoff"
                resume(process)
                paused = False

            assert not select.select([peer], [], [], 0.1)[0], "paused start response reached the client"
            set_send_blocked(process, False)
            blocked = False
            resume(process, 2)
            paused = False
            with concurrent.futures.ThreadPoolExecutor(max_workers=1) as readers:
                session_id = session_reply(peer)
                reader = readers.submit(drain_trace, peer)
                stop(endpoint, session_id)
                evidence["requests"].append({"session": session_id, "bytes": reader.result(timeout=4)})
        finally:
            if blocked and process.poll() is None:
                set_send_blocked(process, False)
            if paused and process.poll() is None:
                resume(process)


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
                if case == "descriptor-pause":
                    run_descriptor_pause(process, endpoint, evidence)
                elif case == "start-response-pause":
                    run_start_response_pause(process, endpoint, evidence)
                elif case == "active-writer-pause":
                    run_active_worker_pause(process, endpoint, evidence, False)
                elif case == "active-sampling-pause":
                    run_active_worker_pause(process, endpoint, evidence, True)
                elif case.startswith("session-"):
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
