#!/bin/bash
<<<<<<< HEAD
sudo /proj/demeter-PG0/prthaker/cornflakes/mlx5-netperf/build/mlx5-netperf \
=======
sudo /proj/demeter-PG0/prthaker/mlx5-netperf/build/mlx5-netperf \
>>>>>>> c0155e1a126326ac35db370747e511cf7b031f0c
     --mode=SERVER \
     --pci_addr=0000:41:00.0 \
     --server_mac=$SERVER_MAC \
     --server_ip=$SERVER_IP \
     --client_mac=$CLIENT_MAC \
     --client_ip=$CLIENT_IP \
     --array_size=32768000 \
     --segment_size=512 \
     --num_segments=2 \
<<<<<<< HEAD
     --num_cores=1 \
=======
     --num_cores=8 \
>>>>>>> c0155e1a126326ac35db370747e511cf7b031f0c
#     --with_copy
