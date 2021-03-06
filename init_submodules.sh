#!/bin/sh

set -e

CORES=`getconf _NPROCESSORS_ONLN`

git submodule init
git submodule update --init -f --recursive

echo building RDMA-CORE
cd rdma-core
EXTRA_CMAKE_FLAGS=-DENABLE_STATIC=1 MAKEFLAGS=-j$CORES ./build.sh
cd ..
