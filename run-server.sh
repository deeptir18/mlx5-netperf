#!/bin/bash
sudo \
     /proj/demeter-PG0/prthaker/mlx5-netperf/build/mlx5-netperf \
     --mode=SERVER \
     --pci_addr=0000:41:00.0 \
     --server_mac=1c:34:da:41:cb:1c \
     --server_ip=128.110.218.246 \
     --client_mac=1c:34:da:41:c7:34 \
     --client_ip=128.110.219.8 \
     --array_size=32768000 \
     --segment_size=512 \
     --num_segments=2 \
#     --with_copy
