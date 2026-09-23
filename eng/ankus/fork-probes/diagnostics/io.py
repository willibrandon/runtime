"""Bound real diagnostic socket transfers, including partial progress and EINTR."""

import argparse
import array
import ctypes
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

from run import process_info


CASES = ["partial-read", "trickle-read", "interrupted-read", "blocked-write", "slow-write", "interrupted-write",
         "fragmented-read", "drained-write", "eof-read", "empty-read", "empty-write",
         "infinite-read", "infinite-write", "immediate-read", "immediate-empty-read",
         "final-read", "descriptor-read", "descriptor-eof", "descriptor-two", "descriptor-three", "descriptor-missing"]

LIBC = ctypes.CDLL(None, use_errno=True)
LIBC.tgkill.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_int]
LIBC.tgkill.restype = ctypes.c_int


def read_line(process):
    """Read unbuffered records under an independent supervisor deadline."""
    data = bytearray()
    deadline = time.monotonic() + 3
    while not data.endswith(b"\n"):
        remaining = deadline - time.monotonic()
        assert remaining > 0 and select.select([process.stdout], [], [], remaining)[0], "host response timed out"
        byte = os.read(process.stdout.fileno(), 1)
        assert byte, f"host exited with {process.poll()}"
        data.extend(byte)
    return data.decode().strip()


