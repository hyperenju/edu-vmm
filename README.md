# cvmm

Learning repository to understand the KVM API and the role/implementation of a VMM.

This project currently implements:
- Direct Linux kernel boot on KVM (x86_64)
- A virtio-blk backend via MMIO

Future work:
- Additional device emulation such as a virtio-net backend

## Contents
- `boot-kernel.c`: Minimal VMM that boots a Linux `bzImage`, sets up paging,
  wires up a virtio-blk MMIO device, and services its queue in a dedicated I/O
  thread.
- `helloworld.c`: Tiny KVM example that runs a guest in real mode and prints to
  the serial port (COM1).
- `query_vm_types.c`: Utility to query supported KVM VM types on the host.
- `Makefile`: Builds the `helloworld` example.

## Requirements
- Linux host with `/dev/kvm`
- `gcc` and standard development headers
- A Linux `bzImage`
- A rootfs image for virtio-blk (e.g., an ext4 disk image)

## Build

### helloworld
```
make
```

### boot-kernel
```
cc -O2 -Wall -Wextra -std=c11 -pthread -o boot-kernel boot-kernel.c
```

### query_vm_types
```
cc -O2 -Wall -Wextra -std=c11 -o query_vm_types query_vm_types.c
```

## Run

### Query VM types
```
./query_vm_types
```

### Run the hello-world guest
```
./helloworld
```

### Boot a Linux kernel with virtio-blk
```
./boot-kernel /path/to/bzImage /path/to/rootfs.ext4
```

Notes:
- If you omit the second argument, `boot-kernel.c` uses the hard-coded
  `ROOT_FS` path. You will likely want to pass an explicit rootfs path instead.
- The guest kernel must be built with virtio-mmio support (e.g.
  `CONFIG_VIRTIO_MMIO=y` and `CONFIG_VIRTIO_MMIO_CMDLINE_DEVICES=y`).
- The rootfs is exposed as `/dev/vda` and the kernel command line sets
  `root=/dev/vda`.

## References inside the code
- Virtio MMIO register layout and virtio-blk config layout are described in
  comments inside `boot-kernel.c`.

## License
TBD
