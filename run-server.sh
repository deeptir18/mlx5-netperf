#!/bin/bash
sudo nice -n -19 taskset -c 1 \
     /proj/demeter-PG0/prthaker/cornflakes/mlx5-netperf/build/mlx5-netperf \
     --mode=SERVER \
     --pci_addr=0000:41:00.0 \
     --server_mac=1c:34:da:41:cf:c4 \
     --server_ip=192.168.1.2 \
     --client_mac=1c:34:da:41:cb:1c \
     --client_ip=192.168.1.1 \
     --array_size=$((3276800000/4)) \
     --segment_size=512 \
     --num_segments=2 \
#     --zero_copy
