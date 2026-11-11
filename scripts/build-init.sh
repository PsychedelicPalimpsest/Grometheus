#!/usr/bin/env bash
set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
sdk=${ANDROID_HOME:-${ANDROID_SDK_ROOT:-$HOME/Android/Sdk}}
lvgl_dir=$root/out/gromui-vendor/vendor/lvgl
config=${GROM_CONFIG:-$root/config/magisk.prop}
python=${GROM_PYTHON:-python3}
magisk_options=()
if [[ ${GROM_RELEASE:-0} == 1 ]]; then
    magisk_options+=(-r)
fi
abi_list=$(sed -n 's/^abiList=//p' "$config")
IFS=, read -r -a abis <<< "$abi_list"
if ((${#abis[@]} == 0)); then
    echo "No ABIs configured in config/magisk.prop" >&2
    exit 1
fi

# gromui's CMake configuration verifies and unpacks the pinned LVGL archive.
cmake -S "$root/gromui" -B "$root/out/gromui-vendor" \
    -DGROMUI_LVGL=ON -DBUILD_TESTING=OFF >/dev/null

(
    cd "$root/vendor/magisk"
    ANDROID_HOME=$sdk GROM_LVGL_DIR=$lvgl_dir \
        "$python" build.py "${magisk_options[@]}" -c "$config" native magiskinit
)

for abi in "${abis[@]}"; do
    source=$root/vendor/magisk/native/out/$abi/magiskinit
    destination=$root/out/grom-init/$abi/init
    install -Dm755 "$source" "$destination"
    if readelf -l "$destination" | grep -Eq 'INTERP|DYNAMIC' ||
       ! readelf -d "$destination" | grep -q 'There is no dynamic section'; then
        echo "Combined init is not fully static: $destination" >&2
        exit 1
    fi
    echo "Built $destination"
done
