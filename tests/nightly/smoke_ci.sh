#!/usr/bin/env bash
# SPDX-FileCopyrightText: Copyright (c) 2026 T-Head (Shanghai) Semiconductor Co., Ltd. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
set -euo pipefail
umask 022
scripts=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)

fail() { printf '%s\n' "$*" >&2; exit 1; }

sdk_env() {
    # Vendor environment scripts need not be nounset-safe.
    set +u
    # shellcheck disable=SC1091
    source /usr/local/PPU_SDK/envsetup.sh
    set -u
    test -x "${PPU_SDK:?}/bin/hgcc"
}

ubuntu_packages() {
    local missing=() package
    for package in "$@"; do
        if [[ $(dpkg-query -W -f='${db:Status-Status}' "$package" 2>/dev/null) != installed ]]; then
            missing+=("$package")
        fi
    done
    if (( ${#missing[@]} )); then
        # Do not use any image-baked private apt sources in this public workflow.
        printf 'deb https://mirrors.aliyun.com/ubuntu noble main universe\ndeb https://mirrors.aliyun.com/ubuntu noble-updates main universe\ndeb https://mirrors.aliyun.com/ubuntu noble-security main universe\n' > /tmp/sailbandwidth-smoke.list
        local opts=(-o Dir::Etc::sourcelist=/tmp/sailbandwidth-smoke.list -o Dir::Etc::sourceparts=-)
        apt-get "${opts[@]}" -o Acquire::Retries=3 update
        DEBIAN_FRONTEND=noninteractive apt-get "${opts[@]}" install -y --no-install-recommends "${missing[@]}"
    fi
}

mpi_version() {
    dpkg-query -W -f='${Version}\n' openmpi-bin libopenmpi3t64
}

nas_run() {
    [[ ${SMOKE_KEY:?} =~ ^[0-9]+-[0-9]+$ ]] || fail 'Invalid smoke run key'
    local mount=${SMOKE_NAS_MOUNT:?} subdir=${SMOKE_NAS_SUBDIR:?} base resolved
    [[ $mount == /* && $mount != / && $mount != */ ]] || fail 'Invalid NAS mount path'
    [[ $subdir =~ ^[A-Za-z0-9_-]+(/[A-Za-z0-9_-]+)*$ ]] || fail 'Invalid NAS subdirectory'
    mountpoint -q -- "$mount" || fail 'Smoke NAS mount is not mounted'
    base="$mount/$subdir"
    resolved=$(cd -P -- "$base" 2>/dev/null && pwd -P) || fail 'Smoke NAS subdirectory is missing or inaccessible'
    [[ $resolved == "$base" ]] || fail 'Smoke NAS path must not contain symlink or noncanonical components'
    printf '%s/sailbandwidth-smoke-%s\n' "$base" "$SMOKE_KEY"
}

case ${1:-} in
    build)
        output=${2:?}
        staging=$(mktemp -d)
        trap 'rm -rf -- "$staging"' EXIT
        ubuntu_packages cmake make g++ openmpi-bin libopenmpi-dev
        sdk_env
        export PATH="/usr/bin:$PATH"
        export LDFLAGS="${LDFLAGS:+${LDFLAGS} }-Xlinker --allow-shlib-undefined"
        mkdir -p "$staging/comm_tools/"{single_process,multi_process}
        bash build.sh --rebuild --cpu_num 8
        install -m 755 build/sailbandwidth "$staging/comm_tools/single_process/sailbandwidth"
        bash build.sh --rebuild --use_mpi --cpu_num 8
        install -m 755 build/sailbandwidth "$staging/comm_tools/multi_process/sailbandwidth"
        tar -czf "$output/build.tar.gz" -C "$staging" comm_tools
        mpi_version > "$output/openmpi.version"
        ;;
    nas-run) nas_run ;;
    setup)
        root=$(nas_run)
        group=${SMOKE_GROUP:?}
        rank=${NODE_RANK:-${RANK:-0}}
        python3 -S -B "$scripts/smoke_ci.py" record-nas-probe "$root" --group="$group" --rank="$rank"
        # Keep dependency/worker diagnostics off public build and result artifacts.
        (
            sdk_env
            ulimit -l unlimited
            if [[ ${SMOKE_NODES:?} == 2 ]]; then
                ubuntu_packages openmpi-bin
                [[ $(mpi_version) == "$(<"$root/openmpi.version")" ]] || fail 'Open MPI package versions differ from the CPU build'
                test -d /dev/infiniband
            fi
            ppu-smi -L
        ) > "$root/logs/$group-$rank.log" 2>&1
        ;;
    run)
        root=$(nas_run)
        sdk_env
        export PATH="/usr/bin:$PATH"
        ulimit -l unlimited
        args=()
        if [[ ${SMOKE_NODES:?} == 2 ]]; then
            test -s "${CLUSTER_HOSTS_FILE:?}"
            iface=$(awk '$2 == "00000000" && $1 != "lo" {print $1; exit}' /proc/net/route)
            [[ -n $iface ]] || fail 'Cannot identify the default-route interface'
            export HWLOC_COMPONENTS=-gl
            args=(--hostfile "$CLUSTER_HOSTS_FILE" --mpi-interface "$iface")
        fi
        python3 -S -B "$scripts/run_cases.py" \
            --group "$root/selection/${SMOKE_GROUP:?}" \
            --cache "$root/cache/${SMOKE_BUILD_SHA:?}" --identity "$root/build.json" \
            --output "$root/results/$SMOKE_GROUP" --work-root "$root/scratch" "${args[@]}"
        ;;
    *) fail 'Usage: smoke_ci.sh build OUTPUT | nas-run | setup | run' ;;
esac
