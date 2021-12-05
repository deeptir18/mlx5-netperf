#!/bin/bash
sudo nice -n -19 taskset -c 1 \
     /proj/demeter-PG0/prthaker/cornflakes//mlx5-netperf/build/mlx5-netperf \
     --mode=SERVER \
     --pci_addr=0000:41:00.0 \
     --server_mac=1c:34:da:41:c7:0c \
     --server_ip=128.110.218.245 \
     --client_mac=1c:34:da:41:d2:94 \
     --client_ip=128.110.218.249 \
     --array_size=163840000 \
     --segment_size=8192 \
     --num_segments=1 
