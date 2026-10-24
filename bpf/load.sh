#!/bin/sh
# Register bdpcc, load BBR, and let unprivileged sockets select all four algorithms.
# Run as root (sudo sh load.sh) from the bpf/ directory.
set -e
cd "$(dirname "$0")"

modprobe tcp_bbr
bpftool struct_ops register bdpcc.bpf.o
sysctl -w net.ipv4.tcp_allowed_congestion_control="reno cubic bbr bdpcc"

sysctl net.ipv4.tcp_available_congestion_control
bpftool struct_ops list
