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
#include <errno.h>

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

struct virtio_queue {
    uint64_t desc_guest_addr;
    uint64_t avail_guest_addr;
    uint64_t used_guest_addr;

    uint32_t queue_ready;
    uint32_t queue_size;

    uint16_t last_avail_index;
};

/* stateful fields other than virtqueue. reset when driver requests */
struct virtio_blk_state {
        uint32_t status;
        uint32_t device_feature_sel;
        uint32_t driver_feature_sel;
        uint32_t queue_sel;
        uint32_t interrupt_status;
        uint32_t negotiated_features[2];
};

struct virtio_dev {
    void *mem; /* guest memory */
    int vm_fd;
};

struct virtio_blk_dev {
        /* volatile fields */
        struct virtio_blk_state state;
        struct virtio_queue queue;

        /* static fields */
        uint32_t device_features[2];
        uint32_t irq_number; 
        uint32_t queue_size_max; 
        int disk_fd;
        struct virtio_blk_config config;

        struct virtio_dev dev;
};

struct virtq_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
};

struct virtq_avail {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[];
};

struct virtq_used_elem {
    uint32_t id;
    uint32_t len;
};

struct virtq_used {
    uint16_t flags;
    uint16_t idx;
    struct virtq_used_elem ring[];
};

struct virtio_blk_req {
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
};

#define SECTOR_SIZE 512

void do_virtio_blk_io(struct virtio_blk_dev *blk_dev) {
    void *guest_mem = blk_dev->dev.mem;
    int disk_fd = blk_dev->disk_fd;
    int vm_fd = blk_dev->dev.vm_fd;
    struct virtq_desc *desc_ring = guest_mem + blk_dev->queue.desc_guest_addr;
    struct virtq_avail *avail = guest_mem + blk_dev->queue.avail_guest_addr;
    struct virtq_used *used = guest_mem + blk_dev->queue.used_guest_addr;
    int err;

    while (blk_dev->queue.last_avail_index != avail->idx) {
        uint16_t desc_idx = avail->ring[blk_dev->queue.last_avail_index % blk_dev->queue.queue_size];
        struct virtq_desc *desc = &desc_ring[desc_idx];
        struct virtio_blk_req *req = guest_mem + desc->addr;
        struct virtq_desc *data_desc;
        struct virtq_desc *status_desc;

        fprintf(stderr, "[VIRTIO: BLK: desc(%d): at 0x%lx with size = 0x%x, next = %d]\n", desc_idx, desc->addr, desc->len, desc->next);
        fprintf(stderr, "[VIRTIO: BLK: req: type = %d]\n", req->type);


        uint8_t status = VIRTIO_BLK_S_OK;
        status_desc = &desc_ring[desc->next]; // default

        switch (req->type) {
            case VIRTIO_BLK_T_IN:
                data_desc = &desc_ring[desc->next];
                status_desc = &desc_ring[data_desc->next];

                err = lseek(disk_fd, req->sector * SECTOR_SIZE, SEEK_SET);
                if (err == -1) {
                    fprintf(stderr, "[VIRTIO: BLK: seek err(%d)]\n", errno);
                    status = VIRTIO_BLK_S_IOERR;
                    break;
                }

                err = read(disk_fd, (void *)(guest_mem + data_desc->addr), data_desc->len);
                if (err == -1) {
                    fprintf(stderr, "[VIRTIO: BLK: read err(%d)]\n", errno);
                    status = VIRTIO_BLK_S_IOERR;
                    break;
                }

                break;
            case VIRTIO_BLK_T_OUT:
                data_desc = &desc_ring[desc->next];
                status_desc = &desc_ring[data_desc->next];

                err = lseek(disk_fd, req->sector * SECTOR_SIZE, SEEK_SET);
                if (err == -1) {
                    fprintf(stderr, "[VIRTIO: BLK: seek err(%d)]\n", errno);
                    status = VIRTIO_BLK_S_IOERR;
                    break;
                }

                err = write(disk_fd, (void *)(guest_mem + data_desc->addr), data_desc->len);
                if (err == -1) {
                    fprintf(stderr, "[VIRTIO: BLK: write err(%d)]\n", errno);
                    status = VIRTIO_BLK_S_IOERR;
                    break;
                }

                break;
            case VIRTIO_BLK_T_FLUSH:
                err = fsync(disk_fd);
                if (err) {
                    fprintf(stderr, "[VIRTIO: BLK: FLUSH(fsync) err(%d)]\n", errno);
                    status = VIRTIO_BLK_S_IOERR;
                    break;
                }

                break;
            default:
                status = VIRTIO_BLK_S_UNSUPP;
                break;
        }

        *(uint8_t *)(guest_mem + status_desc->addr) = status;
        used->ring[used->idx % blk_dev->queue.queue_size].id = desc_idx;
        used->ring[used->idx % blk_dev->queue.queue_size].len = 1;
        used->idx++;

        blk_dev->queue.last_avail_index++;
    }

    blk_dev->state.interrupt_status |= VIRTIO_MMIO_INT_VRING;

    struct kvm_irq_level irq = {
        .irq = blk_dev->irq_number,
        .level = 1, // assert
    };
    err = ioctl(vm_fd, KVM_IRQ_LINE, &irq);
    if (err)
        fprintf(stderr, "[VIRTIO: BLK: KVM_IRQ_LINE (asserting IRQ) failed]\n");
}

