#!/usr/bin/env bash
# Boot a static x86_64 gromui example from a copied Android SDK ramdisk.
set -euo pipefail

usage() {
    cat <<'EOF'
Usage: scripts/run_emulator.sh [AVD] [--headless] [--virtio-gpu MODULE] [-- EMULATOR_ARGS...]

Boots an existing x86_64 gromui-init binary as /init in a temporary copy of
an SDK AVD ramdisk. Defaults to test_device and build/gromui-init.
GROMUI_BINARY selects a different static x86_64 executable.
GROMUI_RAMDISK_IMAGE selects a ramdisk already patched by grombake.py.
GROMUI_BACKEND selects auto (default), drm, fbdev, or test-only auto-fbdev.
GROMUI_SCREEN_SIZE selects the test display size (default 800x600).
GROMUI_KERNEL + GROMUI_MODULES select a test kernel and matching uncompressed modules.
The shared grom device profiles choose the ordered modules staged in the ramdisk.
The stock Android 13 SDK kernel has no CONFIG_FB; fbdev needs a test kernel.
ANDROID_SDK_ROOT selects the SDK. Sources and SDK images remain untouched.
Use -- -grpc 8556 to enable the local SDK screenshot endpoint.

EOF
}

avd=test_device
backend=${GROMUI_BACKEND:-auto}
screen_size=${GROMUI_SCREEN_SIZE:-800x600}
if [[ ! $screen_size =~ ^([1-9][0-9]{1,3})x([1-9][0-9]{1,3})$ ]]; then
    echo "Invalid GROMUI_SCREEN_SIZE: $screen_size" >&2
    exit 2
