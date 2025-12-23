#define _GNU_SOURCE

#include <asm/bootparam.h>
#include <fcntl.h>
#include <linux/kvm.h>
#include <linux/virtio_blk.h>
#include <linux/virtio_config.h>
#include <linux/virtio_mmio.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define E820_TYPE_RAM 1
#define E820_TYPE_RESERVED 2

#define BOOT_PARAMS_ADDR 0x10000
#define CMDLINE_ADDR 0x20000
#define KERNEL_ADDR 0x100000
#define PML4_ADDR 0x1000
#define PDPT_ADDR 0x2000
#define PD_ADDR 0x3000

static void setup_paging(void *mem) {
        uint64_t *pml4 = (uint64_t *)((char *)mem + PML4_ADDR);
        uint64_t *pdpt = (uint64_t *)((char *)mem + PDPT_ADDR);
        uint64_t *pd0 = (uint64_t *)((char *)mem + 0x3000); // 0-1GB
        uint64_t *pd1 = (uint64_t *)((char *)mem + 0x4000); // 1-2GB

        memset(pml4, 0, 0x1000);
        memset(pdpt, 0, 0x1000);
        memset(pd0, 0, 0x1000);
        memset(pd1, 0, 0x1000);

        pml4[0] = 0x2000 | 3;
        pdpt[0] = 0x3000 | 3;
        pdpt[1] = 0x4000 | 3;

        // 2MBページで2GBまでマッピング
        for (int i = 0; i < 512; i++) {
                pd0[i] = (i * 0x200000ULL) | 0x83;
                pd1[i] = ((512ULL + i) * 0x200000ULL) | 0x83;
        }
}

struct virtio_blk_status {
        uint32_t status;
        uint32_t device_feature_sel;
        uint32_t driver_feature_sel;
        // TODO: split queue related fields to virtio_queue like structure
        uint32_t queue_sel;
        uint32_t queue_ready;
        uint32_t queue_size;
        uint32_t queue_desc_high;
        uint32_t queue_desc_low;
        uint32_t queue_avail_high;
        uint32_t queue_avail_low;
        uint32_t queue_used_high;
        uint32_t queue_used_low;
};

int main(int argc, char *argv[]) {
        struct virtio_blk_status blk_status = {0};
        struct virtio_blk_config blk_config = {0};
        blk_config.capacity = 2000; // 1MiB (in 512 bytes unit)

        if (argc != 2) {
                fprintf(stderr, "Usage: %s <bzImage>\n", argv[0]);
                return 1;
        }

        int kernel_fd = open(argv[1], O_RDONLY);
        if (kernel_fd < 0) {
                perror("open kernel");
                return 1;
        }

        struct stat st;
        fstat(kernel_fd, &st);

        void *kernel_data =
            mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, kernel_fd, 0);
        if (kernel_data == MAP_FAILED) {
                perror("mmap kernel");
                return 1;
        }

