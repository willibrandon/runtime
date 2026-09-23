"""Verify parent and child diagnostics after a real Native AOT fork."""

import argparse
import concurrent.futures
import json
import os
from pathlib import Path
import signal
import subprocess
import tempfile
import time

from connect import read_line
from listener import command
from run import process_info
from tracing import (collect_body, connect, drain_trace, probe_environment, request,
                     session_reply, set_send_blocked, set_send_limit, stop, wait_blocked_send)


def endpoint_for(directory, pid):
    """Wait for exactly one PID-specific diagnostic endpoint."""
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        endpoints = list(Path(directory).glob(f"dotnet-diagnostic-{pid}-*-socket"))
        if len(endpoints) == 1:
            return endpoints[0]
        time.sleep(0.005)
    raise AssertionError(f"diagnostic endpoint for {pid} was not created")


def socket_descriptors(pid):
    """Return the process's kernel socket identities."""
    sockets = set()
    for descriptor in Path(f"/proc/{pid}/fd").iterdir():
        try:
            target = descriptor.readlink()
        except FileNotFoundError:
            continue
        value = str(target)
        if value.startswith("socket:["):
            sockets.add(value)
    return sockets


def main():
    """Run one bounded parent/child trace lifecycle."""
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", type=Path, required=True)
    parser.add_argument("--library", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--rounds", type=int, default=3)
    args = parser.parse_args()
    assert args.rounds > 0
    args.output.mkdir(parents=True, exist_ok=False)
    evidence = {}
    with tempfile.TemporaryDirectory(prefix="diag-fork-", dir="/tmp") as directory:
        stderr_path = args.output / "stderr.log"
        with stderr_path.open("w") as stderr:
            process = subprocess.Popen(
                [str(args.host.resolve()), str(args.library.resolve())],
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                stderr=stderr,
                bufsize=0,
                env=probe_environment(directory),
                start_new_session=True)
            try:
                assert read_line(process) == f"ready {process.pid}"
                parent_endpoint = endpoint_for(directory, process.pid)
                parent_identity = process_info(parent_endpoint, process.pid)
                assert command(process, "e") == "enable 1"

                with connect(parent_endpoint) as parent_peer:
                    request(parent_peer, 6, collect_body(sampling=True))
                    parent_session = session_reply(parent_peer)
                    with concurrent.futures.ThreadPoolExecutor(max_workers=2) as readers:
                        parent_reader = readers.submit(drain_trace, parent_peer)
                        children = []
                        evidence.update({
                            "parent_pid": process.pid,
                            "parent_identity": parent_identity,
                            "children": children,
                        })
                        for index in range(args.rounds):
                            set_send_limit(process, 32)
                            assert command(process, "g 250") == "managed-work 0"
                            blocked_attempts, constrained_bytes = wait_blocked_send(process)
                            assert constrained_bytes == 32
                            parent_sockets = socket_descriptors(process.pid)
                            response = command(process, "z")
                            fields = response.split()
                            assert fields[0] == "fork-child" and fields[2] == "0", response
                            child_pid = int(fields[1])
                            assert child_pid != process.pid
                            set_send_blocked(process, False)

                            child_endpoint = endpoint_for(directory, child_pid)
                            child_identity = process_info(child_endpoint, child_pid)
                            child_evidence = {
                                "pid": child_pid,
                                "identity": child_identity,
                                "stage": "endpoint-ready",
                            }
                            children.append(child_evidence)
                            assert child_identity["cookie"] != parent_identity["cookie"]
                            assert process_info(parent_endpoint, process.pid) == parent_identity
                            child_sockets_before_trace = socket_descriptors(child_pid)
                            inherited_sockets = parent_sockets & child_sockets_before_trace
                            assert not inherited_sockets, inherited_sockets

                            with connect(child_endpoint) as child_peer:
                                request(child_peer, 6, collect_body(sampling=True))
                                child_session = session_reply(child_peer)
                                child_evidence["stage"] = "trace-started"
                                child_reader = readers.submit(drain_trace, child_peer)
                                assert command(process, "h") == f"child-work {child_pid} 0"
                                child_evidence["stage"] = "managed-work-complete"
                                assert command(process, "g 50") == "managed-work 0"
                                stop(child_endpoint, child_session)
                                child_trace = child_reader.result(timeout=5)

                            child_path = args.output / f"child-{index + 1}.nettrace"
                            child_path.write_bytes(child_trace)
                            assert command(process, "j") == f"child-stopped {child_pid} 0 0"
                            deadline = time.monotonic() + 2
                            while child_endpoint.exists() and time.monotonic() < deadline:
                                time.sleep(0.005)
                            assert not child_endpoint.exists()
                            assert parent_endpoint.exists()
                            assert process_info(parent_endpoint, process.pid) == parent_identity
                            child_evidence.update({
                                "stage": "complete",
                                "inherited_sockets_after_recovery": sorted(inherited_sockets),
                                "blocked_parent_send_attempts": blocked_attempts,
                                "parent_bytes_before_block": constrained_bytes,
                                "trace_bytes": len(child_trace),
                                "trace": str(child_path),
                            })

                        stop(parent_endpoint, parent_session)
                        parent_trace = parent_reader.result(timeout=5)

                parent_path = args.output / "parent.nettrace"
                parent_path.write_bytes(parent_trace)
                assert command(process, "s") == "shutdown 1"
                assert not parent_endpoint.exists()
                evidence.update({
                    "parent_trace_bytes": len(parent_trace),
                    "parent_trace": str(parent_path),
                })
            finally:
                (args.output / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
                if process.poll() is None:
                    process.stdin.write(b"q\n")
                    try:
                        process.communicate(timeout=5)
                    except subprocess.TimeoutExpired:
                        os.killpg(process.pid, signal.SIGKILL)
                        process.communicate(timeout=5)
                assert process.returncode == 0
                assert stderr_path.stat().st_size == 0

    print("diagnostics managed-fork tracing passed", flush=True)


if __name__ == "__main__":
    main()