#define ROOT_FS "/home/kohei/myqemu/Fedora-Server-KVM-Desktop-42.x86_64.ext4"
#define MAX_CMDLINE_LEN 1024

// virtio
#define IRQ_NUMBER 5

// virtio-blk over mmio
// Don't overlap with the memory region
#define VIRTIO_BLK_MMIO_BASE 0x80000000 // これ、blk_dev に持たせてよくない？
#define VIRTIO_BLK_MMIO_SIZE 0x1000 // あと、「このrangeだったらこのハンドラを呼び出す」ってのを作ったほうがいいかも。virtio device が増えるなら。

// virtio-blk specific
#define QUEUE_SIZE_MAX 1024

static const struct {
        uint32_t bit;
        const char *name;
} status_bits[] = {
    {VIRTIO_CONFIG_S_ACKNOWLEDGE, "acknowledge"},
    {VIRTIO_CONFIG_S_DRIVER, "driver"},
    {VIRTIO_CONFIG_S_DRIVER_OK, "driver_ok"},
    {VIRTIO_CONFIG_S_FEATURES_OK, "features_ok"},
    {VIRTIO_CONFIG_S_NEEDS_RESET, "needs_reset"},
    {VIRTIO_CONFIG_S_FAILED, "failed"},
};

static void dump_status(uint32_t status) {
        fprintf(stderr, "[VIRTIO: status: write 0x%x (", status);
        for (int i = 0; i < sizeof(status_bits) / sizeof(status_bits[0]); i++)
                if (status_bits[i].bit & status)
                        fprintf(stderr, "%s ", status_bits[i].name);
        fprintf(stderr, ")]\n");
}

#define DUMMY_VENDOR_ID 0x0
#define VIRTIO_MMIO_MAGIC "virt"
#define VIRTIO_MMIO_VERSION_MODERN 2

static void virtio_mmio_needs_reset(struct virtio_blk_dev *blk_dev) {
        fprintf(stderr, "[VIRTIO: BLK: needs reset. requesting driver to reset "
                        "it's state\n");
        blk_dev->state.status = VIRTIO_CONFIG_S_NEEDS_RESET;
        blk_dev->state.interrupt_status = VIRTIO_MMIO_INT_CONFIG;

        struct kvm_irq_level irq = {
            .irq = blk_dev->irq_number,
            .level = 1,
        };
        int err = ioctl(blk_dev->dev.vm_fd, KVM_IRQ_LINE, &irq);
        if (err)
                fprintf(stderr, "[VIRTIO: BLK: KVM_IRQ_LINE "
                                "(deasserting IRQ) failed]\n");
}

