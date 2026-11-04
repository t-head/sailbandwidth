#!/bin/bash
set -e
# Validate required environment variables.
if [ -z "${source_home}" ]; then
    echo "Error: source_home environment variable is not set"
    exit 1
fi
if [ -z "${build_output}" ]; then
    echo "Error: build_output environment variable is not set"
    exit 1
fi

# Matrix builds override pkg_name/sdk_package so every distribution keeps a distinct
# tarball name and installs the SDK build that matches its own libc/toolchain.
pkg_name=${pkg_name:-sailbandwidth_hggcrt13_afa3}

# The PPU SDK image ships neither cmake/make nor an MPI toolchain, while build.sh is only a
# CMake wrapper (cmake_minimum_required 3.20) and the multi_process variant builds with
# -DMULTINODE=1, which needs mpicxx. Install whatever is missing first; the apt mirror
# rewrite mirrors pr-test-ppu.yml because the image bakes in an internal-only apt proxy.
install_build_dependencies() {
    if command -v apt-get >/dev/null 2>&1; then
        local missing=""
        for pkg in cmake make libopenmpi-dev openmpi-bin; do
            if ! dpkg -s "$pkg" >/dev/null 2>&1; then
                echo "$pkg is missing, will install"
                missing="$missing $pkg"
            fi
        done
        if [ -z "$missing" ]; then
            echo "build dependencies already installed"
            return 0
        fi
        if grep -Rq "art.eng.t-head.cn" /etc/apt/sources.list /etc/apt/sources.list.d/ 2>/dev/null; then
            local codename
            codename=$(. /etc/os-release && echo "$VERSION_CODENAME")
            echo "Replacing art.eng.t-head.cn apt sources with mirrors.aliyun.com"
            rm -f /etc/apt/sources.list.d/*.sources
            printf '%s\n' \
                "deb http://mirrors.aliyun.com/ubuntu/ ${codename} main restricted universe multiverse" \
                "deb http://mirrors.aliyun.com/ubuntu/ ${codename}-updates main restricted universe multiverse" \
                "deb http://mirrors.aliyun.com/ubuntu/ ${codename}-backports main restricted universe multiverse" \
                "deb http://mirrors.aliyun.com/ubuntu/ ${codename}-security main restricted universe multiverse" \
                > /etc/apt/sources.list
        else
            if [ -f /etc/apt/sources.list ]; then
                sed -i 's|archive.ubuntu.com|mirrors.aliyun.com|g' /etc/apt/sources.list
                sed -i 's|security.ubuntu.com|mirrors.aliyun.com|g' /etc/apt/sources.list
            fi
            for f in /etc/apt/sources.list.d/*.sources; do
                [ -f "$f" ] || continue
                sed -i 's|archive.ubuntu.com|mirrors.aliyun.com|g' "$f"
                sed -i 's|security.ubuntu.com|mirrors.aliyun.com|g' "$f"
            done
        fi
        apt-get update
        # shellcheck disable=SC2086 # intentional word splitting on the package list
        apt-get install -y $missing
    elif command -v yum >/dev/null 2>&1; then
        yum install -y cmake make openmpi-devel
    else
        echo "Error: neither apt-get nor yum is available to install build dependencies"
        exit 1
    fi
}

echo "######################### $(date +%Y%m%d%H%M%S) Install build dependencies ##########################"
install_build_dependencies

echo "######################### $(date +%Y%m%d%H%M%S) Source PPU_SDK ##########################"

source /usr/local/PPU_SDK/envsetup.sh

mkdir -p ${build_output}/comm_internal/comm_tools/single_process
mkdir -p ${build_output}/comm_release/comm_tools/single_process
mkdir -p ${build_output}/comm_internal/comm_tools/multi_process
mkdir -p ${build_output}/comm_release/comm_tools/multi_process
mkdir -p ${build_output}/comm_internal/comm_tools/licenses
mkdir -p ${build_output}/comm_release/comm_tools/licenses

echo "BUILD single_process"
cd ${source_home}/sailbandwidth
bash build.sh --rebuild

cp ${source_home}/sailbandwidth/build/sailbandwidth ${build_output}/comm_internal/comm_tools/single_process
cp ${source_home}/sailbandwidth/build/sailbandwidth ${build_output}/comm_release/comm_tools/single_process

echo "BUILD multi_process"
cd ${source_home}/sailbandwidth
bash build.sh --rebuild --use_mpi

cp ${source_home}/sailbandwidth/build/sailbandwidth ${build_output}/comm_internal/comm_tools/multi_process
cp ${source_home}/sailbandwidth/build/sailbandwidth ${build_output}/comm_release/comm_tools/multi_process

cp ${source_home}/sailbandwidth/LICENSE ${build_output}/comm_internal/comm_tools/licenses/nvbandwidth-LICENSE
cp ${source_home}/sailbandwidth/LICENSE ${build_output}/comm_release/comm_tools/licenses/nvbandwidth-LICENSE
cp ${source_home}/sailbandwidth/Licenses.txt ${build_output}/comm_internal/comm_tools/licenses/nvbandwidth-Licenses.txt
cp ${source_home}/sailbandwidth/Licenses.txt ${build_output}/comm_release/comm_tools/licenses/nvbandwidth-Licenses.txt

cd ${build_output}/comm_internal/ 
tar -zcvf ${build_output}/comm_internal/"${pkg_name}.tar.gz" *
ls | grep -v "${pkg_name}.tar.gz" | xargs rm -rf
cd ${build_output}/comm_release/ 
tar -zcvf ${build_output}/comm_release/"${pkg_name}.tar.gz" *
ls | grep -v "${pkg_name}.tar.gz" | xargs rm -rf
echo "BUILD sailbandwidth FINISHED"