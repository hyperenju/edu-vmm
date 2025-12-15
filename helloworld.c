#define _GNU_SOURCE

#include <fcntl.h>
#include <linux/kvm.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

static void die(const char *msg) {
    perror(msg);
    exit(1);
}

static void check_ioctl(int ret, const char *msg) {
	    if (ret < 0)
	        die(msg);
}

int main(void) {
		    const size_t mem_size = 16u * 1024u * 1024u;
		    const uint16_t com1_port = 0x3F8;

    int dev_fd = open("/dev/kvm", O_RDWR | O_CLOEXEC);
    if (dev_fd < 0)
        die("open(/dev/kvm)");

    int api_ver = ioctl(dev_fd, KVM_GET_API_VERSION, 0);
    if (api_ver < 0)
        die("ioctl(KVM_GET_API_VERSION)");
    if (api_ver != KVM_API_VERSION) {
        fprintf(stderr, "KVM API version mismatch: got=%d expected=%d\n",
                api_ver, KVM_API_VERSION);
        return 1;
    }

    int vm_fd = ioctl(dev_fd, KVM_CREATE_VM, 0); // KVM_X86_DEFAULT_VM = 0 (default)
    if (vm_fd < 0)
        die("ioctl(KVM_CREATE_VM)");

    void *mem = mmap(NULL, mem_size, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (mem == MAP_FAILED)
        die("mmap(guest mem)");

    // このメモリ領域をmlockしなければならないという制約は、一般にはなさそう。
    struct kvm_userspace_memory_region region = {
        .slot = 0, // slot ってなんだろ
        // This ioctl allows the user to create, modify or delete a guest physical memory slot. Bits 0-15 of “slot” specify the slot id and this value should be less than the maximum number of user memory slots supported per VM. The maximum allowed slots can be queried using KVM_CAP_NR_MEMSLOTS. Slots may not overlap in guest physical address space.
        .flags = 0,
        .guest_phys_addr = 0,
        .memory_size = mem_size,
        .userspace_addr = (uintptr_t)mem,
    };
    
    check_ioctl(ioctl(vm_fd, KVM_SET_USER_MEMORY_REGION, &region),
                "ioctl(KVM_SET_USER_MEMORY_REGION)");

    const uint8_t guest_code[] = {
        0xBA,
        (uint8_t)(com1_port & 0xFF),
        (uint8_t)(com1_port >> 8),
        0xB0,
        'H',
        0xEE,
        0xB0,
        'e',
        0xEE,
        0xB0,
        'l',
        0xEE,
        0xB0,
        'l',
        0xEE,
        0xB0,
        'o',
        0xEE,
        0xB0,
        ',',
        0xEE,
        0xB0,
        ' ',
        0xEE,
        0xB0,
        'w',
        0xEE,
        0xB0,
        'o',
        0xEE,
        0xB0,
        'r',
        0xEE,
        0xB0,
        'l',
        0xEE,
        0xB0,
        'd',
        0xEE,
        0xB0,
        '!',
        0xEE,
        0xB0,
        '\n',
        0xEE,
        0xF4,
    };
    memcpy(mem, guest_code, sizeof(guest_code));

    int vcpu_fd = ioctl(vm_fd, KVM_CREATE_VCPU, 0);
    if (vcpu_fd < 0)
        die("ioctl(KVM_CREATE_VCPU)");

    int vcpu_mmap_size = ioctl(dev_fd, KVM_GET_VCPU_MMAP_SIZE, 0);
    if (vcpu_mmap_size < 0)
        die("ioctl(KVM_GET_VCPU_MMAP_SIZE)");
    if ((size_t)vcpu_mmap_size < sizeof(struct kvm_run)) {
        fprintf(stderr, "KVM_RUN mmap size too small: %d\n", vcpu_mmap_size);
        return 1;
    }

    // Application code obtains a pointer to the kvm_run structure by mmap()ing a vcpu fd. From that point, application code can control execution by changing fields in kvm_run prior to calling the KVM_RUN ioctl, and obtain information about the reason KVM_RUN returned by looking up structure members.
    struct kvm_run *run = mmap(NULL, (size_t)vcpu_mmap_size,
                               PROT_READ | PROT_WRITE, MAP_SHARED, vcpu_fd, 0);
    if (run == MAP_FAILED)
        die("mmap(kvm_run)");

	    struct kvm_sregs sregs;
	    check_ioctl(ioctl(vcpu_fd, KVM_GET_SREGS, &sregs), "ioctl(KVM_GET_SREGS)");
	    sregs.cs.base = 0;
	    sregs.cs.selector = 0;
	    sregs.ds.base = sregs.es.base = sregs.fs.base = sregs.gs.base =
	        sregs.ss.base = 0;
	    sregs.ds.selector = sregs.es.selector = sregs.fs.selector =
	        sregs.gs.selector = sregs.ss.selector = 0;
	    sregs.cr0 = 0x10; // ET=1, PE=0 => real mode
	    sregs.efer = 0;
	    check_ioctl(ioctl(vcpu_fd, KVM_SET_SREGS, &sregs), "ioctl(KVM_SET_SREGS)");

    struct kvm_regs regs;
    memset(&regs, 0, sizeof(regs));
    regs.rip = 0x0000;
    regs.rflags = 0x2;
    regs.rsp = 0x200000;
    check_ioctl(ioctl(vcpu_fd, KVM_SET_REGS, &regs), "ioctl(KVM_SET_REGS)");

    for (;;) {
        check_ioctl(ioctl(vcpu_fd, KVM_RUN, 0), "ioctl(KVM_RUN)");

        switch (run->exit_reason) {
        case KVM_EXIT_HLT:
            goto out;

        case KVM_EXIT_IO: {
            // fprintf(stderr, "KVM_EXIT_IO\n");
            if (run->io.direction != KVM_EXIT_IO_OUT) {
                fprintf(stderr, "Unhandled IO direction=%u\n",
                        run->io.direction);
                return 1;
            }
            if (run->io.port != com1_port || run->io.size != 1) {
                fprintf(stderr, "Unhandled IO port=0x%x size=%u count=%u\n",
                        run->io.port, run->io.size, run->io.count);
                return 1;
            }

            uint8_t *data = (uint8_t *)run + run->io.data_offset;
            for (uint32_t i = 0; i < run->io.count; i++)
                putchar((int)data[i]);
            fflush(stdout);
            break;
        }

        case KVM_EXIT_FAIL_ENTRY:
            fprintf(
                stderr,
                "KVM_EXIT_FAIL_ENTRY: hardware_entry_failure_reason=0x%llx\n",
                (unsigned long long)
                    run->fail_entry.hardware_entry_failure_reason);
            return 1;

        case KVM_EXIT_INTERNAL_ERROR:
            fprintf(stderr, "KVM_EXIT_INTERNAL_ERROR: suberror=0x%x\n",
                    run->internal.suberror);
            return 1;

        default:
            fprintf(stderr, "Unhandled KVM exit reason: %u\n",
                    run->exit_reason);
            return 1;
        }
    }

out:
    munmap(run, (size_t)vcpu_mmap_size);
    munmap(mem, mem_size);
    close(vcpu_fd);
    close(vm_fd);
    close(dev_fd);
    return 0;
}
