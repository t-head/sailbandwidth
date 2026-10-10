#!/bin/bash
set -ex
pip install filelock==3.20.0
export NCCL_DEBUG=INFO
source ./PPU_SDK/envsetup.sh
echo $TEST_ROOT
echo $LD_LIBRARY_PATH
echo $PATH

cd $TEST_ROOT
echo "######################### $(date +%Y%m%d%H%M%S) sailbandwidth test ##########################"
echo $PWD
echo "pwd has:"
ls

./sailbandwidth/build/sailbandwidth -t device_to_device_memcpy_read_ce -i 1
./sailbandwidth/build/sailbandwidth -t device_to_device_memcpy_write_ce -i 1
./sailbandwidth/build/sailbandwidth -t device_to_device_bidirectional_memcpy_read_ce -i 1
./sailbandwidth/build/sailbandwidth -t device_to_device_bidirectional_memcpy_write_ce -i 1
./sailbandwidth/build/sailbandwidth -t device_to_device_memcpy_read_sm -i 1
./sailbandwidth/build/sailbandwidth -t device_to_device_latency_sm -i 1
./sailbandwidth/build/sailbandwidth -t device_to_device_memcpy_write_sm -i 1
./sailbandwidth/build/sailbandwidth -t device_to_device_bidirectional_memcpy_read_sm -i 1
./sailbandwidth/build/sailbandwidth -t device_to_device_bidirectional_memcpy_write_sm -i 1
./sailbandwidth/build/sailbandwidth -t device_to_host_bidirectional_memcpy_sm -i 1
./sailbandwidth/build/sailbandwidth -t host_to_device_bidirectional_memcpy_sm -i 1
./sailbandwidth/build/sailbandwidth -t all_to_host_bidirectional_memcpy_sm -i 1
./sailbandwidth/build/sailbandwidth -t host_to_all_bidirectional_memcpy_sm -i 1
./sailbandwidth/build/sailbandwidth -t host_to_device_memcpy_ce -i 1
./sailbandwidth/build/sailbandwidth -t device_to_host_memcpy_ce -i 1
./sailbandwidth/build/sailbandwidth -t host_to_device_memcpy_sm -i 1
./sailbandwidth/build/sailbandwidth -t device_to_host_memcpy_sm -i 1
./sailbandwidth/build/sailbandwidth -t host_device_latency_sm -i 1

echo "######################### $(date +%Y%m%d%H%M%S) sailbandwidth test finish ##########################"
