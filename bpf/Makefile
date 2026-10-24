# Build the bdpcc eBPF congestion control program.
#   make          -> bdpcc.bpf.o
#   sudo make load / sudo make unload
ARCH := $(shell uname -m | sed -e 's/x86_64/x86/' -e 's/aarch64/arm64/')
BPFTOOL ?= bpftool

all: bdpcc.bpf.o

vmlinux.h:
	$(BPFTOOL) btf dump file /sys/kernel/btf/vmlinux format c > $@

bdpcc.bpf.o: bdpcc.bpf.c vmlinux.h
	clang -O2 -g -target bpf -D__TARGET_ARCH_$(ARCH) -c $< -o $@

load: bdpcc.bpf.o
	sh ./load.sh

unload:
	$(BPFTOOL) struct_ops unregister name bdpcc

clean:
	rm -f bdpcc.bpf.o vmlinux.h

.PHONY: all load unload clean
