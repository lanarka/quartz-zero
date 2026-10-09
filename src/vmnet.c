#include <sys/socket.h>
#include <sys/un.h>
#include <poll.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "vmnet.h"


// Internal helper function: fills in a sockaddr_un for the given
// 'path'. Also checks the path length to prevent overflow.
static int fill_sockaddr(struct sockaddr_un *addr, const char *path) {
    memset(addr, 0, sizeof(*addr));
    addr->sun_family = AF_UNIX;

    if (strlen(path) >= sizeof(addr->sun_path)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    strcpy(addr->sun_path, path);
    return 0;
}

int vmnet_channel_init(VmNetChannel *ch, size_t max_packet) {
    if (ch == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (max_packet == 0)
        max_packet = VMNET_DEFAULT_MAX_PACKET;

    if (max_packet < VMNET_MIN_MAX_PACKET) {
        errno = EINVAL;
        return -1;
    }

    ch->fd          = -1;
    ch->max_packet  = max_packet;
    ch->rx_buf_size = sizeof(VmNetHeader) + max_packet;

    ch->rx_buf = (uint8_t *)malloc(ch->rx_buf_size);
    if (ch->rx_buf == NULL) {
        errno = ENOMEM;
        return -1;
    }

    return 0;
}

void vmnet_channel_close(VmNetChannel *ch) {
    if (ch == NULL)
        return;

    if (ch->fd >= 0) {
        close(ch->fd);//
        ch->fd = -1;
    }

    free(ch->rx_buf);
    ch->rx_buf      = NULL;
    ch->rx_buf_size = 0;
    ch->max_packet  = 0;
}

size_t vmnet_channel_max_packet(const VmNetChannel *ch) {
    if (ch == NULL)
        return 0;

    return ch->max_packet;
}

int vmnet_connect(VmNetChannel *ch, const char *path) {
    int fd;
    struct sockaddr_un addr;

    if (ch == NULL || ch->rx_buf == NULL || path == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (fill_sockaddr(&addr, path) < 0)
        return -1;

    fd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    if (fd < 0)
        return -1;

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        int saved_errno = errno;
        close(fd);
        errno = saved_errno;
        return -1;
    }

    ch->fd = fd;
    return 0;
}

int vmnet_listen(const char *path) {
    int fd;
    struct sockaddr_un addr;

    if (path == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (fill_sockaddr(&addr, path) < 0)
        return -1;

    fd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    if (fd < 0)
        return -1;

    //The socket file may still exist from a previous crash.
    unlink(path);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        int saved_errno = errno;
        close(fd);
        errno = saved_errno;
        return -1;
    }

    if (listen(fd, 1) < 0) {
        int saved_errno = errno;
        close(fd);
        unlink(path);
        errno = saved_errno;
        return -1;
    }

    return fd;
}

int vmnet_accept(int server_fd, VmNetChannel *ch) {
    int client_fd;

    if (ch == NULL || ch->rx_buf == NULL) {
        errno = EINVAL;
        return -1;
    }

    // Intentionally without an internal EINTR-retry loop: the
    // caller (e.g. a demo application with its own signal handler)
    // can thus check its own termination condition after an
    // interrupted accept() and decide whether to retry or exit.
    client_fd = accept(server_fd, NULL, NULL);

    if (client_fd < 0)
        return -1;

    ch->fd = client_fd;
    return 0;
}

int vmnet_send(VmNetChannel *ch, const void *data, size_t len) {
    VmNetHeader header;
    struct iovec iov[2];
    struct msghdr msg;

    if (ch == NULL || ch->fd < 0) {
        errno = EINVAL;
        return -1;
    }

    if (len > ch->max_packet) {
        errno = EMSGSIZE;
        return -1;
    }

    header.version = VMNET_VERSION;
    header.type    = VMNET_MSG_PACKET;
    header.length  = (uint32_t)len;

    memset(&msg, 0, sizeof(msg));//

    iov[0].iov_base = &header;
    iov[0].iov_len  = sizeof(header);

    iov[1].iov_base = (void *)data;
    iov[1].iov_len  = len;

    msg.msg_iov    = iov;
    msg.msg_iovlen = 2;

    for (;;) {
        ssize_t n = sendmsg(ch->fd, &msg, 0);

        if (n >= 0) {
            if ((size_t)n != sizeof(header) + len) {
                errno = EIO;
                return -1;
            }
            return 0;
        }

        if (errno == EINTR)
            continue;

        return -1;
    }
}

ssize_t vmnet_recv(VmNetChannel *ch, void *data, size_t capacity) {
    ssize_t n;
    VmNetHeader header;

    if (ch == NULL || ch->fd < 0 || ch->rx_buf == NULL) {
        errno = EINVAL;
        return -1;
    }

    for (;;) {
        n = recv(ch->fd, ch->rx_buf, ch->rx_buf_size, 0);

        if (n >= 0)
            break;

        if (errno == EINTR)
            continue;

        return -1;
    }

    if (n == 0)
        return 0;

    if ((size_t)n < sizeof(VmNetHeader)) {
        errno = EPROTO;
        return -1;
    }

    memcpy(&header, ch->rx_buf, sizeof(header));

    if (header.version != VMNET_VERSION) {
        errno = EPROTO;
        return -1;
    }

    if (header.type != VMNET_MSG_PACKET) {
        errno = EPROTO;
        return -1;
    }

    if (header.length > ch->max_packet) {
        errno = EPROTO;
        return -1;
    }

    if ((size_t)n != sizeof(VmNetHeader) + header.length) {
        errno = EPROTO;
        return -1;
    }

    if (header.length > capacity) {
        errno = EMSGSIZE;
        return -1;
    }

    memcpy(data, ch->rx_buf + sizeof(VmNetHeader), header.length);

    return (ssize_t)header.length;
}

int vmnet_poll_readable(VmNetChannel *ch, int timeout_ms) {
    struct pollfd pfd;
    int ret;

    if (ch == NULL || ch->fd < 0) {
        errno = EINVAL;
        return -1;
    }

    pfd.fd     = ch->fd;
    pfd.events = POLLIN;

    for (;;) {
        pfd.revents = 0;
        ret = poll(&pfd, 1, timeout_ms);

        if (ret < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }

        break;
    }

    if (ret == 0)
        return 0;

    if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))
        return 1; // let vmnet_recv handle it (it will return 0 or -1)

    if (pfd.revents & POLLIN)
        return 1;

    return 0;
}
