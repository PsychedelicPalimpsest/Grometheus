#!/usr/bin/env python3
"""Build the static grom/MagiskInit scaffold and patch a disposable boot image."""

from __future__ import annotations

import argparse
from contextlib import ExitStack
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import zipfile

from scripts import debug_adb, driver_bundle, image_builder, romdump


ROOT = Path(__file__).resolve().parent
MAGISK = ROOT / "vendor/magisk"
BUILD_SCRIPT = ROOT / "scripts/build-init.sh"
TEST_BUILD = ROOT / "out/gromui-tests"
OUTPUT = ROOT / "out/grom-init"
ARCHES = {"x86_64": 62, "arm64-v8a": 183}
STEPS = (
    "Inspect boot image or ramdisk and detect target architecture (when supplied)",
    "Find the Android SDK and pinned Magisk ONDK",
    "Install the pinned ONDK if missing",
    "Validate any selected driver profile against supplied modules and target kernel",
    "Build pinned magiskboot and the combined static init for the target ABI",
    "Run builder checks and static gromui host tests",
    "Patch and reopen the image; check static ELF, stock init, drivers, and hashes",
)


class BuildError(Exception):
    """A failure that can be shown without a Python traceback."""


def say(message: str) -> None:
    print(f"[grombake] {message}", flush=True)


def run(command: list[str], *, cwd: Path = ROOT,
        env: dict[str, str] | None = None) -> None:
    say("$ " + " ".join(command))
    try:
        result = subprocess.run(command, cwd=cwd, env=env, check=False)
    except OSError as exc:
        raise BuildError(f"could not run {command[0]}: {exc}") from exc
    if result.returncode:
        raise BuildError(f"{command[0]} failed with exit code {result.returncode}")


def require_tools(*names: str) -> None:
    missing = [name for name in names if shutil.which(name) is None]
    if missing:
        raise BuildError(
            "missing build tools: " + ", ".join(missing) +
            ". Install them and rerun; the host tests also need a C compiler."
        )


def pinned_ondk_version() -> str:
    source = (MAGISK / "build.py").read_text()
    match = re.search(r'^ondk_version\s*=\s*["\']([^"\']+)["\']', source, re.MULTILINE)
    if not match:
        raise BuildError("cannot find the pinned ONDK version in Magisk build.py")
    return match.group(1)


def ondk_path(sdk: Path) -> Path:
    return sdk / "ndk/magisk"


def ondk_ready(sdk: Path, version: str) -> bool:
    ndk = ondk_path(sdk)
    marker = ndk / "ONDK_VERSION"
    try:
        return (marker.read_text().strip() == version and
                (ndk / "ndk-build").is_file() and
                (ndk / "toolchains/rust/bin/cargo").is_file())
    except OSError:
        return False


def sdk_candidates(explicit: str | None, environ: dict[str, str],
                   home: Path) -> list[Path]:
    if explicit:
        return [Path(explicit).expanduser().resolve()]
    raw = [environ.get("ANDROID_HOME"), environ.get("ANDROID_SDK_ROOT"),
           str(home / "Android/Sdk"), "/opt/android-sdk"]
    paths = []
    for value in raw:
        if value:
            path = Path(value).expanduser().resolve()
            if path not in paths:
                paths.append(path)
    return paths


def find_sdk(explicit: str | None, environ: dict[str, str],
             home: Path, version: str) -> Path:
    candidates = sdk_candidates(explicit, environ, home)
    for sdk in candidates:
        if ondk_ready(sdk, version):
            return sdk
    if explicit:
        return candidates[0]
    for sdk in candidates:
        if (sdk.is_dir() and not ondk_path(sdk).exists() and os.access(sdk, os.W_OK)
                and any((sdk / item).exists()
                        for item in ("ndk", "emulator", "platform-tools"))):
            return sdk
    for sdk in candidates:
        if sdk.is_dir() and any((sdk / item).exists()
                                for item in ("ndk", "emulator", "platform-tools")):
            return sdk
    return home / "Android/Sdk"


def ensure_ondk(sdk: Path, version: str, *, offline: bool,
                env: dict[str, str]) -> None:
    if ondk_ready(sdk, version):
        say(f"using ONDK {version} at {ondk_path(sdk)}")
        return
    ndk = ondk_path(sdk)
    if ndk.exists():
        raise BuildError(
            f"{ndk} exists but is not a complete ONDK {version} installation. "
            "Choose another SDK with --sdk or repair that installation explicitly."
        )
    if offline:
        raise BuildError(f"ONDK {version} is missing at {ndk}; --offline forbids download")
    (sdk / "ndk").mkdir(parents=True, exist_ok=True)
    say(f"installing pinned ONDK {version} into {sdk} using Magisk's installer")
    run([sys.executable, "build.py", "ndk"], cwd=MAGISK, env=env)
    if not ondk_ready(sdk, version):
        raise BuildError(f"Magisk's installer did not produce a complete ONDK {version}")


