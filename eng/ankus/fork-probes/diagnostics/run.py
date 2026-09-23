"""Exercise diagnostic endpoint ownership through the real IPC protocol on Linux."""

import argparse
import json
import os
from pathlib import Path
import select
import signal
import socket
import struct
import subprocess
import tempfile


MAGIC = b"DOTNET_IPC_V1\0"
HEADER = struct.Struct("<14sHBBH")


def receive(connection, count):
    """Read one bounded protocol record, rejecting premature EOF."""
    data = bytearray()
    while len(data) < count:
        block = connection.recv(count - len(data))
        if not block:
            raise AssertionError("diagnostics reply ended early")
        data.extend(block)
    return bytes(data)


def process_info(endpoint, expected_pid):
    """Connect to a diagnostic endpoint and verify its process identity."""
    with socket.socket(socket.AF_UNIX) as connection:
        connection.settimeout(5)
        connection.connect(str(endpoint))
        return process_info_stream(connection, expected_pid)


def process_info_stream(connection, expected_pid):
    """Decode ProcessInfo2 and independently verify identity and every field boundary."""
    connection.sendall(HEADER.pack(MAGIC, HEADER.size, 4, 4, 0))
    magic, size, command_set, command, reserved = HEADER.unpack(receive(connection, HEADER.size))
    assert (magic, command_set, command, reserved) == (MAGIC, 255, 0, 0)
    assert HEADER.size + 24 <= size <= 65535
    payload = receive(connection, size - HEADER.size)
    pid = struct.unpack_from("<Q", payload)[0]
    cookie = payload[8:24].hex()
    assert pid == expected_pid and cookie != "00" * 16
    fields = []
    offset = 24
    for _ in range(5):
        length = struct.unpack_from("<I", payload, offset)[0]
        offset += 4
        assert 1 <= length <= 32767 and offset + length * 2 <= len(payload)
        value = payload[offset:offset + length * 2].decode("utf-16-le")
        assert value.endswith("\0") and "\0" not in value[:-1]
        fields.append(value[:-1])
        offset += length * 2
    assert offset == len(payload)
    assert fields[1:3] == ["Linux", "x64"] and fields[3] == "NativeDiagnosticsProbe", fields
    return {"pid": pid, "cookie": cookie, "fields": fields}


def read_line(process):
    """Bound host responses without relying on managed timing facilities."""
    assert select.select([process.stdout], [], [], 10)[0], "host response timed out"
    line = process.stdout.readline().strip()
    assert line, "host exited without a response"
    return line


def command(process, text):
    """Send a single native-only operation and return its completion record."""
    process.stdin.write(text + "\n")
    process.stdin.flush()
    return read_line(process)


def main():
    """Keep sockets, logs and processes inside one explicitly owned validation run."""
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", type=Path, required=True)
    parser.add_argument("--library", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--case", choices=["all", "ownership", "ownership-close", "listen-failure"], default="all")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    evidence = {"observations": []}
    with tempfile.TemporaryDirectory(prefix="diag-", dir="/tmp") as socket_directory:
        env = dict(os.environ)
        for name in list(env):
            if name.startswith(("DOTNET_", "COMPlus_")) and any(
                    word in name for word in ["Diagnostic", "EventPipe", "gcServer", "gcConcurrent"]):
                env.pop(name)
        env["TMPDIR"] = socket_directory
        with (args.output / "stderr.log").open("w") as errors:
            process = subprocess.Popen([str(args.host.resolve()), str(args.library.resolve())],
                                       stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=errors,
                                       text=True, env=env, start_new_session=True)
            try:
                assert read_line(process) == f"ready {process.pid}"
                endpoints = list(Path(socket_directory).glob(f"dotnet-diagnostic-{process.pid}-*-socket"))
                assert len(endpoints) == 1
                endpoint = endpoints[0]
                before = process_info(endpoint, process.pid)
                evidence["observations"].append({"stage": "before", "process": before})
                child_commands = {
                    "all": ["c"] * 3 + ["n"] * 3,
                    "ownership": ["c"] * 3,
                    "ownership-close": ["n"] * 3,
                    "listen-failure": [],
                }[args.case]
                for index, child_command in enumerate(child_commands):
                    response = command(process, child_command)
                    assert response.startswith("child-closed ")
                    child_pid = int(response.split()[1])
                    assert child_pid != process.pid and not Path(f"/proc/{child_pid}").exists()
                    after = process_info(endpoint, process.pid)
                    assert after == before
                    evidence["observations"].append({"stage": f"after-child-{index + 1}", "shutdown": child_command == "c", "process": after})
                if args.case in ("all", "listen-failure"):
                    failure_endpoint = Path(socket_directory) / "listen-failure.socket"
                    evidence["listen_failure"] = command(process, "l " + str(failure_endpoint))
                    assert evidence["listen_failure"] == "listen-failure 0"
                    assert not failure_endpoint.exists()
                    assert process_info(endpoint, process.pid) == before
                # Full diagnostic fork support is still guarded until all session/thread work is implemented.
                evidence["fork_capability"] = command(process, "e")
                assert evidence["fork_capability"] == "enable -3"
                assert command(process, "s") == "shutdown 1"
                assert not endpoint.exists(), "the creating process failed to unlink its own endpoint"
                evidence["owner_cleanup"] = True
            finally:
                if process.poll() is None:
                    process.stdin.write("q\n")
                    process.stdin.flush()
                    try:
                        remaining, _ = process.communicate(timeout=5)
                    except subprocess.TimeoutExpired:
                        os.killpg(process.pid, signal.SIGKILL)
                        remaining, _ = process.communicate(timeout=5)
                    evidence["remaining_output"] = remaining
                evidence["exit"] = process.returncode
                (args.output / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
            assert process.returncode == 0
            assert (args.output / "stderr.log").stat().st_size == 0
    print(f"diagnostics {args.case} checks passed; owner cleanup passed")


if __name__ == "__main__":
    main()
