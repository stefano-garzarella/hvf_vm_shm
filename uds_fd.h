#ifndef UDS_FD_H
#define UDS_FD_H

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <sys/socket.h>

static int send_fd(int socket, int *fd) {
        struct msghdr msg = {0};
        struct cmsghdr *cmsg;
        char buf[CMSG_SPACE(sizeof(int))], dup[256];
        struct iovec io = { .iov_base = &dup, .iov_len = sizeof(dup) };

        memset(buf, '\0', sizeof(buf));

        msg.msg_iov = &io;
        msg.msg_iovlen = 1;
        msg.msg_control = buf;
        msg.msg_controllen = sizeof(buf);

        cmsg = CMSG_FIRSTHDR(&msg);
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type = SCM_RIGHTS;
        cmsg->cmsg_len = CMSG_LEN(sizeof(int));

        memcpy ((int *) CMSG_DATA(cmsg), fd, sizeof (int));

        if (sendmsg(socket, &msg, 0) < 0) {
            perror("sendmsg");
            return -1;
        }

        return 0;
}

static int recv_fd(int socket) {
        struct msghdr msg = {0};
        struct cmsghdr *cmsg;
        char buf[CMSG_SPACE(sizeof(int))], dup[256];
        struct iovec io = { .iov_base = &dup, .iov_len = sizeof(dup) };
        int fd = -1;

        memset(buf, '\0', sizeof(buf));

        msg.msg_iov = &io;
        msg.msg_iovlen = 1;
        msg.msg_control = buf;
        msg.msg_controllen = sizeof(buf);

        if (recvmsg(socket, &msg, 0) < 0) {
            perror("recvmsg");
            return -1;
        }

        cmsg = CMSG_FIRSTHDR(&msg);

        memcpy(&fd, (int *) CMSG_DATA(cmsg), sizeof(int));

        return fd;
}

#endif
