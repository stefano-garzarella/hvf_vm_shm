#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>

#define SHM_ID "/vhost-user-memory"

// ARM64 instructions to compute ((2 + 2) - 1) and make a hypervisor call with the result
const char s_ckVMCode[] = {
    0x40, 0x00, 0x80, 0xD2, // mov x0, #2
    0x00, 0x08, 0x00, 0x91, // add x0, x0, #2
    0x00, 0x04, 0x00, 0xD1, // sub x0, x0, #1
    0x03, 0x00, 0x00, 0xD4, // smc #0
    0x02, 0x00, 0x00, 0xD4, // hvc #0
    0x00, 0x00, 0x20, 0xD4, // brk #0
};

const uint64_t g_kAdrMainMemory = 0x80000000;
const uint64_t g_szMainMemSize = 0x1000000;

void* g_pMainMemory = NULL;

int main(int argc, const char * argv[])
{
    // unlink it if it already exists, otherwise ftruncate will fail
    shm_unlink(SHM_ID);

    // Based on https://gist.github.com/pldubouilh/c007a311707798b42f31a8d1a09f1138
    // get shared memory file descriptor (NOT a file)
    int memfd = shm_open(SHM_ID, O_RDWR | O_CREAT | O_EXCL, S_IRUSR | S_IWUSR);
    if (memfd == -1) {
        perror("shm_open");
        return -ENOMEM;
    }

    // extend shared memory object as by default it's initialized with size 0
    if (ftruncate(memfd, g_szMainMemSize) == -1) {
        perror("ftruncate");
        return -ENOMEM;
    }

    // map shared memory to process address space
    g_pMainMemory = mmap(NULL, g_szMainMemSize, PROT_WRITE, MAP_SHARED, memfd, 0);
    if (g_pMainMemory == MAP_FAILED) {
        perror("mmap");
        return -ENOMEM;
    }

    // Copy our code into the VM's RAM
    memset(g_pMainMemory, 0, g_szMainMemSize);
    memcpy(g_pMainMemory, s_ckVMCode, sizeof(s_ckVMCode));

    // mmap cleanup
    if (munmap(g_pMainMemory, g_szMainMemSize)  == -1) {
        perror("munmap");
        return 40;
    }

    return 0;
}