def configured_abis() -> list[str]:
    props = (ROOT / "config/magisk.prop").read_text().splitlines()
    values = [line.split("=", 1)[1] for line in props if line.startswith("abiList=")]
    if len(values) != 1:
        raise BuildError("config/magisk.prop must contain one abiList")
    abis = [abi.strip() for abi in values[0].split(",")]
    if not abis or any(abi not in ARCHES for abi in abis) or len(abis) != len(set(abis)):
        raise BuildError("this builder supports unique x86_64 and arm64-v8a ABIs")
    return abis


def write_abi_config(path: Path, abis: list[str]) -> None:
    original = (ROOT / "config/magisk.prop").read_text().splitlines()
    lines = [f"abiList={','.join(abis)}" if line.startswith("abiList=") else line
             for line in original]
    path.write_text("\n".join(lines) + "\n")


def host_abi() -> str:
    machine = platform.machine().lower()
    if machine in ("x86_64", "amd64"):
        return "x86_64"
    if machine in ("aarch64", "arm64"):
        return "arm64-v8a"
    raise BuildError(f"cannot run a pinned Android magiskboot on host {machine}; "
                     "pass --magiskboot PATH")


def pinned_magiskboot(workdir: Path, env: dict[str, str], release: bool) -> Path:
    abi = host_abi()
    config = workdir / "magiskboot.prop"
    write_abi_config(config, [abi])
    command = [sys.executable, "build.py"]
    if release:
        command.append("-r")
    command += ["-c", str(config), "native", "magiskboot"]
    say(f"building pinned magiskboot for host {abi}")
    run(command, cwd=MAGISK, env=env)
    tool = MAGISK / "native/out" / abi / "magiskboot"
    check_static_elf(tool, abi)
    return tool


def check_static_elf(path: Path, abi: str) -> dict[str, object]:
    try:
        data = path.read_bytes()
    except OSError as exc:
        raise BuildError(f"missing output {path}: {exc}") from exc
    if len(data) < 64 or data[:4] != b"\x7fELF" or data[4:6] != b"\x02\x01":
        raise BuildError(f"{path} is not a little-endian 64-bit ELF")
    machine = struct.unpack_from("<H", data, 18)[0]
    if machine != ARCHES[abi]:
        raise BuildError(f"{path} has ELF machine {machine}, expected {ARCHES[abi]}")
    phoff = struct.unpack_from("<Q", data, 32)[0]
    phentsize, phnum = struct.unpack_from("<HH", data, 54)
    if phentsize < 4 or not phnum or phoff + phentsize * phnum > len(data):
        raise BuildError(f"{path} has invalid ELF program headers")
    headers = [struct.unpack_from("<I", data, phoff + i * phentsize)[0]
               for i in range(phnum)]
    if 1 not in headers or 2 in headers or 3 in headers:
        raise BuildError(f"{path} has no load segment or uses dynamic linking")
    try:
        recorded_path = str(path.relative_to(ROOT))
    except ValueError:
        recorded_path = str(path)
    return {
        "path": recorded_path,
        "bytes": len(data),
        "sha256": hashlib.sha256(data).hexdigest(),
        "machine": machine,
        "static": True,
    }


