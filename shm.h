#ifndef SHM_H
#define SHM_H

// undefine SHM to not use the shared memory for the VM main memory
#define SHM 1
// undefine SHM_UDS to not use the UDS to pass the memfd
#define SHM_UDS 1

#define SHM_ID "/vhost-user-memory"
#define SHM_SIZE 0x1000000

#define UDS_PATH "./vhost-user.sock"

#endif
