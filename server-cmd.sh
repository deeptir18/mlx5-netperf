#!/bin/bash
sudo nice -n -19 taskset -c 1 /proj/demeter-PG0/prthaker/mlx5-netperf/build/mlx5-netperf \
     --mode=SERVER \
     --pci_addr=0000:41:00.0 \
     --server_mac=1c:34:da:41:c7:4c \
     --server_ip=128.110.218.241 \
     --client_mac=1c:34:da:41:ca:c4 \
     --client_ip=128.110.218.253 \
     --num_cores=2 \
     --array_size=32768000 \
     --segment_size=512 \
     --num_segments=2 \
     --busy_work_us=0
