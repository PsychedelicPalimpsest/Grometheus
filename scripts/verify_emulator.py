#!/usr/bin/env python3
"""Boot an isolated SDK emulator, capture real output, stop the owned process group."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import signal
import socket
import subprocess
import sys
import time

p = argparse.ArgumentParser()
p.add_argument("name")
p.add_argument("--backend", default="auto")
p.add_argument("--kernel")
p.add_argument("--modules")
p.add_argument("--binary")
p.add_argument("--gpu-module", required=True)
p.add_argument("--avd", default="test_device")
a = p.parse_args()
with socket.socket() as port_check:
    if port_check.connect_ex(("127.0.0.1", 8556)) == 0:
        raise SystemExit(
            "Port 8556 is in use; stop the other emulator before verification"
        )
root = Path(__file__).resolve().parent.parent
env = dict(os.environ, MUI_BACKEND=a.backend)
for k, v in [
    ("MUI_KERNEL", a.kernel),
    ("MUI_MODULES", a.modules),
    ("MUI_BINARY", a.binary),
]:
    if v:
        env[k] = str(Path(v).resolve())
log = root / "docs" / (a.name + "-runner.log")
with log.open("w+") as output:
    proc = subprocess.Popen(
        [
            str(root / "scripts/run_emulator.sh"),
            a.avd,
            "--headless",
            "--virtio-gpu",
            str(Path(a.gpu_module).resolve()),
            "--",
            "-grpc",
            "8556",
        ],
        env=env,
        stdout=output,
        stderr=subprocess.STDOUT,
        start_new_session=True,
    )
    try:
        work = None
        deadline = time.monotonic() + 60
        while time.monotonic() < deadline:
            output.flush()
            text = log.read_text()
            match = re.search(r"work directory: (\S+)", text)
            if match:
                work = Path(match[1])
                guest = work / "emulator.log"
                if guest.exists() and "miniui backend=" in guest.read_text(
                    errors="replace"
                ):
                    break
            if proc.poll() is not None:
                raise RuntimeError(text)
            time.sleep(0.2)
        else:
            raise RuntimeError("No successful display open; see " + str(work))
        time.sleep(0.2)
        before = root / "docs" / (a.name + "-before.png")
        subprocess.run(
            [sys.executable, str(root / "scripts/capture.py"), str(before)], check=True
        )
        time.sleep(5)
        subprocess.run(
            [
                sys.executable,
                str(root / "scripts/capture.py"),
                str(root / "docs" / (a.name + ".png")),
            ],
            check=True,
        )
        (root / "docs" / (a.name + ".log")).write_bytes(guest.read_bytes())
        from PIL import Image, ImageChops

        final = Image.open(root / "docs" / (a.name + ".png")).convert("RGB")
        initial = Image.open(before).convert("RGB")
        if final.size != (800, 600):
            raise RuntimeError("Unexpected emulator resolution")
        lvgl = a.binary and "lvgl" in a.binary
        colors = [
            (255, 0, 0),
            (0, 255, 0),
            (0, 0, 255),
            (255, 255, 255),
            (0, 255, 255),
            (255, 0, 255),
        ]
        for i, color in enumerate(colors):
            point = (90 + i * 120, 240) if lvgl else (50 + i * 100, 100)
            if final.getpixel(point) != color:
                raise RuntimeError(f"Wrong displayed color at {point}")
        if lvgl:
            if ImageChops.difference(
                initial.crop((0, 0, 800, 290)), final.crop((0, 0, 800, 290))
            ).getbbox():
                raise RuntimeError(
                    "Partial updates damaged previously rendered top area"
                )
            if not ImageChops.difference(initial, final).getbbox():
                raise RuntimeError("No animation change captured")
        binary = Path(a.binary).resolve() if a.binary else root / "build/miniui-init"
        metadata = dict(
            backend_request=a.backend,
            kernel=a.kernel or "SDK Android 13 kernel-ranchu",
            binary_sha256=hashlib.sha256(binary.read_bytes()).hexdigest(),
            screenshot_sha256=hashlib.sha256(
                (root / "docs" / (a.name + ".png")).read_bytes()
            ).hexdigest(),
            dimensions=list(final.size),
            pixel_checks="passed",
            workdir=str(work),
        )
        (root / "docs" / (a.name + ".json")).write_text(
            json.dumps(metadata, indent=2) + "\n"
        )
        print(work)
    finally:
        os.killpg(proc.pid, signal.SIGTERM)
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            os.killpg(proc.pid, signal.SIGKILL)
            proc.wait()
