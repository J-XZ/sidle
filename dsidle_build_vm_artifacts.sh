#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
image="$root/image/root.img"
out="$root/build-jammy"

while (($#)); do
  case "$1" in
    --image) image=$2; shift 2;;
    --out-dir) out=$2; shift 2;;
    --help) echo "usage: $0 [--image PATH] [--out-dir DIR]"; exit 0;;
    *) echo "usage: $0 [--image PATH] [--out-dir DIR]" >&2; exit 2;;
  esac
done
[[ -s "$image" ]] || { echo "missing VM image: $image" >&2; exit 2; }
mkdir -p "$out"
# Build once outside VMs in the exact Jammy root filesystem used by the guests.
systemd-nspawn --quiet --image="$image" --bind="$root:/src" --bind="$out:/build" /bin/bash -lc \
  'cmake -S /src -B /build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ && cmake --build /build --target dsidle_e2e_suite_runner dsidle_e2e_trace_runner -j8 && make -C /src/third_party/ivshmem-kernel KDIR=/lib/modules/5.15.0-25-generic/build && cp /src/third_party/ivshmem-kernel/ivshmem_driver.ko /build/'
test -x "$out/dsidle_e2e_suite_runner"
test -x "$out/dsidle_e2e_trace_runner"
test -s "$out/ivshmem_driver.ko"
echo "DSIDLE_VM_ARTIFACTS_OK out_dir=$out"
