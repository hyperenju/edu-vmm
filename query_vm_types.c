#define _GNU_SOURCE

#include <fcntl.h>
#include <linux/kvm.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
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

char vm_type_name[][32] = {
    "KVM_X86_DEFAULT_VM", "KVM_X86_SW_PROTECTED_VM", "KVM_X86_SEV_VM",
    "KVM_X86_SEV_ES_VM",  "KVM_X86_SNP_VM",          "KVM_X86_TDX_VM",
};

int main(void) {
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

    int vm_types = ioctl(dev_fd, KVM_CHECK_EXTENSION, KVM_CAP_VM_TYPES);
    if (vm_types == 0)
        die("ioctl(KVM_CHECK_EXTENSION, KVM_CAP_VM_TYPES) not supported");


    printf("Supported VM TYPES:\n");
    for (int i = 0; i < sizeof(vm_type_name) / sizeof(vm_type_name[0]); i++) {
        if (vm_types & (1 << i))
            printf("\t%s\n", vm_type_name[i]);
    }

out:
    close(dev_fd);
}