def write_manifest(abis: list[str], version: str, release: bool,
                   tested: bool, image: dict[str, object] | None = None) -> Path:
    artifacts = {abi: check_static_elf(OUTPUT / abi / "init", abi) for abi in abis}
    lock = (MAGISK / "UPSTREAM.lock").read_text().splitlines()
    magisk_commit = next((line.split("=", 1)[1] for line in lock
                          if line.startswith("commit=")), "unknown")
    manifest = {
        "schema": 1,
        "status": "preboot-display-scaffold; Android handoff is not implemented",
        "magisk_commit": magisk_commit,
        "ondk_version": version,
        "build_type": "release" if release else "debug",
        "gromui_host_tests": "passed" if tested else "skipped",
        "builder_checks": "passed" if tested else "skipped",
        "artifacts": artifacts,
    }
    if image:
        manifest["image"] = image
    output = OUTPUT / "build.json"
    temporary = OUTPUT / "build.json.tmp"
    temporary.write_text(json.dumps(manifest, indent=2) + "\n")
    temporary.replace(output)
    return output


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="build static grom init and optionally patch a boot image")
    parser.add_argument("image", nargs="?", type=Path,
                        help="boot image, ramdisk, ROM ZIP, or ROM directory")
    parser.add_argument("-o", "--output", type=Path,
                        help="patched image path (default: out/grom-images/<source>-grom.img)")
    parser.add_argument("--arch", choices=ARCHES,
                        help="architecture fallback when image detection is inconclusive")
    parser.add_argument("--magiskboot", type=Path,
                        help="use this host magiskboot instead of building pinned source")
    parser.add_argument("--driver-profile", type=Path,
                        help="grom/devices profile name or JSON path")
    parser.add_argument("--driver-source", type=Path,
                        help="directory containing matching lib/modules and firmware files")
    parser.add_argument("--romdump", type=Path,
                        help="ROM ZIP or directory to extract matching vendor modules from")
    parser.add_argument("--vendor-slot", choices=('a', 'b'),
                        help="vendor slot when both super partitions contain data")
    parser.add_argument("--touch-diagnostics", action="store_true",
                        help="show temporary touchscreen diagnostics on the preboot display")
    parser.add_argument("--debug-adb", action="store_true",
                        help="bundle a recovery ADB shell for preboot USB debugging")
    parser.add_argument("--debug-adb-source", type=Path,
                        help="recovery boot image, cpio, or unpacked root with system/bin/adbd")
    parser.add_argument("--sdk", metavar="PATH", help="Android SDK/ONDK root (auto-detected)")
    parser.add_argument("--offline", action="store_true",
                        help="do not download ONDK or Cargo dependencies")
    parser.add_argument("--release", action="store_true", help="use Magisk's release build")
    parser.add_argument("--skip-tests", action="store_true",
                        help="skip the static gromui host test suite")
    parser.add_argument("--jobs", type=int, default=min(os.cpu_count() or 1, 4),
                        help="parallel host test build jobs (default: up to 4)")
    parser.add_argument("--list-steps", action="store_true", help="show the pipeline and exit")
    args = parser.parse_args(argv)
    if args.list_steps:
        for number, step in enumerate(STEPS, 1):
            print(f"{number}. {step}")
        return 0
    try:
        if args.output and not args.image:
            raise BuildError("--output requires an input image")
        if args.arch and not args.image:
            raise BuildError("--arch requires an input image")
        if args.driver_profile and not args.image:
            raise BuildError("driver bundling requires an image")
        if args.driver_source and not args.driver_profile:
            raise BuildError("--driver-source requires --driver-profile")
        if args.driver_source and args.romdump:
            raise BuildError("choose --driver-source or --romdump")
        if args.vendor_slot and not args.driver_profile:
            raise BuildError("--vendor-slot requires --driver-profile")
        if args.debug_adb_source and not args.debug_adb:
            raise BuildError("--debug-adb-source requires --debug-adb")
        if args.jobs < 1:
            raise BuildError("--jobs must be at least 1")
        require_tools("bash", "cmake", "git", "make", "readelf", "grep", "install")
        if not args.skip_tests:
            require_tools("cc", "ctest")
        abis = configured_abis()
        version = pinned_ondk_version()
        sdk = find_sdk(args.sdk, os.environ, Path.home(), version)
        say(f"Android SDK: {sdk}")
        env = os.environ.copy()
        env["ANDROID_HOME"] = str(sdk)
        env["ANDROID_SDK_ROOT"] = str(sdk)
        env["GROM_PYTHON"] = sys.executable
        if args.release:
            env["GROM_RELEASE"] = "1"
        else:
            env.pop("GROM_RELEASE", None)
        if args.offline:
            env["CARGO_NET_OFFLINE"] = "true"
        ensure_ondk(sdk, version, offline=args.offline, env=env)
        (ROOT / "out/grombake").mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="image-", dir=ROOT / "out/grombake") as tmp, ExitStack() as stack:
            workdir = Path(tmp)
            image = None
            tool = None
            rom = None
            image_path = args.image
            if args.image and (args.image.suffix.lower() == '.zip' or
                               (args.image.is_dir() and args.driver_profile and not args.driver_source)):
                rom = stack.enter_context(romdump.RomDump(args.image))
                image_path = rom.boot_image(workdir / 'rom-boot')
            if args.romdump:
                rom = stack.enter_context(romdump.RomDump(args.romdump))
            if args.image:
                if args.magiskboot:
                    tool = args.magiskboot.expanduser().resolve()
                    if not tool.is_file() or not os.access(tool, os.X_OK):
                        raise BuildError(f"magiskboot is not executable: {tool}")
                else:
                    tool = pinned_magiskboot(workdir, env, args.release)
                image = image_builder.prepare_image(image_path, workdir, tool, args.arch)
                abis = [image.arch]
                say(f"detected {image.arch} from {image.source.name} ({image.kind})")
                config = workdir / "grom-target.prop"
                write_abi_config(config, abis)
                env["GROM_CONFIG"] = str(config)
            bundle = None
            if args.driver_profile:
                assert image is not None
                profile = args.driver_profile
                if not profile.is_file():
                    if profile.parent != Path("."):
                        raise BuildError(f"driver profile does not exist: {profile}")
                    filename = profile.name if profile.suffix == ".json" else profile.name + ".json"
                    profile = ROOT / "grom/devices" / filename
                target_release = driver_bundle.kernel_release(image.workdir / "kernel")
                if target_release is None:
                    for name in ("kernel-ranchu", "kernel", "Image"):
                        target_release = driver_bundle.kernel_release(image.source.parent / name)
                        if target_release:
                            break
                _, modules, firmware = driver_bundle.requirements(profile, image.arch, target_release)
                if args.driver_source:
                    source = args.driver_source.expanduser().resolve()
                    if not source.is_dir():
                        raise BuildError(f"driver source is not a directory: {source}")
                else:
                    if rom is None:
                        raise BuildError("driver bundling needs --driver-source or a ROM ZIP/directory (or --romdump)")
                    source = romdump.extract_modules(rom, modules, workdir / 'rom-vendor',
                                                     args.vendor_slot, firmware)
                bundle = driver_bundle.load(profile, source, image.arch, target_release)
                say(f"driver profile {bundle.name}: {len(bundle.modules)} modules")
            say("building combined init for " + ", ".join(abis))
            (OUTPUT / "build.json").unlink(missing_ok=True)
            run(["bash", str(BUILD_SCRIPT)], env=env)
            if not args.skip_tests:
                say("running builder checks")
                run([sys.executable, "-m", "unittest", "discover", "-s", "tests",
                     "-p", "test_grombake.py"])
                say("building and running static gromui host tests")
                run(["cmake", "-S", str(ROOT / "gromui"), "-B", str(TEST_BUILD),
                     "-DGROMUI_LVGL=ON", "-DBUILD_TESTING=ON",
                     "-DCMAKE_BUILD_TYPE=MinSizeRel"])
                run(["cmake", "--build", str(TEST_BUILD), "--parallel", str(args.jobs)])
                run(["ctest", "--test-dir", str(TEST_BUILD), "--output-on-failure"])
            image_info = None
            if image:
                assert tool is not None
                binary = OUTPUT / image.arch / "init"
                check_static_elf(binary, image.arch)
                if args.output:
                    destination = args.output
                elif rom:
                    name = re.sub(r'[^A-Za-z0-9_.-]+', '-', rom.source.stem)
                    destination = ROOT / 'out/grom-images' / f'{name}-grom-{image.source.name}'
                else:
                    digest = hashlib.sha256(image.source.read_bytes()).hexdigest()[:8]
                    name = f"{image.source.parent.name}-{image.source.stem}-{digest}-grom"
                    destination = ROOT / "out/grom-images" / (name + image.source.suffix)
                say(f"patching a copy of {image.source} -> {destination}")
                adb_source = None
                if args.debug_adb:
                    if args.debug_adb_source:
                        adb_source = args.debug_adb_source.expanduser().resolve()
                    elif args.driver_profile and "motogfast" in args.driver_profile.name:
                        adb_source = ROOT / "motogfast/twrp-3.5.0-0-rav-sofia.img"
                    else:
                        raise BuildError("--debug-adb needs --debug-adb-source for this target")
                    if not adb_source.exists():
                        raise BuildError(f"ADB source does not exist: {adb_source}")
                image_info = image_builder.patch_image(image, binary, destination, tool, bundle,
                                                       args.touch_diagnostics, adb_source)
                if args.image and args.image.suffix.lower() == '.zip':
                    image_info['requested_input'] = str(args.image.expanduser().resolve())
                    image_info['patched_input'] = f"{args.image.expanduser().resolve()}!{image.source.name}"
                if rom:
                    image_info['romdump_source'] = str(rom.source)
                    if args.vendor_slot:
                        image_info['vendor_slot'] = args.vendor_slot
                image_info["magiskboot_sha256"] = hashlib.sha256(tool.read_bytes()).hexdigest()
                sidecar = Path(image_info["output"] + ".json")
                sidecar.write_text(json.dumps(image_info, indent=2) + "\n")
            manifest = write_manifest(abis, version, args.release,
                                      not args.skip_tests, image_info)
            say(f"verified static outputs; manifest: {manifest}")
            if image_info:
                say(f"verified patched image: {image_info['output']}")
            say("These outputs remain preboot scaffolds; Android handoff is not implemented.")
        return 0
    except (BuildError, driver_bundle.BundleError, debug_adb.AdbError,
            image_builder.ImageError,
            romdump.RomError, OSError, zipfile.BadZipFile) as exc:
        print(f"[grombake] ERROR: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
