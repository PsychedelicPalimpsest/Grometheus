"""Checks for toolchain selection and ELF validation in the build pipeline."""

from pathlib import Path
import struct
import tempfile
import unittest

import grombake


class BuilderChecks(unittest.TestCase):
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
