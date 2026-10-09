#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <errno.h>
#include "vmnet_peer.h"

volatile sig_atomic_t vmnet_peer_stop = 0;

static void on_signal(int sig) {
    (void)sig;
    vmnet_peer_stop = 1;
}

void vmnet_peer_install_signal_handlers(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
}


// Single-byte "type" at the start of the payload, purely at the
// level of this demo application (the vmnet library knows nothing
// about it - to the library it's just plain packet content).
//
//   PEER_KIND_TEXT - the original message, typed by the user.
//                    The receiver prints it and SENDS IT BACK as
//                    an echo.
//   PEER_KIND_ECHO - this is already an echo reply to my own
//                    message. The receiver only prints it and
//                    does not echo it further - this prevents an
//                    infinite ping-pong loop.
enum {
    PEER_KIND_TEXT = 1,
    PEER_KIND_ECHO = 2
};


// A simple line buffer on top of raw fd 0.
//
// We deliberately don't use fgets()/stdio on stdin: buffered stdio
// reads from the file descriptor in its own (larger) blocks, so
// poll() on fd 0 could indicate "nothing ready" even though a
// whole next line is already waiting in the stdio buffer - a
// classic problem when mixing stdio with poll()/select() on the
// same fd.
typedef struct {
    char   buf[4096];
    size_t len;
} LineReader;

static int line_reader_step(LineReader *lr, int fd, char *out, size_t out_cap) {
    char *nl = memchr(lr->buf, '\n', lr->len);

    if (nl == NULL) {
        ssize_t n;

        if (lr->len >= sizeof(lr->buf)) {
            n = 0; // a line without '\n' exceeded the buffer, close it out
        } else {
            n = read(fd, lr->buf + lr->len, sizeof(lr->buf) - lr->len);
        }

        if (n < 0) {
            if (errno == EINTR)
                return 0;
            return -1;
        }

        if (n == 0 && lr->len == 0)
            return -1; // EOF, no pending data

        lr->len += (size_t)n;
        nl = memchr(lr->buf, '\n', lr->len);

        if (nl == NULL) {
            if (n == 0)
                nl = lr->buf + lr->len; // EOF in the middle of a line
            else
                return 0; // the line is not complete yet
        }
    }

    size_t line_len = (size_t)(nl - lr->buf);
    size_t copy_len = (line_len < out_cap) ? line_len : out_cap;

    memcpy(out, lr->buf, copy_len);
    out[copy_len] = '\0';

    size_t consumed = line_len + ((nl < lr->buf + lr->len) ? 1 : 0);
    memmove(lr->buf, lr->buf + consumed, lr->len - consumed);
    lr->len -= consumed;

    return 1;
}

void vmnet_peer_run(VmNetChannel *ch, const char *my_label, const char *peer_label) {
    size_t   max_packet = vmnet_channel_max_packet(ch);
    uint8_t *recv_buf    = (uint8_t *)malloc(max_packet);
    char     line[4096];
    LineReader lr;
    int stdin_open = 1;

    memset(&lr, 0, sizeof(lr));//

    if (recv_buf == NULL || max_packet < 1) {
        fprintf(stderr, "%s: buffer allocation failed\n", my_label);
        free(recv_buf);
        return;
    }

    printf("%s: connection with %s active.\n", my_label, peer_label);
    printf("%s: type lines + Enter = send to %s. Ctrl+D = stop sending, "
           "Ctrl+C = quit.\n", my_label, peer_label);
    fflush(stdout);

    for (;;) {
        struct pollfd fds[2];
        nfds_t nfds = 0;
        int stdin_idx = -1;
        int sock_idx;

        if (stdin_open) {
            fds[nfds].fd     = STDIN_FILENO;
            fds[nfds].events = POLLIN;
            stdin_idx = (int)nfds;
            ++nfds;
        }

        fds[nfds].fd     = ch->fd;
        fds[nfds].events = POLLIN;
        sock_idx = (int)nfds;
        ++nfds;

        int ret = poll(fds, nfds, -1);

        if (ret < 0) {
            if (errno == EINTR) {
                if (vmnet_peer_stop) {
                    printf("%s: interrupted by signal, exiting\n", my_label);
                    break;
                }
                continue;
            }
            perror("poll");
            break;
        }

        // receiving from the other side
        if (fds[sock_idx].revents & POLLIN) {
            ssize_t n = vmnet_recv(ch, recv_buf, max_packet);

            if (n == 0) {
                printf("%s: %s disconnected\n", my_label, peer_label);
                break;
            }
            if (n < 0) {
                perror("vmnet_recv");
                break;
            }
            if (n < 1) {
                fprintf(stderr, "%s: empty packet, ignoring\n", my_label);
            } else {
                uint8_t     kind     = recv_buf[0];
                const char *text     = (const char *)(recv_buf + 1);
                size_t      text_len = (size_t)n - 1;

                if (kind == PEER_KIND_TEXT) {
                    printf("%s: received from %s: \"%.*s\"\n",
                           my_label, peer_label, (int)text_len, text);

                    
                    // Automatic echo back - exactly the same
                    // content, just with the type switched to
                    // PEER_KIND_ECHO, so the other side does not
                    // echo it any further.
                    
                    recv_buf[0] = PEER_KIND_ECHO;

                    if (vmnet_send(ch, recv_buf, (size_t)n) < 0) //
                        perror("vmnet_send (echo)");
                    else
                        printf("%s: echo sent back to %s\n", my_label, peer_label);

                } else if (kind == PEER_KIND_ECHO) {
                    printf("%s: echo from %s: \"%.*s\"\n",
                           my_label, peer_label, (int)text_len, text);
                } else {
                    printf("%s: unknown packet type (%u), ignoring\n",
                           my_label, (unsigned)kind);
                }
            }
        }

        if (fds[sock_idx].revents & (POLLHUP | POLLERR)) {
            printf("%s: connection with %s dropped\n", my_label, peer_label);
            break;
        }

        // sending from our stdin
        if (stdin_idx >= 0 && (fds[stdin_idx].revents & (POLLIN | POLLHUP))) {
            int r = line_reader_step(&lr, STDIN_FILENO, line, sizeof(line) - 1);

            if (r < 0) {
                printf("%s: end of input, no longer sending (still receiving)\n",
                       my_label);
                stdin_open = 0;
            } else if (r == 1) {
                size_t text_len = strlen(line);
                size_t max_text = max_packet - 1; // -1 for the kind byte

                if (text_len > max_text) {
                    fprintf(stderr, "%s: line truncated to %zu characters (max_packet limit)\n", my_label, max_text);
                    text_len = max_text;
                }

                uint8_t *send_buf = (uint8_t *)malloc(text_len + 1);

                if (send_buf == NULL) {
                    fprintf(stderr, "%s: allocation failed\n", my_label);
                } else {
                    send_buf[0] = PEER_KIND_TEXT;
                    memcpy(send_buf + 1, line, text_len);

                    if (vmnet_send(ch, send_buf, text_len + 1) < 0)
                        perror("vmnet_send");
                    else
                        printf("%s: sent to %s: \"%.*s\"\n",
                               my_label, peer_label, (int)text_len, line);

                    free(send_buf);
                }
            }
            // r == 0: the line is not complete yet, wait for the next poll
        }
    }

    free(recv_buf);//
}
