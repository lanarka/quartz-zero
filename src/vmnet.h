#ifndef VMNET_H
#define VMNET_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define VMNET_VERSION    1
#define VMNET_MSG_PACKET 1


// Default values. These are not hard limits of the library -
// they are just defaults that you can override with your own
// value when initializing the channel (see vmnet_channel_init()).

#define VMNET_DEFAULT_MAX_PACKET 65535u
#define VMNET_MIN_MAX_PACKET     64u
#define VMNET_DEFAULT_SOCKET_PATH "/tmp/quartz.sock"


// Header of a single frame on the wire.
 typedef struct {
    uint32_t version;
    uint32_t type;
    uint32_t length;
} VmNetHeader;


// VmNetChannel represents a single connection (VM<->HOST).
//
// max_packet is set dynamically during channel initialization
// (vmnet_channel_init) and is used to derive the size of the
// internal receive buffer, which is allocated on the heap - it
// is not a static array with a fixed size.
typedef struct {
    int      fd;
    size_t   max_packet;
    uint8_t *rx_buf;      // sizeof(VmNetHeader) + max_packet
    size_t   rx_buf_size;
} VmNetChannel;

// Initializes the channel and allocates internal buffers for the
// given max packet size. The channel does not yet have an assigned
// fd (fd = -1); that happens later via connect/accept below.
//
// max_packet == 0 means "use VMNET_DEFAULT_MAX_PACKET".
//
// Returns 0 on success, -1 on error (errno set).
int vmnet_channel_init(VmNetChannel *ch, size_t max_packet);


// Closes the fd (if open) and frees internal buffers.
// After calling this, the channel can be reused via
// vmnet_channel_init().
void vmnet_channel_close(VmNetChannel *ch);


// Returns the max packet size the channel is configured for.
size_t vmnet_channel_max_packet(const VmNetChannel *ch);


// Connects from the VM process to the HOST process via a UNIX
// socket at 'path'. The channel 'ch' must be pre-initialized via
// vmnet_channel_init().
//
// Returns 0 on success, -1 on error.
int vmnet_connect(VmNetChannel *ch, const char *path);

// Creates a listening UNIX socket in the HOST process at 'path'.
// The path is provided here, at call time - it is not hardcoded
// in the library.
//
// Returns the listening socket descriptor on success, -1 on error.
int vmnet_listen(const char *path);


// Accepts an incoming VM connection on the listening socket
// 'server_fd' and populates the channel 'ch' from it (which must
// be pre-initialized via vmnet_channel_init() with the desired
// max packet size).
//
// This call blocks until a connection arrives.
//
// Returns 0 on success, -1 on error.
int vmnet_accept(int server_fd, VmNetChannel *ch);


// Sends a single packet over the channel.
//
// Returns:
//   0  success
//  -1  error (errno set, e.g. EMSGSIZE if len exceeds the
//      channel's max_packet)
int vmnet_send(VmNetChannel *ch, const void *data, size_t len);


// Receives a single packet from the channel (blocking call).
//
// Returns:
//   >0 number of bytes of data received
//    0 the other side closed the connection
//   -1 error (errno set)
 ssize_t vmnet_recv(VmNetChannel *ch, void *data, size_t capacity);


// Helper function for bidirectional/non-blocking communication:
// waits at most 'timeout_ms' milliseconds (or forever if -1)
// until there is something to read on the channel.
//
// Returns:
//   1  data is ready to read (vmnet_recv will not block)
//   0  timeout, nothing ready
//  -1  error
int vmnet_poll_readable(VmNetChannel *ch, int timeout_ms);

#endif
