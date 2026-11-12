"""Checks for toolchain selection and ELF validation in the build pipeline."""

from pathlib import Path
import struct
import tempfile
import unittest

import grombake
from scripts import driver_bundle, image_builder


def newc_entry(name: str, mode: int) -> bytes:
    fields = [0, mode, 0, 0, 1, 0, 0, 0, 0, 0, 0, len(name) + 1, 0]
    header = b"070701" + b"".join(f"{value:08x}".encode() for value in fields)
    record = header + name.encode() + b"\0"
    return record + b"\0" * (-len(record) % 4)


class BuilderChecks(unittest.TestCase):
    def test_ramdisk_rejects_nested_file_without_parent_directory(self):
        with tempfile.TemporaryDirectory() as temp:
            archive = Path(temp) / "ramdisk.cpio"
            file = newc_entry("grom/debug-adb", 0o100644)
            trailer = newc_entry("TRAILER!!!", 0)
            archive.write_bytes(file + trailer)
            with self.assertRaisesRegex(image_builder.ImageError, "lacks directory /grom"):
                image_builder.verify_ramdisk_parents(archive, ["grom/debug-adb"])
            archive.write_bytes(newc_entry("grom", 0o040755) + file + trailer)
            image_builder.verify_ramdisk_parents(archive, ["grom/debug-adb"])

    def test_driver_profile_stages_order_and_rejects_wrong_kernel(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            source = root / "source/lib/modules"
            source.mkdir(parents=True)
            # The profile accepts only an uncompressed module for the target ABI.
            module = bytearray(128)
            module[:6] = b"\x7fELF\x02\x01"
            struct.pack_into("<H", module, 18, 183)
            module.extend(b"vermagic=4.14.117-perf+ SMP\0")
            (source / "touch.ko").write_bytes(module)
            profile = root / "profile.json"
            profile.write_text('{"schema":1,"name":"test","arch":"arm64-v8a",'
                               '"kernel_release":"4.14.117-perf+","modules":["touch.ko"]}')
            with self.assertRaisesRegex(driver_bundle.BundleError, "differs from target"):
                driver_bundle.load(profile, root / "source", "arm64-v8a", "4.14.180-perf+")
            bundle = driver_bundle.load(profile, root / "source", "arm64-v8a",
                                        "4.14.117-perf+")
            destination = root / "stage"
            driver_bundle.stage(bundle, destination)
            self.assertEqual((destination / "grom/drivers/modules.load").read_text(),
                             "touch.ko\n")
            self.assertEqual((destination / "grom/drivers/modules/touch.ko").read_bytes(),
                             module)

    def test_selects_complete_toolchain_over_wrong_environment(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            wrong = root / "system-sdk"
            wrong.mkdir()
            home = root / "home"
            complete = home / "Android/Sdk/ndk/magisk"
            (complete / "toolchains/rust/bin").mkdir(parents=True)
            (complete / "ONDK_VERSION").write_text("r29.2\n")
            (complete / "ndk-build").touch()
            (complete / "toolchains/rust/bin/cargo").touch()
            selected = grombake.find_sdk(None, {"ANDROID_HOME": str(wrong)},
                                          home, "r29.2")
            self.assertEqual(selected, complete.parent.parent)
            self.assertEqual(grombake.find_sdk(str(wrong), {}, home, "r29.2"), wrong)

    def test_offline_missing_ondk_fails_without_installing(self):
        with tempfile.TemporaryDirectory() as temp:
            sdk = Path(temp)
            with self.assertRaisesRegex(grombake.BuildError, "--offline forbids download"):
                grombake.ensure_ondk(sdk, "r29.2", offline=True, env={})
            self.assertFalse((sdk / "ndk/magisk").exists())

    def test_rejects_dynamic_or_wrong_architecture(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "init"
            data = bytearray(64 + 56 * 2)
            data[:6] = b"\x7fELF\x02\x01"
            struct.pack_into("<H", data, 18, 62)
            struct.pack_into("<Q", data, 32, 64)
            struct.pack_into("<HH", data, 54, 56, 2)
            struct.pack_into("<I", data, 64, 1)  # PT_LOAD
            path.write_bytes(data)
            self.assertTrue(grombake.check_static_elf(path, "x86_64")["static"])
            with self.assertRaisesRegex(grombake.BuildError, "expected 183"):
                grombake.check_static_elf(path, "arm64-v8a")
            struct.pack_into("<I", data, 64 + 56, 2)  # PT_DYNAMIC
            path.write_bytes(data)
            with self.assertRaisesRegex(grombake.BuildError, "dynamic linking"):
                grombake.check_static_elf(path, "x86_64")


if __name__ == "__main__":
    unittest.main()
