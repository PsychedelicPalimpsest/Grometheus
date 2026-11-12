"""Inspect, patch, and verify disposable Android boot or ramdisk images."""

from __future__ import annotations

from dataclasses import dataclass
import hashlib
from pathlib import Path
import re
import shutil
import struct
import subprocess

from gromui.scripts.ramdisk_tools import split_first_archive
from scripts import driver_bundle, debug_adb


class ImageError(Exception):
    """Image cannot be handled without risking an invalid output."""


ARCHES = {62: "x86_64", 183: "arm64-v8a"}
COMPONENTS = ("kernel", "kernel_dtb", "second", "dtb",
              "recovery_dtbo", "extra")


@dataclass
class ImageInput:
    requested: Path
    source: Path
    workdir: Path
    kind: str
    compression: str
    arch: str
    stock_init: Path | None = None


def run(command: list[str], cwd: Path) -> str:
    try:
        result = subprocess.run(command, cwd=cwd, capture_output=True,
                                text=True, check=False)
    except OSError as exc:
        raise ImageError(f"could not run {command[0]}: {exc}") from exc
    output = (result.stdout or "") + (result.stderr or "")
    if result.returncode:
        raise ImageError(f"{' '.join(command)} failed ({result.returncode}):\n"
                         + "\n".join(output.splitlines()[-20:]))
    return output


def resolve_image(path: Path) -> Path:
    path = path.expanduser().resolve()
    if path.is_file():
        return path
    if path.is_dir():
        for name in ("boot.img", "init_boot.img", "boot_a.img", "ramdisk.img",
                     "ramdisk.cpio"):
            candidate = path / name
            if candidate.is_file():
                return candidate
    raise ImageError(f"no supported boot image or ramdisk found at {path}")


def elf_arch(path: Path) -> str | None:
    try:
        with path.open("rb") as file:
            head = file.read(64)
    except OSError:
        return None
    if len(head) < 20 or head[:4] != b"\x7fELF" or head[5] != 1:
        return None
    if head[4] != 2:
        return None
    return ARCHES.get(struct.unpack_from("<H", head, 18)[0])


def kernel_arch(path: Path) -> str | None:
    if not path.is_file():
        return None
    with path.open("rb") as file:
        head = file.read(65536)
    if len(head) >= 0x3c and head[0x38:0x3c] == b"ARM\x64":
        return "arm64-v8a"
    if len(head) >= 0x238 and head[0x202:0x206] == b"HdrS":
        flags = struct.unpack_from("<H", head, 0x236)[0]
        return "x86_64" if flags & 1 else None
    return elf_arch(path)


def cpio_exists(tool: Path, archive: Path, entry: str, cwd: Path) -> bool:
    result = subprocess.run([str(tool), "cpio", str(archive), f"exists {entry}"],
                            cwd=cwd, capture_output=True, check=False)
    return result.returncode == 0


def cpio_extract(tool: Path, archive: Path, entry: str, dest: Path,
                 cwd: Path) -> bool:
    if not cpio_exists(tool, archive, entry, cwd):
        return False
    dest.unlink(missing_ok=True)
    run([str(tool), "cpio", str(archive), f"extract {entry} {dest}"], cwd)
    return dest.is_file() and dest.stat().st_size > 0


def verify_ramdisk_parents(archive: Path, staged_paths: list[str]) -> None:
    """The kernel cpio unpacker needs directory entries before nested files."""
    data = archive.read_bytes()
    position = 0
    entries: dict[str, tuple[int, int]] = {}
    while data[position:position + 6] in (b"070701", b"070702"):
        fields = [int(data[position + 6 + i * 8:position + 14 + i * 8], 16)
                  for i in range(13)]
        mode, size, name_size = fields[1], fields[6], fields[11]
        name = data[position + 110:position + 110 + name_size - 1].decode()
        content = (position + 110 + name_size + 3) & ~3
        position = (content + size + 3) & ~3
        if name == "TRAILER!!!":
            break
        entries[name] = (mode, len(entries))
    for path in staged_paths:
        if path not in entries:
            raise ImageError(f"repacked ramdisk is missing {path}")
        parent = Path(path).parent
        while parent != Path("."):
            name = parent.as_posix()
            if name not in entries or entries[name][0] & 0o170000 != 0o040000:
                raise ImageError(f"ramdisk lacks directory /{name} for /{path}")
            if entries[name][1] >= entries[path][1]:
                raise ImageError(f"ramdisk directory /{name} follows /{path}")
            parent = parent.parent