fi
screen_width=${BASH_REMATCH[1]}
screen_height=${BASH_REMATCH[2]}
if [[ $# -gt 0 && $1 != -* ]]; then
    avd=$1
    shift
fi
headless=0
virtio_gpu=
emulator_args=()
while (($#)); do
    case $1 in
        --headless)
            headless=1
            shift
            ;;
        --virtio-gpu)
            if (($# < 2)); then
                echo "--virtio-gpu needs a module path" >&2
                exit 2
            fi
            virtio_gpu=$2
            shift 2
            ;;
        --)
            shift
            emulator_args=("$@")
            break
            ;;
        -h | --help)
            usage
            exit 0
            ;;
        *)
            echo "Unknown option: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

for tool in python3 lz4 cpio; do
    command -v "$tool" >/dev/null || {
        echo "Missing required tool: $tool" >&2
        exit 1
    }
done
if [[ -n $virtio_gpu && ! -f $virtio_gpu ]]; then
    echo "Module not found: $virtio_gpu" >&2
    exit 1
fi

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
project_root=$(cd -- "$root/.." && pwd)
sdk_root=${ANDROID_SDK_ROOT:-$HOME/Android/Sdk}
if [[ ! -x $sdk_root/emulator/emulator && -n ${ANDROID_HOME:-} ]]; then
    sdk_root=$ANDROID_HOME
fi
sdk_root=$(cd -- "$sdk_root" && pwd)
emulator=$sdk_root/emulator/emulator
[[ -x $emulator ]] || {
    echo "Emulator not found: $emulator" >&2
    exit 1
}

avd_home=${ANDROID_AVD_HOME:-${ANDROID_USER_HOME:-$HOME/.android}/avd}
avd_ini=$avd_home/$avd.ini
[[ -f $avd_ini ]] || {
    echo "AVD not found: $avd_ini" >&2
    exit 1
}
avd_dir=$(sed -n 's/^path=//p' "$avd_ini" | head -n 1)
[[ -n $avd_dir && -f $avd_dir/config.ini ]] || {
    echo "Cannot find configuration for AVD $avd" >&2
    exit 1
}
config=$avd_dir/config.ini
config_value() {
    awk -F= -v key="$1" '$1 ~ "^[[:space:]]*" key "[[:space:]]*$" { sub(/^[[:space:]]+/, "", $2); sub(/[[:space:]]+$/, "", $2); print $2; exit }' "$config"
}
abi=$(config_value abi.type)
[[ $abi == x86_64 ]] || {
    echo "AVD $avd uses $abi; this runner requires x86_64" >&2
    exit 1
}
sysdir=$(config_value image.sysdir.1)
[[ -n $sysdir ]] || {
    echo "AVD $avd has no image.sysdir.1" >&2
    exit 1
}
if [[ $sysdir != /* ]]; then sysdir=$sdk_root/$sysdir; fi
sysdir=$(cd -- "$sysdir" && pwd)
stock_ramdisk=$sysdir/ramdisk.img
[[ -f $stock_ramdisk ]] || {
    echo "Stock ramdisk not found: $stock_ramdisk" >&2
    exit 1
}

workdir=$(mktemp -d "${TMPDIR:-/tmp}/gromui-emulator.XXXXXXXX")
stage=$workdir/stage
mkdir -p "$stage"
echo "gromui emulator work directory: $workdir"

if [[ -z ${GROMUI_KERNEL:-} && -z $virtio_gpu ]]; then
    command -v debugfs >/dev/null || {
        echo "Missing required tool: debugfs" >&2
        exit 1
    }
    vendor_image=$sysdir/vendor.img
    [[ -f $vendor_image ]] || {
        echo "Vendor image not found: $vendor_image" >&2
        exit 1
    }
    echo "Extracting the AVD's virtio-gpu module..."
    python3 "$root/scripts/ramdisk_tools.py" vendor "$vendor_image" "$workdir/vendor.ext4"
    virtio_gpu=$workdir/virtio-gpu.ko
    debugfs -R "dump /lib/modules/virtio-gpu.ko $virtio_gpu" \
        "$workdir/vendor.ext4" >"$workdir/debugfs.log" 2>&1
    [[ -s $virtio_gpu ]] || {
        echo "Could not extract virtio-gpu.ko from $vendor_image" >&2
        cat "$workdir/debugfs.log" >&2
        exit 1
    }
    virtio_input=$workdir/virtio_input.ko
    debugfs -R "dump /lib/modules/virtio_input.ko $virtio_input" \
        "$workdir/vendor.ext4" >>"$workdir/debugfs.log" 2>&1
    [[ -s $virtio_input ]] || {
        echo "Could not extract virtio_input.ko from $vendor_image" >&2
        cat "$workdir/debugfs.log" >&2
        exit 1
    }
    rm -- "$workdir/vendor.ext4"
fi

binary=${GROMUI_BINARY:-$root/build/gromui-init}
source_ramdisk=${GROMUI_RAMDISK_IMAGE:-$stock_ramdisk}
[[ -f $source_ramdisk ]] || {
    echo "Ramdisk not found: $source_ramdisk" >&2
    exit 1
}
echo "Staging a copy of the ramdisk: $source_ramdisk"
lz4 -d -q "$source_ramdisk" "$workdir/stock.cpio"
python3 "$root/scripts/ramdisk_tools.py" split \
    "$workdir/stock.cpio" "$workdir/first.cpio" "$workdir/remainder.cpio"
(
    cd "$stage"
    cpio -id --quiet --no-absolute-filenames <"$workdir/first.cpio"
)
[[ -f $stage/init && ! -L $stage/init ]] || {
    echo "Ramdisk has no regular /init" >&2
    exit 1
}
if [[ -n ${GROMUI_RAMDISK_IMAGE:-} ]]; then
    [[ -f $stage/init.orig && ! -L $stage/init.orig ]] || {
        echo "Patched ramdisk has no regular /init.orig" >&2
        exit 1
    }
    if [[ -n ${GROMUI_BINARY:-} ]]; then
        cmp "$binary" "$stage/init" || {
            echo "Patched ramdisk /init differs from GROMUI_BINARY" >&2
            exit 1
        }
    fi
else
    mv "$stage/init" "$stage/init.orig"
    install -m 755 "$binary" "$stage/init"
fi
printf "%s\n" "$backend" >"$stage/gromui-backend"
if [[ -n ${GROMUI_KERNEL:-} && -f $stage/grom/drivers/modules.load ]]; then
    echo "Ramdisk already contains a driver bundle; rebuild it for GROMUI_KERNEL" >&2
    exit 1
fi
if [[ ! -f $stage/grom/drivers/modules.load ]]; then
    driver_source=$workdir/driver-source
    driver_profile=$project_root/grom/devices/sdk-emulator.json
    mkdir -p "$driver_source/lib/modules"
    if [[ -n ${GROMUI_KERNEL:-} ]]; then
        [[ -d ${GROMUI_MODULES:-} ]] || {
            echo "GROMUI_KERNEL requires GROMUI_MODULES" >&2
            exit 1
        }
        driver_source=$GROMUI_MODULES
        driver_profile=$project_root/grom/devices/sdk-emulator-fbdev.json
    else
        install -m 644 "$virtio_gpu" "$driver_source/lib/modules/virtio-gpu.ko"
        install -m 644 "$virtio_input" "$driver_source/lib/modules/virtio_input.ko"
        if [[ -s $workdir/remainder.cpio ]]; then
            (
                cd "$driver_source"
                cpio -id --quiet --no-absolute-filenames 'lib/modules/virtio_*.ko' \
                    <"$workdir/remainder.cpio"
            )
        fi
    fi
    python3 "$project_root/scripts/driver_bundle.py" \
        --profile "$driver_profile" --source "$driver_source" \
        --arch x86_64 --destination "$stage"
fi
(
    cd "$stage"
    find . -mindepth 1 -print0 |
        cpio --null -o -H newc --owner=0:0 --quiet >"$workdir/patched-first.cpio"
)
if [[ -n ${GROMUI_KERNEL:-} ]]; then
    cp "$workdir/patched-first.cpio" "$workdir/patched.cpio"
else
    cat "$workdir/patched-first.cpio" "$workdir/remainder.cpio" >"$workdir/patched.cpio"
fi
lz4 -l -z -q "$workdir/patched.cpio" "$workdir/ramdisk.img"

options=(-avd "$avd" -sysdir "$sysdir" -ramdisk "$workdir/ramdisk.img"
    -skin "$screen_size" -no-snapshot -no-boot-anim -gpu off -show-kernel)
if [[ -n ${GROMUI_KERNEL:-} ]]; then options+=(-kernel "$GROMUI_KERNEL"); fi
options+=(-read-only)
if ((headless)); then options+=(-no-window -no-audio); fi
echo "Starting emulator. Kernel and gromui log: $workdir/emulator.log"
ANDROID_SDK_ROOT=$sdk_root ANDROID_HOME=$sdk_root \
    "$emulator" "${options[@]}" "${emulator_args[@]}" \
    -qemu -device "virtio-vga,id=mui-gpu,virgl=off,xres=$screen_width,yres=$screen_height" -monitor unix:$workdir/monitor.sock,server,nowait >"$workdir/emulator.log" 2>&1 || {
    status=$?
    tail -n 40 "$workdir/emulator.log" >&2
    exit "$status"
}