def payload(size):
    """Use a nonconstant byte sequence to expose truncation, duplication and reordering."""
    return (bytes(range(251)) * ((size + 250) // 251))[:size]


def run_case(args, case):
    """Give each case its own native process, endpoint and external peer."""
    evidence = {"case": case}
    errors = []
    transferred = bytearray()
    stop = threading.Event()
    peer_thread = None
    with tempfile.TemporaryDirectory(prefix="diag-io-", dir="/tmp") as directory:
        env = dict(os.environ)
        for name in list(env):
            if name.startswith(("DOTNET_", "COMPlus_")) and any(
                    word in name for word in ["Diagnostic", "EventPipe", "gcServer", "gcConcurrent"]):
                env.pop(name)
        env["TMPDIR"] = directory
        stderr_path = args.output / f"{case}.stderr.log"
        with stderr_path.open("w") as stderr:
            process = subprocess.Popen([str(args.host.resolve()), str(args.library.resolve())],
                                       stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=stderr,
                                       bufsize=0, env=env, start_new_session=True)
            try:
                assert read_line(process) == f"ready {process.pid}"
                evidence["fds_before"] = len(list(Path(f"/proc/{process.pid}/fd").iterdir()))
                direction = "w" if "write" in case else "r"
                if case.startswith("descriptor-"):
                    direction = "f"
                size = 8 * 1024 * 1024 if direction == "w" else 128
                if case.startswith("empty-"):
                    size = 0
                timeout = 300
                if case.startswith("infinite-"):
                    timeout = 0xffffffff
                elif case.startswith("immediate-") or size == 0:
                    timeout = 0
                elif case in ("fragmented-read", "drained-write"):
                    timeout = 2000
                endpoint = Path(directory) / "io.socket"
                process.stdin.write(f"i{direction} {timeout} {size} {endpoint}\n".encode())
                assert read_line(process) == "io-listening"
                expected = payload(size)
                with socket.socket(socket.AF_UNIX) as peer:
                    peer.settimeout(2)
                    peer.connect(str(endpoint))
                    assert read_line(process) == "io-connected"
                    if case in ("immediate-read", "final-read"):
                        peer.sendall(expected)
                    if case == "final-read":
                        peer.shutdown(socket.SHUT_WR)

                    def drive_peer():
                        """Provide data or back pressure independently of the runtime under test."""
                        try:
                            if case in ("partial-read", "interrupted-read", "interrupted-write"):
                                if direction == "r":
                                    peer.sendall(expected[:1])
                                while case.startswith("interrupted-") and not stop.wait(0.02):
                                    # The host's main thread performs the transfer. Do not signal a GC/helper thread.
                                    assert LIBC.tgkill(process.pid, process.pid, signal.SIGALRM) == 0, ctypes.get_errno()
                            elif case == "trickle-read":
                                for value in expected:
                                    peer.sendall(bytes([value]))
                                    if stop.wait(0.07):
                                        break
                            elif case in ("fragmented-read", "infinite-read"):
                                for offset in range(0, size, 16):
                                    if stop.wait(0.02):
                                        break
                                    peer.sendall(expected[offset:offset + 16])
                            elif case == "eof-read":
                                peer.sendall(expected[:1])
                                peer.shutdown(socket.SHUT_WR)
                            elif case in ("drained-write", "infinite-write"):
                                while len(transferred) < size:
                                    block = peer.recv(min(16384, size - len(transferred)))
                                    assert block, "write ended early"
                                    transferred.extend(block)
                            elif case == "slow-write":
                                while not stop.wait(0.07):
                                    block = peer.recv(16384)
                                    assert block, "write ended early"
                                    transferred.extend(block)
                            elif case in ("descriptor-read", "descriptor-two", "descriptor-three"):
                                with tempfile.TemporaryFile(dir=directory) as source:
                                    source.write(expected)
                                    source.flush()
                                    if not stop.wait(0.08):
                                        count = {"descriptor-read": 1, "descriptor-two": 2, "descriptor-three": 3}[case]
                                        peer.sendmsg([b"f"], [(socket.SOL_SOCKET, socket.SCM_RIGHTS, array.array("i", [source.fileno()] * count))])
                            elif case == "descriptor-eof":
                                peer.shutdown(socket.SHUT_WR)
                            elif case == "descriptor-missing":
                                peer.sendall(b"f")
                        except Exception as error:
                            errors.append(repr(error))

                    peer_thread = threading.Thread(target=drive_peer)
                    peer_thread.start()
                    started = time.monotonic()
                    process.stdin.write(b"g\n")
                    evidence["result"] = read_line(process)
                    evidence["elapsed_ms"] = (time.monotonic() - started) * 1000
                    if direction == "f":
                        evidence["descriptor"] = read_line(process)
                    assert read_line(process) == "io-complete 0"
                    signals = read_line(process)
                    assert signals.startswith("io-signals ")
                    evidence["signals"] = int(signals.split()[1])
                    if case.startswith("interrupted-"):
                        assert evidence["signals"] >= 5, evidence
                    stop.set()
                    peer_thread.join(timeout=3)
                    assert not peer_thread.is_alive()
                assert not errors, errors
                assert not endpoint.exists(), "stream listener was not removed"
                evidence["fds_after"] = len(list(Path(f"/proc/{process.pid}/fd").iterdir()))
                assert evidence["fds_before"] == evidence["fds_after"], evidence
                failed = case in ("partial-read", "trickle-read", "interrupted-read", "blocked-write", "slow-write", "interrupted-write", "eof-read", "immediate-empty-read", "descriptor-eof", "descriptor-two", "descriptor-three", "descriptor-missing")
                assert evidence["result"] == f"io-result {0 if failed else 1} {0 if failed else size} 1", evidence
                if direction == "f":
                    assert evidence["descriptor"] == ("io-descriptor 0 -1 -1" if failed else "io-descriptor 1 1 1"), evidence
                if case in ("partial-read", "trickle-read", "interrupted-read", "blocked-write", "slow-write", "interrupted-write"):
                    assert 200 <= evidence["elapsed_ms"] < 1500, evidence
                if case == "slow-write":
                    assert 0 < len(transferred) < size and transferred == expected[:len(transferred)]
                if case in ("drained-write", "infinite-write"):
                    assert transferred == expected, "write bytes were lost or reordered"
                endpoints = list(Path(directory).glob(f"dotnet-diagnostic-{process.pid}-*-socket"))
                assert len(endpoints) == 1
                evidence["process_after"] = process_info(endpoints[0], process.pid)
                process.stdin.write(b"q\n")
                process.wait(timeout=3)
                assert process.returncode == 0
                assert stderr_path.stat().st_size == 0
            finally:
                stop.set()
                if process.poll() is None:
                    os.killpg(process.pid, signal.SIGKILL)
                    process.wait(timeout=3)
                if peer_thread is not None:
                    peer_thread.join(timeout=3)
                evidence["exit"] = process.returncode
                evidence["peer_errors"] = errors
                (args.output / f"{case}.json").write_text(json.dumps(evidence, indent=2) + "\n")
    print(f"{case}: passed ({evidence['elapsed_ms']:.1f} ms)", flush=True)


def main():
    """Retain every result and fail immediately on an unbounded or incorrect transfer."""
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