static void do_virtio_blk(typeof(((struct kvm_run *)0)->mmio) *mmio, struct virtio_blk_dev *blk_dev) {
        uint32_t mmio_offset = (uint32_t)mmio->phys_addr - VIRTIO_BLK_MMIO_BASE;
        uint32_t sel;

        /* access to MMIO configuration space */
        if (mmio_offset >= VIRTIO_MMIO_CONFIG && mmio_offset < VIRTIO_MMIO_CONFIG + sizeof(struct virtio_blk_config)) {
                uint32_t config_offset = mmio_offset - VIRTIO_MMIO_CONFIG;

                if (mmio->is_write) {
                        memcpy((void *)&blk_dev->config + config_offset,
                               mmio->data, mmio->len);
                } else {
                        memcpy(mmio->data,
                               (void *)&blk_dev->config + config_offset,
                               mmio->len);
                }

                return;
        }

        /* access to MMIO registers */
        if (mmio->len != 4)
                return;

        switch (mmio_offset) {
        case VIRTIO_MMIO_MAGIC_VALUE:
                if (mmio->is_write)
                        break;
                memcpy(mmio->data, &VIRTIO_MMIO_MAGIC, 4);
                break;
        case VIRTIO_MMIO_VERSION:
                if (mmio->is_write)
                        break;
                *(uint32_t *)mmio->data = VIRTIO_MMIO_VERSION_MODERN;
                break;
        case VIRTIO_MMIO_DEVICE_ID:
                if (mmio->is_write)
                        break;
                *(uint32_t *)mmio->data = VIRTIO_ID_BLOCK;
                break;
        case VIRTIO_MMIO_VENDOR_ID:
                if (mmio->is_write)
                        break;
                *(uint32_t *)mmio->data = DUMMY_VENDOR_ID;
                break;
        case VIRTIO_MMIO_DEVICE_FEATURES_SEL:
                if (!mmio->is_write)
                        break;
                blk_dev->state.device_feature_sel = *(uint32_t *)mmio->data;
                fprintf(stderr, "[VIRTIO: feature(device): sel = %d]\n",
                        blk_dev->state.device_feature_sel);
                break;
        case VIRTIO_MMIO_DEVICE_FEATURES:
                if (mmio->is_write)
                        break;

                sel = blk_dev->state.device_feature_sel;
                if (sel > 1) {
                        *(uint32_t *)mmio->data = 0;
                        break;
                }

                *(uint32_t *)mmio->data = blk_dev->device_features[sel];
                break;
        case VIRTIO_MMIO_DRIVER_FEATURES_SEL:
                if (!mmio->is_write)
                        break;
                blk_dev->state.driver_feature_sel = *(uint32_t *)mmio->data;
                fprintf(stderr, "[VIRTIO: feature(driver): sel = %d]\n",
                        blk_dev->state.driver_feature_sel);
                break;
        case VIRTIO_MMIO_DRIVER_FEATURES:
                if (!mmio->is_write)
                        break;

                sel = blk_dev->state.driver_feature_sel;
                if (sel > 1) {
                    break;
                }

                blk_dev->state.negotiated_features[sel] = *(uint32_t *)mmio->data;
                if (blk_dev->state.negotiated_features[sel] !=
                    blk_dev->device_features[sel]) {
                        fprintf(stderr,
                                "[VIRTIO: BLK: degraded features(sel=%d), "
                                "offerred %d, but driver accepted %d]\n",
                                sel, blk_dev->device_features[sel],
                                blk_dev->state.negotiated_features[sel]);
                }

                if (sel == 1 && !(blk_dev->state.negotiated_features[1] &
                                  (1 << VIRTIO_F_VERSION_1 % 32))) {
                        fprintf(stderr, "[VIRTIO: BLK: driver didn't accept "
                                        "VIRTIO_F_VERSION_1. abort\n");
                        virtio_mmio_needs_reset(blk_dev);
                }

                break;
        case VIRTIO_MMIO_QUEUE_SEL:
                if (!mmio->is_write)
                        break;
                blk_dev->state.queue_sel = *(uint32_t *)mmio->data;
                fprintf(stderr, "[VIRTIO: blk: queue (%d) is selected]\n",
                        blk_dev->state.queue_sel);
                break;
        case VIRTIO_MMIO_QUEUE_READY: // RW
                // TODO: check sanity of QUEUE_SEL. always should be 0 in
                // virtio-blk
                if (mmio->is_write) {
                        blk_dev->queue.queue_ready = *(uint32_t *)mmio->data;
                        fprintf(stderr, "[VIRTIO: blk: queue(%d) %s]\n",
                                blk_dev->state.queue_sel,
                                blk_dev->queue.queue_ready == 1 ? "READY"
                                                                : "NOT READY");
                } else {
                        *(uint32_t *)mmio->data = blk_dev->queue.queue_ready;
                }
                break;
        case VIRTIO_MMIO_QUEUE_NUM_MAX:
                if (mmio->is_write)
                        break;
                if (blk_dev->state.queue_sel != 0) {
                        *(uint32_t *)mmio->data =
                            0; // the specified queue is not existent
                        break;
                }
                *(uint32_t *)mmio->data = blk_dev->queue_size_max;
                break;
        case VIRTIO_MMIO_QUEUE_NUM:
                if (!mmio->is_write)
                        break;
                if (blk_dev->state.queue_sel != 0)
                        break; // the specified queue is not existent

                uint32_t negotiated_queue_size = *(uint32_t *)mmio->data;
                if (negotiated_queue_size > blk_dev->queue_size_max) {
                    fprintf(stderr,
                            "[VIRTIO: BLK: invalid queue size (%d). larger "
                            "than max size (%d)]\n",
                            negotiated_queue_size, blk_dev->queue_size_max);

                    virtio_mmio_needs_reset(blk_dev);
                    break;
                }

                blk_dev->queue.queue_size = negotiated_queue_size;
                fprintf(stderr,
                        "[VIRTIO: blk: queue size (%d) is negotiated]\n",
                        blk_dev->queue.queue_size);
                break;
        case VIRTIO_MMIO_QUEUE_DESC_HIGH:
                if (!mmio->is_write)
                        break;
                blk_dev->queue.desc_guest_addr |=
                    (uint64_t)(*(uint32_t *)mmio->data) << 32;
                break;
        case VIRTIO_MMIO_QUEUE_DESC_LOW:
                if (!mmio->is_write)
                        break;
                blk_dev->queue.desc_guest_addr |=
                    (uint64_t)(*(uint32_t *)mmio->data);
                break;
        case VIRTIO_MMIO_QUEUE_AVAIL_HIGH:
                if (!mmio->is_write)
                        break;
                blk_dev->queue.avail_guest_addr |=
                    (uint64_t)(*(uint32_t *)mmio->data) << 32;
                break;
        case VIRTIO_MMIO_QUEUE_AVAIL_LOW:
                if (!mmio->is_write)
                        break;
                blk_dev->queue.avail_guest_addr |=
                    (uint64_t)(*(uint32_t *)mmio->data);
                break;
        case VIRTIO_MMIO_QUEUE_USED_HIGH:
                if (!mmio->is_write)
                        break;
                blk_dev->queue.used_guest_addr |=
                    (uint64_t)(*(uint32_t *)mmio->data) << 32;
                break;
        case VIRTIO_MMIO_QUEUE_USED_LOW:
                if (!mmio->is_write)
                        break;
                blk_dev->queue.used_guest_addr |=
                    (uint64_t)(*(uint32_t *)mmio->data);
                break;
        case VIRTIO_MMIO_CONFIG_GENERATION:
                if (mmio->is_write)
                        break;
                // static since we don't change MMIO configuration space
                *(uint32_t *)mmio->data = 0;
                break;
        case VIRTIO_MMIO_QUEUE_NOTIFY:
                if (!mmio->is_write)
                        break;
                fprintf(stderr, "[VIRTIO: blk: QUEUE (%d) NOTIFIED]\n",
                        blk_dev->state.queue_sel);
                do_virtio_blk_io(blk_dev);
                break;
        case VIRTIO_MMIO_INTERRUPT_STATUS:
                if (mmio->is_write)
                        break;
                *(uint32_t *)mmio->data = blk_dev->state.interrupt_status;
                break;
        case VIRTIO_MMIO_INTERRUPT_ACK:
                if (!mmio->is_write)
                        break;
                blk_dev->state.interrupt_status &= ~(*(uint32_t *)mmio->data);
                struct kvm_irq_level irq = {
                    .irq = blk_dev->irq_number,
                    .level = 0, // deassert
                };
                int err = ioctl(blk_dev->dev.vm_fd, KVM_IRQ_LINE, &irq);
                if (err)
                        fprintf(stderr, "[VIRTIO: BLK: KVM_IRQ_LINE "
                                        "(deasserting IRQ) failed]\n");
                break;
        case VIRTIO_MMIO_STATUS:
                if (!mmio->is_write) { /* READ */
                        *(uint32_t *)mmio->data = blk_dev->state.status;
                        dump_status(blk_dev->state.status);
                        break;
                }

                /* Write */
                uint32_t new_status = *(uint32_t *)mmio->data;
                if (!new_status) {
                        fprintf(stderr, "[VIRTIO: status: "
                                        "reset requested]\n");
                        memset(&blk_dev->state, 0, sizeof(blk_dev->state));
                        memset(&blk_dev->queue, 0, sizeof(blk_dev->queue));
                        break;
                }

                blk_dev->state.status = new_status;
                dump_status(new_status);
                break;

        default:
                fprintf(stderr, "[VIRTIO: BLK: unhandled offset: %d]\n", mmio_offset);
                break;
        }
}

