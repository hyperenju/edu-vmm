CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra -std=c11

.PHONY: all run clean

all: helloworld boot-kernel query_vm_types

helloworld: helloworld.c
	$(CC) $(CFLAGS) -o $@ $<

boot-kernel: boot-kernel.c
	$(CC) $(CFLAGS) -pthread -o $@ $<

query_vm_types: query_vm_types.c
	$(CC) $(CFLAGS) -o $@ $<

run: helloworld
	./helloworld

clean:
	rm -f helloworld boot-kernel query_vm_types