def ramdisk_arch(tool: Path, first: Path, cwd: Path) -> str | None:
    probe = cwd / "arch-probe.cpio"
    shutil.copy2(first, probe)
    try:
        for entry in ("init", "system/bin/init", "first_stage_ramdisk/system/bin/init"):
            extracted = cwd / "arch-probe-init"
            if cpio_extract(tool, probe, entry, extracted, cwd):
                arch = elf_arch(extracted)
                extracted.unlink(missing_ok=True)
                if arch:
                    return arch
    finally:
        probe.unlink(missing_ok=True)
    return None


def unpack_boot(source: Path, directory: Path, tool: Path) -> tuple[Path | None, str]:
    directory.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, directory / "boot.img")
    output = run([str(tool), "unpack", "-h", "boot.img"], directory)
    ramdisk = directory / "ramdisk.cpio"
    return (ramdisk if ramdisk.is_file() and ramdisk.stat().st_size else None,
            output)


def prepare_image(source: Path, workdir: Path, tool: Path,
                  arch_override: str | None = None) -> ImageInput:
    requested = resolve_image(source)
    with requested.open("rb") as file:
        magic = file.read(8)
    image_dir = workdir / "image"
    image_dir.mkdir(parents=True, exist_ok=True)
    source = requested
    from_kernel = None
    if magic == b"ANDROID!":
        ramdisk, _ = unpack_boot(source, image_dir, tool)
        from_kernel = kernel_arch(image_dir / "kernel")
        if ramdisk is None:
            sibling_name = "init_boot_a.img" if source.name == "boot_a.img" else "init_boot.img"
            sibling = source.parent / sibling_name
            if source.name == "init_boot.img" or not sibling.is_file():
                raise ImageError("boot image has no ramdisk; provide its init_boot.img")
            source = sibling.resolve()
            image_dir = workdir / "init_boot"
            ramdisk, _ = unpack_boot(source, image_dir, tool)
            if ramdisk is None:
                raise ImageError("sibling init_boot.img has no ramdisk")
        kind = "boot"
        compression = "from-boot-header"
    elif magic == b"VNDRBOOT":
        raise ImageError("vendor_boot is not a supported /init replacement target")
    else:
        kind = "ramdisk"
        ramdisk = image_dir / "ramdisk.cpio"
        if magic[:6] in (b"070701", b"070702"):
            shutil.copy2(source, ramdisk)
            compression = "raw"
        else:
            output = run([str(tool), "decompress", str(source), str(ramdisk)], image_dir)
            match = re.search(r"Detected format:\s*\[?([a-zA-Z0-9_]+)", output)
            if not match:
                raise ImageError("cannot determine standalone ramdisk compression")
            compression = match.group(1)
        for name in ("kernel-ranchu", "kernel", "Image"):
            from_kernel = kernel_arch(source.parent / name)
            if from_kernel:
                break
    first = image_dir / "first.cpio"
    remainder = image_dir / "remainder.cpio"
    try:
        split_first_archive(ramdisk, first, remainder)
    except (ValueError, IndexError) as exc:
        raise ImageError(f"ramdisk is not a supported newc archive: {exc}") from exc
    from_init = ramdisk_arch(tool, first, image_dir)
    if from_kernel and from_init and from_kernel != from_init:
        raise ImageError(f"kernel is {from_kernel} but ramdisk init is {from_init}")
    detected = from_kernel or from_init
    if arch_override and detected and arch_override != detected:
        raise ImageError(f"--arch {arch_override} disagrees with detected {detected}")
    arch = detected or arch_override
    if not arch:
        raise ImageError("cannot detect architecture; supply --arch x86_64 or arm64-v8a")
    return ImageInput(requested, source, image_dir, kind, compression, arch)