int virtio_blk_sw_init(struct virtio_blk_dev *blk_dev, char *rootfs, void *mem, int vm_fd) {
        struct stat st;

        blk_dev->irq_number = IRQ_NUMBER;
        blk_dev->queue_size_max = QUEUE_SIZE_MAX;
        blk_dev->device_features[0] = 1 << (VIRTIO_BLK_F_FLUSH);
        blk_dev->device_features[1] = 1 << (VIRTIO_F_VERSION_1 % 32);

        blk_dev->dev.mem = mem;
        blk_dev->dev.vm_fd = vm_fd;

        blk_dev->disk_fd = open(rootfs, O_RDWR);
        if (blk_dev->disk_fd < 0) {
                perror("open rootfs");
                return 1;
        }

        fstat(blk_dev->disk_fd, &st);
        blk_dev->config.capacity = (st.st_size - 1) / SECTOR_SIZE + 1;

        return 0;
};

int main(int argc, char *argv[]) {
        struct virtio_blk_dev blk_dev = {0};
        int err;


        char cmdline[MAX_CMDLINE_LEN];
        const char *cmdline_fmt =
            "console=ttyS0 root=/dev/vda "
            /* Minimize uneccesary IO port VM Exit (see firecracker) */
            "i8042.noaux i8042.nomux i8042.dumbkbd "
            /* Allow guest kernel to locate the virtio device via MMIO transport */
            "virtio_mmio.device=0x%lx@0x%lx:%d "
            /* disable needless features */
            "audit=0 selinux=0 nokaslr ";

        if (argc < 2) {
                fprintf(stderr, "Usage: %s <bzImage> <rootfs(optional)>\n", argv[0]);
                return 1;
        }

        if (snprintf(cmdline, MAX_CMDLINE_LEN, cmdline_fmt, VIRTIO_BLK_MMIO_SIZE, VIRTIO_BLK_MMIO_BASE, IRQ_NUMBER) < 0) {
            perror("snprintf");
            return 1;
        }

        char *rootfs = ROOT_FS;
        if (argc == 3)
            rootfs = argv[2];

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

        err = virtio_blk_sw_init(&blk_dev, rootfs, mem, vm_fd);
        if (err) {
                perror("virtio_blk_sw_init");
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
                                // fprintf(stderr, "[unhandled IO %s port=0x%x
                                // size=%d]\n",
                                //         run->io.direction ? "OUT" : "IN",
                                //         run->io.port, run->io.size);
                        }
                        break;
                case KVM_EXIT_MMIO:
                        if ((uint32_t)run->mmio.phys_addr >=
                                VIRTIO_BLK_MMIO_BASE &&
                            (uint32_t)run->mmio.phys_addr <
                                VIRTIO_BLK_MMIO_BASE + VIRTIO_BLK_MMIO_SIZE) {
                                do_virtio_blk(&run->mmio, &blk_dev);
                                break;
                        }

                        if (run->mmio.is_write)
                                fprintf(stderr,
                                        "[unhandled MMIO %s at 0x%x with size = %d, "
                                        "data=(0x%x, "
                                        "0x%x, 0x%x, 0x%x)]\n",
                                        run->mmio.is_write ? "write" : "read",
                                        (uint32_t)run->mmio.phys_addr,
                                        run->mmio.len, run->mmio.data[0],
                                        run->mmio.data[1], run->mmio.data[2],
                                        run->mmio.data[3]);
                        if (!run->mmio.is_write)
                                fprintf(stderr,
                                        "[unhandled MMIO %s at 0x%x with size = %d]\n",
                                        run->mmio.is_write ? "write" : "read",
                                        (uint32_t)run->mmio.phys_addr,
                                        run->mmio.len);
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
