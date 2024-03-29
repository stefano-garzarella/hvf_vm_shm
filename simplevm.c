// Based on https://gist.github.com/imbushuo/51b09e61ecd7b7ac063853ad65cedf34

// simplevm.c: demonstrates Hypervisor.Framework usage in Apple Silicon
// Based on the work by @zhuowei
// @imbushuo - Nov 2020

// To build:
// Prepare the entitlement with BOTH com.apple.security.hypervisor and com.apple.vm.networking WHEN SIP IS OFF
// Prepare the entitlement com.apple.security.hypervisor and NO com.apple.vm.networking WHEN SIP IS ON
// ^ Per @never_released, tested on 11.0.1, idk why
// clang -o simplevm -O2 -framework Hypervisor -mmacosx-version-min=11.0 simplevm.c
// codesign --entitlements simplevm.entitlements --force -s - simplevm             

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <sys/socket.h>

#include <Hypervisor/Hypervisor.h>

#include "shm.h"
#include "uds_fd.h"

// Diagnostics
#define HYP_ASSERT_SUCCESS(ret) assert((hv_return_t) (ret) == HV_SUCCESS)

// ARM64 reset tramp
// I don't know why the CPU resets at EL0, so this is a trampoline
// that takes you to EL1.
// UPDATE: See CPSR below
const char s_cArm64ResetVector[] = {
    0x01, 0x00, 0x00, 0xD4, // svc #0
    // If BRK is caught by host (configured)
    // Something happened badly
    0x00, 0x00, 0x20, 0xD4, // brk #0
};

const char s_cArm64ResetTramp[] = {
    0x00, 0x00, 0xB0, 0xD2, // mov x0, #0x80000000
    0x00, 0x00, 0x1F, 0xD6, // br  x0
    // If BRK is caught by host (configured)
    // Something happened badly
    0x00, 0x00, 0x20, 0xD4, // brk #0
};

// ARM64 instructions to compute ((2 + 2) - 1) and make a hypervisor call with the result
const char s_ckVMCode[] = {
    0x40, 0x00, 0x80, 0xD2, // mov x0, #2
    0x00, 0x08, 0x00, 0x91, // add x0, x0, #2
    0x00, 0x04, 0x00, 0xD1, // sub x0, x0, #1
    0x03, 0x00, 0x00, 0xD4, // smc #0
    0x02, 0x00, 0x00, 0xD4, // hvc #0
    0x00, 0x00, 0x20, 0xD4, // brk #0
};

// Overview of this memory layout:
// Main memory starts at 0x80000000
// Reset VBAR_EL1 at 0xF0000000 - 0xF0010000;
// Reset trampoline code at 0xF0000800
const uint64_t g_kAdrResetTrampoline = 0xF0000000;
const uint64_t g_szResetTrampolineMemory = 0x10000;

const uint64_t g_kAdrMainMemory = 0x80000000;
const uint64_t g_szMainMemSize = SHM_SIZE;

void* g_pResetTrampolineMemory = NULL;
void* g_pMainMemory = NULL;

int RecvRemoteMemFd() {
    int memfd = -1;

#ifndef SHM_UDS
    printf("Using shm_open() to open the memfd\n");
    // Based on https://gist.github.com/pldubouilh/c007a311707798b42f31a8d1a09f1138
    // get shared memory file descriptor (NOT a file)
    memfd = shm_open(SHM_ID, O_RDWR, S_IRUSR | S_IWUSR);
    if (memfd == -1) {
        perror("shm_open");
    }
#else
    struct sockaddr_un addr;
    int sfd;

    printf("Using UDS to receive the memfd\n");

    sfd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sfd < 0) {
        perror("socket");
        return -1;
    }

    memset(&addr, 0, sizeof(struct sockaddr_un));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, UDS_PATH, sizeof(addr.sun_path) - 1);

    if (connect(sfd, (struct sockaddr *) &addr, sizeof(struct sockaddr_un)) == -1) {
        perror("connect");
        close(sfd);
        return -1;
    }

    memfd = recv_fd(sfd);

    close(sfd);
#endif

    return memfd;

}

int VmpPrepareSystemMemory()
{
    // Reset trampoline
    // Well, dear Apple, why you reset the CPU at EL0
    posix_memalign(&g_pResetTrampolineMemory, 0x10000, g_szResetTrampolineMemory);
    if (g_pResetTrampolineMemory == NULL) {
        return -ENOMEM;
    }

    memset(g_pResetTrampolineMemory, 0, g_szResetTrampolineMemory);
    for (uint64_t offset = 0; offset < 0x780; offset += 0x80) {
        memcpy((void*) g_pResetTrampolineMemory + offset, s_cArm64ResetTramp, sizeof(s_cArm64ResetTramp));
    }

    memcpy((void*) g_pResetTrampolineMemory + 0x800, s_cArm64ResetVector, sizeof(s_cArm64ResetVector));

    // Map the RAM into the VM
    HYP_ASSERT_SUCCESS(hv_vm_map(g_pResetTrampolineMemory, g_kAdrResetTrampoline, g_szResetTrampolineMemory, HV_MEMORY_READ | HV_MEMORY_EXEC));

    // Main memory.
#ifdef SHM
    int memfd = RecvRemoteMemFd();
    if (memfd < 0) {
        return -ENOMEM;
    }

    // map shared memory to process address space
    g_pMainMemory = mmap(NULL, g_szMainMemSize, PROT_WRITE, MAP_SHARED, memfd, 0);
    if (g_pMainMemory == MAP_FAILED) {
        perror("mmap");
        return -ENOMEM;
    }
#else
    posix_memalign(&g_pMainMemory, 0x1000, g_szMainMemSize);
    if (g_pMainMemory == NULL) {
        return -ENOMEM;
    }

    // Copy our code into the VM's RAM
    memset(g_pMainMemory, 0, g_szMainMemSize);
    memcpy(g_pMainMemory, s_ckVMCode, sizeof(s_ckVMCode));
#endif

    // Map the RAM into the VM
    HYP_ASSERT_SUCCESS(hv_vm_map(g_pMainMemory, g_kAdrMainMemory, g_szMainMemSize, HV_MEMORY_READ | HV_MEMORY_WRITE | HV_MEMORY_EXEC));

    return 0;
}

