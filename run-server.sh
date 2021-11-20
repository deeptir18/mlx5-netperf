#!/bin/bash
sudo nice -n -19 taskset -c 1 \
     /proj/demeter-PG0/prthaker/cornflakes//mlx5-netperf/build/mlx5-netperf \
     --mode=SERVER \
     --pci_addr=0000:41:00.0 \
     --server_mac=1c:34:da:41:c6:fc \
     --server_ip=128.110.218.251 \
     --client_mac=1c:34:da:41:c8:04
     --client_ip=128.110.218.243 \
     --array_size=3276800000 \
     --segment_size=8192 \
     --num_segments=1 \
