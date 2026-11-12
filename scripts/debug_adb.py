"""Stage a recovery ADB userspace and its ELF dependencies for preboot USB."""

from __future__ import annotations

from pathlib import Path
import re
import shutil
import subprocess


class AdbError(Exception):
    pass


APPLETS = ("cat", "chmod", "cp", "dmesg", "find", "grep", "head", "id",
           "ls", "mount", "ps", "sed", "tail", "tr", "uname", "wc", "xxd")
BINARIES = ("adbd", "sh", "toybox", "linker64")


def _run(args: list[str], cwd: Path) -> str:
    result = subprocess.run(args, cwd=cwd, capture_output=True, text=True)
    if result.returncode:
        raise AdbError(f"{' '.join(args)}: {result.stderr.strip() or result.stdout.strip()}")
    return result.stdout


def _needed(path: Path) -> tuple[str, ...]:
    output = _run(["readelf", "-d", str(path)], path.parent)
    return tuple(re.findall(r"NEEDED.*\[(.*?)\]", output))


def stage(source: Path, tool: Path, staging: Path, architecture: str,
          project: Path) -> tuple[list[tuple[str, Path, int]], tuple[str, ...]]:
    """Return (ramdisk path, staged file, mode) plus toybox applet links."""
    if architecture not in ("arm64-v8a", "x86_64"):
        raise AdbError(f"unsupported ADB architecture: {architecture}")
    source = source.resolve()
    tool = tool.resolve()
    staging = staging.resolve()
    project = project.resolve()
    staging.mkdir(parents=True, exist_ok=True)
    if source.is_dir():
        root = source
    elif source.is_file():
        root = staging / "source"
        root.mkdir(exist_ok=True)
        if source.suffix == ".cpio":
            archive = source
        else:
            shutil.copy2(source, root / "source.img")
            _run([str(tool), "unpack", "source.img"], root)
            archive = root / "ramdisk.cpio"
        if not archive.is_file():
            raise AdbError(f"ADB source has no ramdisk: {source}")
        _run([str(tool), "cpio", str(archive), "extract"], root)
    else:
        raise AdbError(f"ADB source does not exist: {source}")

    system = root / "system"
    libdir = "lib64" if architecture == "arm64-v8a" or architecture == "x86_64" else "lib"
    binaries = [system / "bin" / name for name in BINARIES]
    missing = [str(path.relative_to(root)) for path in binaries if not path.is_file()]
    if missing:
        raise AdbError(f"ADB source is missing {', '.join(missing)}")
    # Read the ELF machine directly; this is also checked for each dependency.
    from scripts.image_builder import elf_arch
    for binary in binaries:
        if elf_arch(binary) != architecture:
            raise AdbError(f"{binary} is not {architecture}")

    files = set(binaries)
    pending = list(binaries)
    search = (system / libdir, system / f"adbd_{libdir}")
    while pending:
        current = pending.pop()
        for name in _needed(current):
            library = next((directory / name for directory in search
                            if (directory / name).is_file()), None)
            if library is None:
                raise AdbError(f"{current.name} needs missing library {name}")
            if elf_arch(library) != architecture:
                raise AdbError(f"{library} is not {architecture}")
            if library not in files:
                files.add(library)
                pending.append(library)

    compiler = "aarch64-linux-gnu-gcc" if architecture == "arm64-v8a" else "cc"
    shim = staging / "libgrom_adb_shim.so"
    _run([compiler, "-shared", "-fPIC", "-nostdlib", "-O2",
          str(project / "grom/src/debug_adb_shim.c"), "-o", str(shim)], staging)
    if elf_arch(shim) != architecture:
        raise AdbError("ADB property shim has the wrong architecture")

    debug_command = staging / "grom-debug"
    _run([compiler, "-std=c11", "-O2", "-static", "-Wall", "-Wextra",
          str(project / "grom/src/debug_cli.c"), "-o", str(debug_command)], staging)
    if elf_arch(debug_command) != architecture:
        raise AdbError("grom-debug has the wrong architecture")
    if "INTERP" in _run(["readelf", "-l", str(debug_command)], staging):
        raise AdbError("grom-debug must be static")

    result = []
    for path in sorted(files):
        relative = path.relative_to(system)
        destination = f"system/{relative}"
        # Libraries from adbd_lib64 are installed into the normal library path.
        if relative.parts[0] == f"adbd_{libdir}":
            destination = f"system/{libdir}/{path.name}"
        result.append((destination, path, 0o755 if relative.parts[0] == "bin" else 0o644))
    ld_config = system / "etc/ld.config.txt"
    if ld_config.is_file():
        result.append(("system/etc/ld.config.txt", ld_config, 0o644))
    result.append((f"system/{libdir}/{shim.name}", shim, 0o755))
    result.append(("system/bin/grom-debug", debug_command, 0o755))
    marker = staging / "debug-adb.marker"
    marker.write_text("USB ADB preboot shell enabled\n")
    result.append(("grom/debug-adb", marker, 0o644))
    return result, APPLETS
