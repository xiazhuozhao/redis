#!/usr/bin/env bash
# Build an RVV Redis with target-trained PGO. The script performs all builds
# locally and only runs the instrumented training workload on the target.
# It intentionally never invokes sudo.
set -euo pipefail

repo=$(cd "$(dirname "$0")/.." && pwd)
jobs=${JOBS:-$(getconf _NPROCESSORS_ONLN)}
target_host=${TARGET_HOST:-muse-pi-pro}
target_root=${TARGET_ROOT:-/home/xzz/rvspoc-S2603}
target_port=${TARGET_PORT:-11308}
sysroot=${RISCV_SYSROOT:-/home/xzz2/1/tools/jammy-cross/root}
gcc11=${RISCV_GCC11:-$sysroot/usr/bin/riscv64-linux-gnu-gcc-11}
rvv_gcc=${RVV_GCC:-/home/xzz2/1/tools/riscv-gnu/bin/riscv64-unknown-linux-gnu-gcc}
profile_dir=${PROFILE_DIR:-/tmp/redis-rvv-pgo-$$}
profile_name=$(basename "$profile_dir")
profile_archive=/tmp/$profile_name.tgz

case "$profile_dir" in
    /tmp/redis-rvv-pgo-*) ;;
    *) echo "PROFILE_DIR must be below /tmp and start with redis-rvv-pgo-" >&2; exit 2 ;;
esac
if [[ -e "$profile_dir" || -e "$profile_archive" ]]; then
    echo "Refusing to overwrite existing profile data: $profile_dir" >&2
    exit 2
fi

host_lib=$sysroot/usr/lib/x86_64-linux-gnu
target_lib=$sysroot/usr/riscv64-linux-gnu/lib
gcc11_lib=$sysroot/usr/lib/gcc-cross/riscv64-linux-gnu/11
target_bin=$sysroot/usr/riscv64-linux-gnu/bin

scalar_cc="env LD_LIBRARY_PATH=$host_lib $gcc11 --sysroot=$sysroot -isystem $sysroot/usr/riscv64-linux-gnu/include -B$gcc11_lib/ -B$target_bin/ -L$target_lib"
vector_cc="$rvv_gcc --sysroot=$sysroot -isystem $sysroot/usr/riscv64-linux-gnu/include -B$target_lib/ -L$target_lib"
make_args=(
    -C "$repo/src" -j"$jobs" BUILD_RVV=yes MALLOC=libc BUILD_TLS=no
    "CC=$scalar_cc" "RVV_CC=$vector_cc"
)

generate_opt="-O3 -march=rv64gc -mabi=lp64d -fno-tree-vectorize -fno-pie -no-pie -fprofile-generate=$profile_dir -fprofile-prefix-path=$repo -fprofile-update=atomic"
use_opt="-O3 -march=rv64gc -mabi=lp64d -fno-tree-vectorize -fprofile-use=$profile_dir -fprofile-prefix-path=$repo -fprofile-correction -Wno-missing-profile -flto=auto -fomit-frame-pointer -fno-semantic-interposition -fno-plt -fno-pie -no-pie"

make -C "$repo/src" clean >/dev/null
make "${make_args[@]}" \
    "RVV_CFLAGS=-fno-profile-generate -fno-pie -g0 -march=rv64gcv -mabi=lp64d" \
    "OPT=$generate_opt" redis-server

ssh "$target_host" mkdir -p "$target_root/bin-rvv" "$target_root/run"
scp "$repo/src/redis-server" "$target_host:$target_root/bin-rvv/redis-server-pgogen"
scp "$repo/tests/rvv_resp_smoke.py" "$target_host:$target_root/rvv_resp_smoke.py"

ssh "$target_host" bash -s -- "$target_root" "$profile_dir" "$target_port" <<'REMOTE'
set -euo pipefail
target_root=$1
profile_dir=$2
port=$3
binary=$target_root/bin-rvv/redis-server-pgogen
memtier=$target_root/memtier-src/memtier_benchmark
pidfile=$target_root/run/rvv-pgo.pid
logfile=$target_root/run/rvv-pgo.log

test ! -e "$profile_dir"
mkdir "$profile_dir"
cleanup() {
    if [[ -f "$pidfile" ]]; then
        pid=$(cat "$pidfile")
        if kill -0 "$pid" 2>/dev/null; then
            kill "$pid"
            while kill -0 "$pid" 2>/dev/null; do sleep 0.05; done
        fi
    fi
}
trap cleanup EXIT

chmod +x "$binary"
taskset -c 7 "$binary" --port "$port" --bind 127.0.0.1 --save "" \
    --appendonly no --daemonize yes --pidfile "$pidfile" --logfile "$logfile"
python3 "$target_root/rvv_resp_smoke.py" --host 127.0.0.1 --port "$port"

# Populate the whole key range, then train the hit-heavy pipeline and the
# high-value copy paths used by validation.
taskset -c 0-3 "$memtier" --server 127.0.0.1 --port "$port" --protocol redis \
    --threads 2 --clients 25 --pipeline 16 --ratio 1:0 --data-size 32 \
    --key-minimum 1 --key-maximum 10000 --key-pattern P:P --requests 400 \
    --hide-histogram >/dev/null
taskset -c 0-3 "$memtier" --server 127.0.0.1 --port "$port" --protocol redis \
    --threads 2 --clients 25 --pipeline 16 --ratio 1:1 --data-size 32 \
    --key-minimum 1 --key-maximum 10000 --key-pattern R:R --test-time 25 \
    --hide-histogram >/dev/null
taskset -c 0-3 "$memtier" --server 127.0.0.1 --port "$port" --protocol redis \
    --threads 2 --clients 25 --pipeline 4 --ratio 1:1 --data-size 4096 \
    --key-minimum 1 --key-maximum 100000 --key-pattern R:R --test-time 8 \
    --hide-histogram >/dev/null

cleanup
trap - EXIT
tar -C /tmp -czf "$target_root/run/$(basename "$profile_dir").tgz" "$(basename "$profile_dir")"
REMOTE

scp "$target_host:$target_root/run/$profile_name.tgz" "$profile_archive"
tar -C /tmp -xzf "$profile_archive"
test "$(find "$profile_dir" -type f | wc -l)" -ge 100

make -C "$repo/src" clean >/dev/null
make "${make_args[@]}" \
    "RVV_CFLAGS=-fno-profile-use -fno-lto -fno-pie -g0 -march=rv64gcv -mabi=lp64d" \
    "OPT=$use_opt" redis-server

if [[ ${DEPLOY_FINAL:-0} == 1 ]]; then
    scp "$repo/src/redis-server" "$target_host:$target_root/bin-rvv/redis-server"
fi
echo "RVV PGO binary: $repo/src/redis-server"
