#!/bin/sh
# Build the container images: tiggo-build (ARM cross toolchain, gcc 4.8 / glibc 2.17 like the HU)
# and tiggo-pc (PC build, asset baking). usage: container/build.sh
set -e
cd "$(dirname "$0")"
podman build -t localhost/tiggo-build -f Containerfile .
podman build -t localhost/tiggo-pc -f Containerfile.pc .