// /home/kohei/ghq/git.kernel.org/pub/scm/linux/kernel/git/bpf/bpf-next/arch/x86/boot/bzImage
// The first step in loading a Linux kernel should be to load the real-mode code
// (boot sector and setup code) and then examine the following header at offset
// 0x01f1
#define X86_REAL_MODE_HEADER_OFFSET 0x1f1
#define X86_BOOT_FLAG 0xAA55
#define X86_MAGIC_HDRS 0x53726448

        struct setup_header *hdr =
            (struct setup_header *)((char *)kernel_data +
                                    X86_REAL_MODE_HEADER_OFFSET);
        // 01FE/2 ALL boot_flag 0xAA55 magic number
        // 0202/4 2.00+ header Magic signature “HdrS” (0x53726448)
        if (hdr->boot_flag != X86_BOOT_FLAG || hdr->header != X86_MAGIC_HDRS) {
                fprintf(stderr, "Invalid kernel\n");
                return 1;
        }

        // Contains the boot protocol version, in (major << 8) + minor format,
        // e.g. 0x0204 for version 2.04, and 0x0a11 for a hypothetical
        // version 10.17.
        printf("Boot protocol version: %d.%d\n", hdr->version >> 8,
               hdr->version & 0xff);

        int kvm_fd = open("/dev/kvm", O_RDWR);
        int vm_fd = ioctl(kvm_fd, KVM_CREATE_VM, 0);

        // PIC, IOAPIC, Local APIC とは？
        // PIC: PIC 8259?. レガシーIRQ?
        // IOAPIC: GSI (global system inerrupt). I/O Advanced Programmable
        // Interrupt Controller Local APIC: per vCPU APIC IRQチップを作成（PIC,
        // IOAPIC, Local APIC）
        if (ioctl(vm_fd, KVM_CREATE_IRQCHIP, 0) < 0) {
                perror("KVM_CREATE_IRQCHIP");
                return 1;
        }

        // PIT（タイマー）を作成
        // Programmable Interrupt timer
        struct kvm_pit_config pit_config = {0};
        pit_config.flags = KVM_PIT_SPEAKER_DUMMY;
        if (ioctl(vm_fd, KVM_CREATE_PIT2, &pit_config) < 0) {
                perror("KVM_CREATE_PIT2");
                return 1;
        }

        // 1 GiB guest memory
        const size_t mem_size = 1024 * 1024 * 1024;
        void *mem = mmap(NULL, mem_size, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (mem == MAP_FAILED) {
                perror("mmap guest memory");
                return 1;
        }

        struct kvm_userspace_memory_region region = {
            .slot = 0,
            .guest_phys_addr = 0,
            .memory_size = mem_size,
            .userspace_addr = (uintptr_t)mem,
        };
        if (ioctl(vm_fd, KVM_SET_USER_MEMORY_REGION, &region)) {
                perror("ioctl(KVM_SET_USER_MEMORY_REGION) failed");
                return 1;
        }

        struct boot_params *bp =
            (struct boot_params *)((char *)mem + BOOT_PARAMS_ADDR);
        // 0 で初期化
        memset(bp, 0, sizeof(*bp));
        // kernel image header をコピー
        memcpy(&bp->hdr, hdr, sizeof(struct setup_header));

        // boot loader によって書き込まれるべき値
        bp->hdr.type_of_loader = 0xff; // boot loader ID
        bp->hdr.loadflags |=
            (1 << 0); // bzImage の場合には bit0
                      // をセットするらしい。CAN_USE_HEAP (bit7) は不要？

        /* x86_64 で virtio over MMIO のデバイスの場所を kernel に教える方法
         * 3. Kernel module (or command line) parameter. Can be used more than
         *once - one device will be created for each one. Syntax:
         *
         *		[virtio_mmio.]device=<size>@<baseaddr>:<irq>[:<id>]
         *    where:
         *		<size>     := size (can use standard suffixes like K, M
         *or G) <baseaddr> := physical base address <irq>      := interrupt
         *number (as passed to request_irq()) <id>       := (optional) platform
         *device id eg.:
         *		virtio_mmio.device=0x100@0x100b0000:48 \
         *				virtio_mmio.device=1K@0x1001e000:74
         */

        // virtio-mmio の MMIO register layout
        // https://docs.oasis-open.org/virtio/virtio/v1.0/cs04/virtio-v1.0-cs04.html#x1-1110002
        // virtio-blk の configuration layout: struct virtio_blk_config が 0x100
        // start で存在する？ sizeof(struct virtio_blk_config): 96
        // guest kernel でこれらが必要
        // CONFIG_VIRTIO_MMIO=y
        // CONFIG_VIRTIO_MMIO_CMDLINE_DEVICES=y

        // i8042 は無効化する。firecracker を参考にした。
        const char *cmdline =
            "console=ttyS0 audit=0 selinux=0 root=/dev/vda nokaslr ro "
            "i8042.noaux i8042.nomux i8042.dumbkbd "
            "virtio_mmio.device=0x1000@0x80000000:5"; // memory
                                                       // と被らないように適当に配置。サイズも余分に確保. irq を最初適当に50にしてたら、多分 request irq が失敗した。
        strcpy((char *)mem + CMDLINE_ADDR, cmdline);
        bp->hdr.cmd_line_ptr = CMDLINE_ADDR;

        // e820 とは？table とは？
        bp->e820_entries = 4;
        bp->e820_table[0].addr = 0x0;
        bp->e820_table[0].size = 0x1000;
        bp->e820_table[0].type = E820_TYPE_RESERVED;
        bp->e820_table[1].addr = 0x1000;
        bp->e820_table[1].size = 0x9f000;
        bp->e820_table[1].type = E820_TYPE_RAM;
        bp->e820_table[2].addr = 0xa0000;
        bp->e820_table[2].size = 0x60000;
        bp->e820_table[2].type = E820_TYPE_RESERVED;
        bp->e820_table[3].addr = 0x100000;
        bp->e820_table[3].size = mem_size - 0x100000;
        bp->e820_table[3].type = E820_TYPE_RAM; // 0x40100000 まで

        uint32_t setup_sects = hdr->setup_sects ? hdr->setup_sects : 4;
        uint32_t kernel_offset = (setup_sects + 1) * 512;
        memcpy((char *)mem + KERNEL_ADDR, (char *)kernel_data + kernel_offset,
               st.st_size - kernel_offset);

        setup_paging(mem);

        int vcpu_fd = ioctl(vm_fd, KVM_CREATE_VCPU, 0);
        if (vcpu_fd < 0) {
                perror("ioctl: KVM_CREATE_VCPU failed");
                return 1;
        }

        // CPUID をセット
#define KVM_MAX_CPUID_ENTRIES 256
        // TODO: free memory in out/error path
        struct kvm_cpuid2 *cpuid_data = calloc(
            1, sizeof(struct kvm_cpuid2) +
                   KVM_MAX_CPUID_ENTRIES * sizeof(struct kvm_cpuid_entry2));
        if (!cpuid_data) {
                perror("failed to malloc for cpuid_data");
                return 1;
        }

        cpuid_data->nent = KVM_MAX_CPUID_ENTRIES;
        if (ioctl(kvm_fd, KVM_GET_SUPPORTED_CPUID, cpuid_data) < 0) {
                perror("KVM_GET_SUPPORTED_CPUID");
                return 1;
        }
        if (ioctl(vcpu_fd, KVM_SET_CPUID2, cpuid_data) < 0) {
                perror("KVM_SET_CPUID2");
                return 1;
        }

        int mmap_size = ioctl(kvm_fd, KVM_GET_VCPU_MMAP_SIZE, 0);
        if (mmap_size < 0) {
                perror("ioctl(KVM_GET_VCPU_MMAP_SIZE) failed");
                return 1;
        }
        struct kvm_run *run = mmap(NULL, mmap_size, PROT_READ | PROT_WRITE,
                                   MAP_SHARED, vcpu_fd, 0);
        if (run == MAP_FAILED) {
                perror("mmap kvm_run");
                return 1;
        }

        struct kvm_sregs sregs;
        ioctl(vcpu_fd, KVM_GET_SREGS, &sregs);

        sregs.cs.base = 0;
        sregs.cs.limit = 0xffffffff;
        sregs.cs.selector = 0x10;
        sregs.cs.type = 11;
        sregs.cs.present = 1;
        sregs.cs.dpl = 0;
        sregs.cs.db = 0;
        sregs.cs.s = 1;
        sregs.cs.l = 1;
        sregs.cs.g = 1;

        sregs.ds.base = 0;
        sregs.ds.limit = 0xffffffff;
        sregs.ds.selector = 0x18;
        sregs.ds.type = 3;
        sregs.ds.present = 1;
        sregs.ds.dpl = 0;
        sregs.ds.db = 1;
        sregs.ds.s = 1;
        sregs.ds.l = 0;
        sregs.ds.g = 1;
        sregs.es = sregs.ss = sregs.fs = sregs.gs = sregs.ds;

        sregs.cr0 = 0x80050033;
        sregs.cr3 = PML4_ADDR;
        sregs.cr4 = 0x668;
        sregs.efer = 0x500;

        if (ioctl(vcpu_fd, KVM_SET_SREGS, &sregs)) {
                perror("ioctl(KVM_SET_SREGS) failed");
                return 1;
        }

        struct kvm_regs regs = {0};
        // In 64-bit boot protocol, the kernel is started by jumping to the
        // 64-bit kernel entry point, which is the start address of loaded
        // 64-bit kernel plus 0x200.
        regs.rip = KERNEL_ADDR + 0x200;
        regs.rsi = BOOT_PARAMS_ADDR;
        regs.rsp = 0x80000;
        regs.rflags = 0x2;
        if (ioctl(vcpu_fd, KVM_SET_REGS, &regs)) {
                perror("ioctl(KVM_SET_REGS) failed");
                return 1;
        }

        printf("Starting kernel at RIP=0x%llx, RSI=0x%llx\n", regs.rip,
               regs.rsi);

        for (;;) {
                if (ioctl(vcpu_fd, KVM_RUN, 0)) {
                        perror("ioctl(KVM_RUN) failed");
                        return 1;
                }

                switch (run->exit_reason) {
                case KVM_EXIT_HLT:
                        fprintf(stderr, "\nKVM_EXIT_HLT\n");
                        return 0;
                case KVM_EXIT_IO:
                        if (run->io.port >= 0x3f8 && run->io.port <= 0x3ff) {
                                uint8_t *data =
                                    (uint8_t *)run + run->io.data_offset;
                                if (run->io.direction == KVM_EXIT_IO_OUT) {
                                        if (run->io.port == 0x3f8) {
                                                for (uint32_t i = 0;
                                                     i < run->io.count; i++)
                                                        putchar(data[i]);
                                                fflush(stdout);
                                        }
                                } else {
                                        for (uint32_t i = 0; i < run->io.count;
                                             i++) {
                                                if (run->io.port == 0x3fd)
                                                        data[i] = 0x60;
                                                else
                                                        data[i] = 0;
                                        }
                                }
                        } else {
                                // // 未処理のIOをログ
                                // fprintf(stderr, "[IO %s port=0x%x
                                // size=%d]\n",
                                //         run->io.direction ? "OUT" : "IN",
                                //         run->io.port, run->io.size);
                        }
                        break;
                // TODO: Don't hardcode MMIO address
                // "virtio_mmio.device=0x1000@0x80000000:50";
                case KVM_EXIT_MMIO:
                        if (run->mmio.is_write)
                            fprintf(stderr,
                                    "[MMIO %s at 0x%x with size = %d, data=(0x%x, "
                                    "0x%x, 0x%x, 0x%x)]\n",
                                    run->mmio.is_write ? "write" : "read",
                                    (uint32_t)run->mmio.phys_addr, run->mmio.len,
                                    run->mmio.data[0], run->mmio.data[1],
                                    run->mmio.data[2], run->mmio.data[3]);
                        if (!run->mmio.is_write)
                            fprintf(stderr,
                                    "[MMIO %s at 0x%x with size = %d]\n",
                                    run->mmio.is_write ? "write" : "read",
                                    (uint32_t)run->mmio.phys_addr, run->mmio.len);
                        if ((uint32_t)run->mmio.phys_addr < 0x80000000 ||
                            0x80000000 + 0x1000 <=
                                (uint32_t)run->mmio.phys_addr) {
                                break;
                        }
                        // virtio-blk
                        uint32_t offset =
                            (uint32_t)run->mmio.phys_addr - 0x80000000;
                        switch (offset) {
                        case VIRTIO_MMIO_MAGIC_VALUE:
                                if (run->mmio.is_write || run->mmio.len != 4)
                                        break;
                                memcpy(run->mmio.data, &"virt", 4);
                                break;
                        case VIRTIO_MMIO_VERSION:
                                if (run->mmio.is_write || run->mmio.len != 4)
                                        break;
                                *(uint32_t *)run->mmio.data = 0x2;
                                break;
                        case VIRTIO_MMIO_DEVICE_ID:
                                if (run->mmio.is_write || run->mmio.len != 4)
                                        break;
                                *(uint32_t *)run->mmio.data =
                                    0x2; // virtio-blk: 0x2
                                break;
                        case VIRTIO_MMIO_VENDOR_ID:
                                if (run->mmio.is_write || run->mmio.len != 4)
                                        break;
                                memset(run->mmio.data, 0, 4); // dummy
                                break;
                        case VIRTIO_MMIO_DEVICE_FEATURES_SEL:
                                if (!run->mmio.is_write)
                                        break;
                                blk_status.device_feature_sel =
                                    *(uint32_t *)run->mmio.data;
                                fprintf(stderr,
                                        "[VIRTIO: feature(device): sel = %d]\n",
                                        blk_status.device_feature_sel);
                                break;
                        case VIRTIO_MMIO_DEVICE_FEATURES:
                                if (run->mmio.is_write)
                                        break;
                                if (blk_status.device_feature_sel == 0) {
                                        *(uint32_t *)run->mmio.data =
                                            1 << VIRTIO_BLK_F_RO;
                                } else if (blk_status.device_feature_sel == 1) {
                                        *(uint32_t *)run->mmio.data =
                                            1 << (VIRTIO_F_VERSION_1 % 32);
                                } else {
                                        *(uint32_t *)run->mmio.data = 0;
                                }
                                break;
                        case VIRTIO_MMIO_DRIVER_FEATURES_SEL:
                                if (!run->mmio.is_write)
                                        break;
                                blk_status.driver_feature_sel =
                                    *(uint32_t *)run->mmio.data;
                                fprintf(stderr,
                                        "[VIRTIO: feature(driver): sel = %d]\n",
                                        blk_status.driver_feature_sel);
                                break;
                        case VIRTIO_MMIO_DRIVER_FEATURES:
                                if (!run->mmio.is_write)
                                        break;
                                // Now for simplicity, don't torelate fallback.
                                // We strongly assume VIRTIO_BLK_F_RO and
                                // VIRTIO_F_VERSION_1 are accpeted by drivers
                                if (blk_status.driver_feature_sel == 0) {
                                        if (*(uint32_t *)run->mmio.data !=
                                            1 << VIRTIO_BLK_F_RO)
                                                fprintf(stderr,
                                                        "[VIRTIO: blk: "
                                                        "VIRTIO_BLK_F_RO is "
                                                        "not accepted.]\n");
                                } else if (blk_status.driver_feature_sel == 1) {
                                        if (*(uint32_t *)run->mmio.data !=
                                            1 << (VIRTIO_F_VERSION_1 % 32))
                                                fprintf(stderr,
                                                        "[VIRTIO: blk: "
                                                        "VIRTIO_F_VERSION_1 is "
                                                        "not accepted.]\n");
                                }
                                break;
                        case VIRTIO_MMIO_QUEUE_SEL:
                            if (!run->mmio.is_write)
                                break;
                            blk_status.queue_sel = *(uint32_t *)run->mmio.data;
                            fprintf(stderr, "[VIRTIO: blk: queue (%d) is selected]\n", blk_status.queue_sel);
                            break;
                        case VIRTIO_MMIO_QUEUE_READY: // RW
                            // TODO: check sanity of QUEUE_SEL
                            if (run->mmio.is_write) {
                                blk_status.queue_ready = *(uint32_t *)run->mmio.data;
                                fprintf(stderr, "[VIRTIO: blk: queue(%d) %s]\n",
                                        blk_status.queue_sel,
                                        blk_status.queue_ready == 1
                                            ? "READY"
                                            : "NOT READY");
                            } else {
                                *(uint32_t *)run->mmio.data = blk_status.queue_ready;
                            }
                            break;
                        case VIRTIO_MMIO_QUEUE_NUM_MAX:
                            if (run->mmio.is_write)
                                break;
                            if (blk_status.queue_sel != 0) {
                                *(uint32_t *)run->mmio.data = 0; // the specified queue is not existent
                                break;
                            }
                            #define QUEUE_SIZE_MAX 1024
                            *(uint32_t *)run->mmio.data = QUEUE_SIZE_MAX;
                            break;
                        case VIRTIO_MMIO_QUEUE_NUM:
                            if (!run->mmio.is_write)
                                break;
                            if (blk_status.queue_sel != 0)
                                break; // the specified queue is not existent
                            blk_status.queue_size = *(uint32_t *)run->mmio.data;
                            fprintf(stderr, "[VIRTIO: blk: queue size (%d) is negotiated]\n", blk_status.queue_size);
                            // TODO: check sanity of given queue_size
                            break;
                        case VIRTIO_MMIO_QUEUE_DESC_HIGH:
                            if (!run->mmio.is_write)
                                break;
                            blk_status.queue_desc_high = *(uint32_t *)run->mmio.data;
                            break;
                        case VIRTIO_MMIO_QUEUE_DESC_LOW:
                            if (!run->mmio.is_write)
                                break;
                            blk_status.queue_desc_low = *(uint32_t *)run->mmio.data;
                            break;
                        case VIRTIO_MMIO_QUEUE_AVAIL_HIGH:
                            if (!run->mmio.is_write)
                                break;
                            blk_status.queue_avail_high = *(uint32_t *)run->mmio.data;
                            break;
                        case VIRTIO_MMIO_QUEUE_AVAIL_LOW:
                            if (!run->mmio.is_write)
                                break;
                            blk_status.queue_avail_low = *(uint32_t *)run->mmio.data;
                            break;
                        case VIRTIO_MMIO_QUEUE_USED_HIGH:
                            if (!run->mmio.is_write)
                                break;
                            blk_status.queue_used_high = *(uint32_t *)run->mmio.data;
                            break;
                        case VIRTIO_MMIO_QUEUE_USED_LOW:
                            if (!run->mmio.is_write)
                                break;
                            blk_status.queue_used_low = *(uint32_t *)run->mmio.data;
                            break;
                        case VIRTIO_MMIO_CONFIG_GENERATION:
                            if (run->mmio.is_write)
                                break;
                            *(uint32_t *)run->mmio.data = 0xbeef; // static value as of now
                            break;
                        case VIRTIO_MMIO_QUEUE_NOTIFY:
                            if (!run->mmio.is_write)
                                break;
                            fprintf(stderr, "[VIRTIO: blk: QUEUE (%d) NOTIFIED]\n", blk_status.queue_sel);
                            // TODO: process IO
                            break;
                        case 0x100 ... 0x1ff: // 0x1ff まで？
                            uint32_t config_offset = offset - 0x100;
                            if (run->mmio.is_write) {
                                memcpy(&blk_config + config_offset, run->mmio.data, run->mmio.len);
                            } else {
                                memcpy(run->mmio.data, &blk_config + config_offset, run->mmio.len);
                            }
                            break;
                        case VIRTIO_MMIO_STATUS:
                                if (run->mmio.len != 4)
                                        break;
                                if (run->mmio.is_write) {
                                        // TODO: replace if/else with switch
                                        if (*(uint32_t *)run->mmio.data == 0) {
                                                // reset. destroy state managed
                                                // by this VMM
                                                fprintf(stderr,
                                                        "[VIRTIO: status: "
                                                        "reset requested]\n");
                                                memset(
                                                    &blk_status, 0,
                                                    sizeof(blk_status));
                                                break;
                                        }

                                        if (*(uint32_t *)run->mmio.data &
                                            VIRTIO_CONFIG_S_ACKNOWLEDGE) {
                                                fprintf(stderr,
                                                        "[VIRTIO: status: "
                                                        "acknowledge (guest "
                                                        "driver has discovered "
                                                        "the device)]\n");
                                                blk_status.status |=
                                                    VIRTIO_CONFIG_S_ACKNOWLEDGE;
                                        }

                                        if (*(uint32_t *)run->mmio.data &
                                            VIRTIO_CONFIG_S_DRIVER) {
                                                fprintf(
                                                    stderr,
                                                    "[VIRTIO: status: driver "
                                                    "(guest has the "
                                                    "approprivate driver)]\n");
                                                blk_status.status |=
                                                    VIRTIO_CONFIG_S_DRIVER;
                                        }

                                        if (*(uint32_t *)run->mmio.data &
                                            VIRTIO_CONFIG_S_DRIVER_OK) {
                                                fprintf(
                                                    stderr,
                                                    "[VIRTIO: status: driver okay\n");
                                                blk_status.status |=
                                                    VIRTIO_CONFIG_S_DRIVER_OK;
                                        }

                                        if (*(uint32_t *)run->mmio.data &
                                            VIRTIO_CONFIG_S_FEATURES_OK) {
                                                fprintf(stderr,
                                                        "[VIRTIO: status: "
                                                        "features are okay]\n");
                                                blk_status.status |=
                                                    VIRTIO_CONFIG_S_FEATURES_OK;
                                        }

                                        break;
                                } else {
                                        *(uint32_t *)run->mmio.data =
                                            blk_status.status;
                                        fprintf(stderr,
                                                "[VIRTIO: status: read: 0x%x]\n",
                                                blk_status.status);
                                        break;
                                }
                                break;
                        default:
                                break;
                        }
                        break;

                case KVM_EXIT_SHUTDOWN:
                        fprintf(stderr, "\nKVM_EXIT_SHUTDOWN\n");
                        return 1;
                case KVM_EXIT_FAIL_ENTRY:
                        fprintf(stderr,
                                "KVM_EXIT_FAIL_ENTRY: "
                                "hardware_entry_failure_reason=0x%llx\n",
                                run->fail_entry.hardware_entry_failure_reason);
                        return 1;

                case KVM_EXIT_INTERNAL_ERROR:
                        fprintf(stderr, "KVM_EXIT_INTERNAL_ERROR\n");
                        return 1;

                default:
                        break; // 他のIOは無視
                }
        }
}
