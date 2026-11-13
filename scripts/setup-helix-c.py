#!/usr/bin/env python3
"""Generate host compile commands for Helix/clangd without building Android."""

import json
from pathlib import Path
import shutil
import subprocess


ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "out"


def configure(source: Path, build: Path, extra: list[str]) -> list[dict]:
    subprocess.run(
        [
            "cmake", "-S", str(source), "-B", str(build), "-G", "Ninja",
            "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON", *extra,
        ],
        check=True,
    )
    return json.loads((build / "compile_commands.json").read_text())


def main() -> None:
    for tool in ("cmake", "ninja", "clangd", "clang"):
        if shutil.which(tool) is None:
            raise SystemExit(f"Missing required tool: {tool}")

    commands = configure(
        ROOT / "gromui", OUT / "helix-gromui",
        ["-DGROMUI_LVGL=ON", "-DBUILD_TESTING=ON"],
    )
    commands += configure(ROOT / "gromui/design", OUT / "helix-design", [])

    # Keep only checked-in project C files. The extracted LVGL sources are
    # generated in out/ and do not need to be indexed as project files.
    result = {}
    for entry in commands:
        source = Path(entry["file"]).resolve()
        if source.is_relative_to(ROOT / "grom") or (
            source.is_relative_to(ROOT / "gromui")
            and not source.is_relative_to(ROOT / "gromui/build")
        ):
            result.setdefault(str(source), entry)

    # Some combined-init sources are compiled by Magisk's Android.mk, not
    # by a host CMake target. Mirror its C include paths for editing only.
    includes = [
        ROOT / "grom/include", ROOT / "grom/src", ROOT / "grom/ui",
        ROOT / "gromui/include", OUT / "helix-gromui/vendor/lvgl",
    ]
    for source in sorted((ROOT / "grom/src").glob("*.c")):
        result.setdefault(str(source), {
            "directory": str(ROOT),
            "file": str(source),
            "arguments": [
                "clang", "-std=c11", "-D_GNU_SOURCE",
                "-DLV_CONF_INCLUDE_SIMPLE",
                *(f"-I{path}" for path in includes),
                "-c", str(source),
            ],
        })

    destination = ROOT / "compile_commands.json"
    destination.write_text(json.dumps(list(result.values()), indent=2) + "\n")
    print(f"Wrote {len(result)} C compile commands to {destination}")


if __name__ == "__main__":
    main()