def patch_image(spec: ImageInput, init: Path, output: Path, tool: Path,
                bundle: driver_bundle.Bundle | None = None,
                touch_diagnostics: bool = False,
                debug_adb_source: Path | None = None) -> dict[str, object]:
    directory = spec.workdir
    first = directory / "first.cpio"
    remainder = directory / "remainder.cpio"
    original_remainder = remainder.read_bytes()
    stock_entry = "init.orig" if cpio_exists(tool, first, "init.orig", directory) else "init"
    stock = directory / "stock-init.bin"
    if not cpio_extract(tool, first, stock_entry, stock, directory):
        raise ImageError(f"ramdisk has no extractable /{stock_entry}; refusing to replace /init")
    stock_arch = elf_arch(stock)
    if stock_arch and stock_arch != spec.arch:
        raise ImageError(f"stock init is {stock_arch}, expected {spec.arch}")
    spec.stock_init = stock
    shutil.copy2(init, directory / "grom-init.bin")
    additions = ["add 0755 init grom-init.bin",
                 "add 0750 init.orig stock-init.bin"]
    staged_paths = ["init", "init.orig"]
    if bundle:
        staging = directory / "driver-stage"
        driver_bundle.stage(bundle, staging)
        for path in ("grom/drivers/modules.load", *(entry.path for entry in bundle.entries)):
            additions.append(f"add 0644 {path} {staging / path}")
            staged_paths.append(path)
    if touch_diagnostics:
        marker = directory / "touch-diagnostics.txt"
        marker.write_text("# touch diagnostics\n" +
                          "".join(name + "\n" for name in (bundle.modules if bundle else ())))
        additions.append(f"add 0644 grom/touch-diagnostics {marker}")
        staged_paths.append("grom/touch-diagnostics")
    adb_files: list[tuple[str, Path, int]] = []
    if debug_adb_source:
        adb_files, applets = debug_adb.stage(debug_adb_source, tool,
                                            directory / "adb-stage", spec.arch,
                                            Path(__file__).resolve().parent.parent)
        for path, file, mode in adb_files:
            additions.append(f"add {mode:04o} {path} {file}")
            staged_paths.append(path)
        for applet in applets:
            additions.append(f"ln toybox system/bin/{applet}")
            staged_paths.append(f"system/bin/{applet}")
    required_dirs = {parent.as_posix() for path in staged_paths
                     for parent in Path(path).parents if parent != Path(".")}
    directories = [f"mkdir 0755 {name}" for name in sorted(required_dirs,
                   key=lambda value: (value.count("/"), value))
                   if not cpio_exists(tool, first, name, directory)]
    additions = directories + additions
    run([str(tool), "cpio", str(first), *additions], directory)
    (directory / "ramdisk.cpio").write_bytes(first.read_bytes() + original_remainder)

    packed = directory / "packed.img"
    if spec.kind == "boot":
        run([str(tool), "repack", "boot.img", str(packed)], directory)
    elif spec.compression == "raw":
        shutil.copy2(directory / "ramdisk.cpio", packed)
    else:
        run([str(tool), f"compress={spec.compression}", "ramdisk.cpio", str(packed)],
            directory)
    if not packed.is_file() or not packed.stat().st_size:
        raise ImageError("repack produced no image")

    verify = directory.parent / "verify"
    verify.mkdir(parents=True, exist_ok=True)
    if spec.kind == "boot":
        verified_ramdisk, _ = unpack_boot(packed, verify, tool)
        if verified_ramdisk is None:
            raise ImageError("repacked boot image has no ramdisk")
        for component in COMPONENTS:
            original = directory / component
            repacked = verify / component
            if original.is_file() and (not repacked.is_file() or
                                       original.read_bytes() != repacked.read_bytes()):
                raise ImageError(f"repacked image changed {component}")
    elif spec.compression == "raw":
        verified_ramdisk = packed
    else:
        verified_ramdisk = verify / "ramdisk.cpio"
        run([str(tool), "decompress", str(packed), str(verified_ramdisk)], verify)
    verified_first = verify / "first.cpio"
    verified_remainder = verify / "remainder.cpio"
    try:
        split_first_archive(verified_ramdisk, verified_first, verified_remainder)
    except (ValueError, IndexError) as exc:
        raise ImageError(f"repacked ramdisk is invalid: {exc}") from exc
    if verified_remainder.read_bytes() != original_remainder:
        raise ImageError("repacked ramdisk changed a later CPIO archive")
    verify_ramdisk_parents(verified_first, staged_paths)
    found_init = verify / "grom-init.bin"
    found_stock = verify / "stock-init.bin"
    if not cpio_extract(tool, verified_first, "init", found_init, verify):
        raise ImageError("repacked image is missing /init")
    if not cpio_extract(tool, verified_first, "init.orig", found_stock, verify):
        raise ImageError("repacked image is missing /init.orig")
    if found_init.read_bytes() != init.read_bytes():
        raise ImageError("repacked /init differs from the built grom binary")
    if found_stock.read_bytes() != stock.read_bytes():
        raise ImageError("repacked /init.orig differs from stock init")
    if bundle:
        staging = directory / "driver-stage"
        for path in ("grom/drivers/modules.load", *(entry.path for entry in bundle.entries)):
            found = verify / "bundle-check" / path
            found.parent.mkdir(parents=True, exist_ok=True)
            if not cpio_extract(tool, verified_first, path, found, verify):
                raise ImageError(f"repacked image is missing {path}")
            if found.read_bytes() != (staging / path).read_bytes():
                raise ImageError(f"repacked image changed {path}")
    if touch_diagnostics:
        found = verify / "touch-diagnostics.txt"
        if not cpio_extract(tool, verified_first, "grom/touch-diagnostics", found, verify):
            raise ImageError("repacked image is missing touch diagnostics marker")
        if found.read_bytes() != (directory / "touch-diagnostics.txt").read_bytes():
            raise ImageError("repacked image changed touch diagnostics marker")
    for path, source, _ in adb_files:
        found = verify / "adb-check" / path
        found.parent.mkdir(parents=True, exist_ok=True)
        if not cpio_extract(tool, verified_first, path, found, verify):
            raise ImageError(f"repacked image is missing {path}")
        if found.read_bytes() != source.read_bytes():
            raise ImageError(f"repacked image changed {path}")

    output = output.expanduser().resolve()
    if output == spec.requested or output == spec.source:
        raise ImageError("output must differ from the input image")
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = output.with_name(output.name + ".tmp")
    shutil.copy2(packed, temporary)
    temporary.replace(output)
    result = {
        "requested_input": str(spec.requested),
        "patched_input": str(spec.source),
        "kind": spec.kind,
        "architecture": spec.arch,
        "compression": spec.compression,
        "source_sha256": hashlib.sha256(spec.source.read_bytes()).hexdigest(),
        "output": str(output),
        "output_sha256": hashlib.sha256(output.read_bytes()).hexdigest(),
        "stock_init_sha256": hashlib.sha256(stock.read_bytes()).hexdigest(),
        "grom_init_sha256": hashlib.sha256(init.read_bytes()).hexdigest(),
        "bytes": output.stat().st_size,
        "status": "preboot scaffold; Android handoff is not implemented",
    }
    if bundle:
        result["drivers"] = {
            "profile": bundle.name,
            "modules": list(bundle.modules),
            "files": {entry.path: entry.sha256 for entry in bundle.entries},
        }
    if touch_diagnostics:
        result["touch_diagnostics"] = True
    if debug_adb_source:
        result["debug_adb"] = {"source": str(debug_adb_source),
                               "files": len(adb_files), "usb_only": True}
    return result
