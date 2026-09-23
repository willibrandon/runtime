"""Compile the allocation probe with the configured Native AOT runtime headers."""

import argparse
import json
from pathlib import Path
import shlex
import subprocess


def main():
    """Reuse the runtime's platform definitions without duplicating its ABI setup."""
    parser = argparse.ArgumentParser()
    parser.add_argument("--compile-commands", type=Path, required=True)
    args = parser.parse_args()
    fixture = Path(__file__).resolve().parent
    entries = json.loads(args.compile_commands.read_text())
    entry = next(value for value in entries if
                 "nativeaot/Runtime/eventpipe/CMakeFiles/eventpipe-shared-objects.dir" in value["command"])
    command = shlex.split(entry["command"])
    output = fixture / "obj" / "ownership.o"
    output.parent.mkdir(parents=True, exist_ok=True)
    command[command.index("-o") + 1] = str(output)
    command[command.index("-c") + 1] = str(fixture / "ownership.cpp")
    for flag in ["-MT", "-MF"]:
        if flag in command:
            index = command.index(flag)
            del command[index:index + 2]
    if "-MD" in command:
        command.remove("-MD")
    command.append("-Werror")
    (fixture / "obj" / "ownership-command.json").write_text(json.dumps(command, indent=2) + "\n")
    subprocess.run(command, cwd=entry["directory"], check=True)


if __name__ == "__main__":
    main()