int main(int argc, const char * argv[])
{
    hv_vcpu_t vcpu;
    hv_vcpu_exit_t *vcpu_exit;

    // Create the VM
    HYP_ASSERT_SUCCESS(hv_vm_create(NULL));

    // Prepare the memory layout
    if (VmpPrepareSystemMemory()) {
        abort();
    }
    
    // Add a virtual CPU to our VM
    HYP_ASSERT_SUCCESS(hv_vcpu_create(&vcpu, &vcpu_exit, NULL));

    // Configure initial VBAR_EL1 to the trampoline
    HYP_ASSERT_SUCCESS(hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_VBAR_EL1, g_kAdrResetTrampoline));

#if USE_EL0_TRAMPOILNE
    // Set the CPU's PC to execute from the trampoline
    HYP_ASSERT_SUCCESS(hv_vcpu_set_reg(vcpu, HV_REG_PC, g_kAdrResetTrampoline + 0x800));
#else
    // Or explicitly set CPSR
    HYP_ASSERT_SUCCESS(hv_vcpu_set_reg(vcpu, HV_REG_CPSR, 0x3c4));
    HYP_ASSERT_SUCCESS(hv_vcpu_set_reg(vcpu, HV_REG_PC, 0x80000000));
#endif

    // Configure misc
    HYP_ASSERT_SUCCESS(hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_SP_EL0, g_kAdrMainMemory + 0x4000));
    HYP_ASSERT_SUCCESS(hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_SP_EL1, g_kAdrMainMemory + 0x8000));

    // Trap debug access (BRK)
    HYP_ASSERT_SUCCESS(hv_vcpu_set_trap_debug_exceptions(vcpu, true));

    // start the VM
    while (true) {
        // Run the VM until a VM exit
        HYP_ASSERT_SUCCESS(hv_vcpu_run(vcpu));
        // Check why we exited the VM
        if (vcpu_exit->reason == HV_EXIT_REASON_EXCEPTION) {
            // Check if this is an HVC call
            // https://developer.arm.com/docs/ddi0595/e/aarch64-system-registers/esr_el2
            uint64_t syndrome = vcpu_exit->exception.syndrome;
            uint8_t ec = (syndrome >> 26) & 0x3f;
            // check Exception Class
            if (ec == 0x16) {
                // Exception Class 0x16 is
                // "HVC instruction execution in AArch64 state, when HVC is not disabled."
                uint64_t x0;
                HYP_ASSERT_SUCCESS(hv_vcpu_get_reg(vcpu, HV_REG_X0, &x0));
                printf("VM made an HVC call! x0 register holds 0x%llx\n", x0);
                //break;
            } else if (ec == 0x17) {
                // Exception Class 0x17 is
                // "SMC instruction execution in AArch64 state, when SMC is not disabled."

                // Yes despite M1 doesn't have EL3, it is capable to trap it too. :)
                uint64_t x0;
                HYP_ASSERT_SUCCESS(hv_vcpu_get_reg(vcpu, HV_REG_X0, &x0));
                printf("VM made an SMC call! x0 register holds 0x%llx\n", x0);
                printf("Return to get on next instruction.\n");

                // ARM spec says trapped SMC have different return path, so it is required
                // to increment elr_el2 by 4 (one instruction.)
                uint64_t pc;
                HYP_ASSERT_SUCCESS(hv_vcpu_get_reg(vcpu, HV_REG_PC, &pc));
                pc += 4;
                HYP_ASSERT_SUCCESS(hv_vcpu_set_reg(vcpu, HV_REG_PC, pc));
            } else if (ec == 0x3C) {
                // Exception Class 0x3C is BRK in AArch64 state
                uint64_t x0;
                HYP_ASSERT_SUCCESS(hv_vcpu_get_reg(vcpu, HV_REG_X0, &x0));
                printf("VM made an BRK call!\n");
                printf("Reg dump:\n");
                for (uint32_t reg = HV_REG_X0; reg < HV_REG_X5; reg++) {
                    uint64_t s;
                    HYP_ASSERT_SUCCESS(hv_vcpu_get_reg(vcpu, reg, &s));
                    printf("X%d: 0x%llx\n", reg, s);
                }
                break;
            } else {
                fprintf(stderr, "Unexpected VM exception: 0x%llx, EC 0x%x, VirtAddr 0x%llx, IPA 0x%llx\n",
                    syndrome,
                    ec,
                    vcpu_exit->exception.virtual_address,
                    vcpu_exit->exception.physical_address
                );
                break;
            }
        } else {
            fprintf(stderr, "Unexpected VM exit reason: %d\n", vcpu_exit->reason);
            break;
        }
    }

    // Tear down the VM
    hv_vcpu_destroy(vcpu);
    hv_vm_destroy();

    // Free memory
    free(g_pResetTrampolineMemory);
#ifdef SHM
    // mmap cleanup
    if (munmap(g_pMainMemory, g_szMainMemSize)  == -1) {
        perror("munmap");
        return 40;
    }

    // shm_open cleanup
    if (shm_unlink(SHM_ID)  == -1) {
        perror("shm_unlink");
        return 100;
    }
#else
    free(g_pMainMemory);
#endif

    return 0;
}
