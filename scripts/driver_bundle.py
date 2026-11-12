"""Validate and stage a small, ordered early-boot driver bundle."""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import hashlib
import json
from pathlib import Path
import re
import shutil
import struct


ARCHES = {"x86_64": 62, "arm64-v8a": 183}
NAME = re.compile(r"[A-Za-z0-9][A-Za-z0-9_.-]{0,126}\Z")
KERNEL_VERSION = re.compile(rb"Linux version ([^\x00\x20]+)")


class BundleError(ValueError):
    pass


@dataclass(frozen=True)
class Entry:
    path: str
    source: Path
    sha256: str


@dataclass(frozen=True)
class Bundle:
    name: str
    entries: tuple[Entry, ...]
    modules: tuple[str, ...]


def kernel_release(kernel: Path) -> str | None:
    if not kernel.is_file():
        return None
    match = KERNEL_VERSION.search(kernel.read_bytes())
    return match.group(1).decode("ascii") if match else None


def _names(value: object, suffix: str, *, nested: bool = False) -> tuple[str, ...]:
    if not isinstance(value, list) or any(
            not isinstance(name, str) or len(name) > 180
            or not all(NAME.fullmatch(part) and part not in (".", "..")
                       for part in (name.split("/") if nested else [name]))
            or not name.endswith(suffix) for name in value):
        raise BundleError("driver profile contains an unsafe file name")
    if len(value) != len(set(value)) or len(value) > 64:
        raise BundleError(f"duplicate or excessive {suffix} entries")
    return tuple(value)


def _source(root: Path, directory: str, name: str) -> Path:
    for candidate in (root / directory / name,
                      root / "vendor" / directory / name,
                      root / Path(directory).name / name,
                      root / name):
        if candidate.is_file() and not candidate.is_symlink():
            return candidate
    raise BundleError(f"missing {directory}/{name} under {root}")


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as file:
        for block in iter(lambda: file.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def requirements(profile: Path, arch: str,
                 target_release: str | None = None) -> tuple[dict, tuple[str, ...], tuple[str, ...]]:
    try:
        data = json.loads(profile.read_text())
    except (OSError, ValueError) as exc:
        raise BundleError(f"cannot read profile {profile}: {exc}") from exc
    if not isinstance(data, dict) or set(data) - {
            "schema", "name", "arch", "kernel_release", "modules", "firmware"}:
        raise BundleError("unknown driver profile fields")
    if data.get("schema") != 1 or not isinstance(data.get("name"), str):
        raise BundleError("driver profile needs schema 1 and a name")
    if data.get("arch") != arch or arch not in ARCHES:
        raise BundleError(f"driver profile architecture does not match {arch}")
    expected_release = data.get("kernel_release")
    if expected_release is not None:
        if not isinstance(expected_release, str) or not expected_release:
            raise BundleError("invalid kernel_release")
        if target_release is None:
            raise BundleError("target kernel release is unknown; cannot check driver profile")
        if target_release != expected_release:
            raise BundleError(f"driver profile kernel {expected_release} differs from target {target_release}")
    modules = _names(data.get("modules"), ".ko")
    firmware = _names(data.get("firmware", []), "", nested=True)
    if not modules:
        raise BundleError("driver profile must name at least one module")
    return data, modules, firmware


def load(profile: Path, source: Path, arch: str,
         target_release: str | None = None) -> Bundle:
    data, modules, firmware = requirements(profile, arch, target_release)
    expected_release = data.get("kernel_release")
    entries = []
    for name in modules:
        path = _source(source, "lib/modules", name)
        with path.open("rb") as file:
            head = file.read(20)
        if len(head) < 20 or head[:6] != b"\x7fELF\x02\x01" or struct.unpack_from("<H", head, 18)[0] != ARCHES[arch]:
            raise BundleError(f"{path} is not an uncompressed {arch} kernel module")
        if expected_release and b"vermagic=" + expected_release.encode() + b" " not in path.read_bytes():
            raise BundleError(f"{path} has no matching vermagic")
        entries.append(Entry(f"grom/drivers/modules/{name}", path, _sha256(path)))
    for name in firmware:
        path = _source(source, "firmware", name)
        entries.append(Entry(f"lib/firmware/{name}", path, _sha256(path)))
    return Bundle(data["name"], tuple(entries), modules)


def stage(bundle: Bundle, destination: Path) -> None:
    manifest = destination / "grom/drivers/modules.load"
    manifest.parent.mkdir(parents=True, exist_ok=True)
    manifest.write_text("".join(name + "\n" for name in bundle.modules))
    for entry in bundle.entries:
        output = destination / entry.path
        output.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(entry.source, output)
        output.chmod(0o644)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--profile", type=Path, required=True)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--arch", choices=ARCHES, required=True)
    parser.add_argument("--kernel", type=Path)
    parser.add_argument("--destination", type=Path, required=True)
    args = parser.parse_args()
    bundle = load(args.profile, args.source, args.arch,
                  kernel_release(args.kernel) if args.kernel else None)
    stage(bundle, args.destination)


if __name__ == "__main__":
    try:
        main()
    except BundleError as exc:
        parser = argparse.ArgumentParser()
        parser.exit(1, f"driver bundle: {exc}\n")
