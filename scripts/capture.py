#!/usr/bin/env python3
"""SDK screenshot endpoint: captures what the emulator displays, without guest adb.
Requires grpcio, grpcio-tools and pillow in the invoking Python environment.
"""

import argparse
import importlib
import os
from pathlib import Path
import subprocess
import sys
import tempfile

p = argparse.ArgumentParser()
p.add_argument("output")
p.add_argument("--port", type=int, default=8556)
p.add_argument(
    "--sdk",
    default=os.environ.get("ANDROID_SDK_ROOT", str(Path.home() / "Android/Sdk")),
)
a = p.parse_args()
with tempfile.TemporaryDirectory() as tmp:
    proto = Path(a.sdk) / "emulator/lib"
    subprocess.run(
        [
            sys.executable,
            "-m",
            "grpc_tools.protoc",
            "-I" + str(proto),
            "--python_out=" + tmp,
            "--grpc_python_out=" + tmp,
            str(proto / "emulator_controller.proto"),
        ],
        check=True,
    )
    sys.path.insert(0, tmp)
    import grpc
    from PIL import Image
    import io

    pb = importlib.import_module("emulator_controller_pb2")
    service = importlib.import_module("emulator_controller_pb2_grpc")
    with grpc.insecure_channel(f"127.0.0.1:{a.port}") as channel:
        result = service.EmulatorControllerStub(channel).getScreenshot(
            pb.ImageFormat(format=pb.ImageFormat.PNG), timeout=10
        )
    if not result.image:
        raise SystemExit("Emulator returned an empty screenshot")
    Image.open(io.BytesIO(result.image)).verify()
    Path(a.output).write_bytes(result.image)
    print(f"{a.output}: {result.format.width}x{result.format.height}")
