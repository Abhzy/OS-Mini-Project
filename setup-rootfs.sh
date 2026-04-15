#!/bin/bash
set -e

echo "Setting up Alpine rootfs..."
mkdir -p rootfs-base
wget https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86_64/alpine-minirootfs-3.20.3-x86_64.tar.gz
tar -xzf alpine-minirootfs-3.20.3-x86_64.tar.gz -C rootfs-base
rm alpine-minirootfs-3.20.3-x86_64.tar.gz

echo "Setup complete. You can now use ./rootfs-base as the template."
