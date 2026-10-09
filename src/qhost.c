#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include "vmnet.h"
#include "vmnet_peer.h"


/*
 qhost - HOST-side counterpart process for the Quartz VM's network card
 (see the OP_NET_* opcodes in src/vm.c and the "sietovka" library in
 src-net/vmnet.h).
 
    qhost [socket_path] [max_packet]
 
  Neither the socket path nor the max packet size is hardcoded - both can
  be overridden at process startup (the defaults VMNET_DEFAULT_SOCKET_PATH
  / VMNET_DEFAULT_MAX_PACKET are used if not given).
 
  qhost runs an infinite outer loop:
    - waits for a VM to connect (accept),
    - while a VM is connected, runs the shared bidirectional/interactive
      line protocol (vmnet_peer_run): whatever you type in qhost is sent to
      the VM, which echoes it straight back; and symmetrically, whatever
      arrives from the VM is printed here and automatically echoed back to
      it,
    - once the VM disconnects, qhost goes back to waiting for the next
      connection instead of exiting.
 
  This is primarily a manual testing tool (kept simple on purpose, as a
  counterpart for the real VM to talk to during development) rather than a
  fixed protocol the real VM binary has to follow - any test/demo program
  driving the VM's OP_NET_SEND/OP_NET_RECV opcodes is free to frame its own
  packets however it likes.
 
  Exit: Ctrl+C / SIGTERM.
*/

int main(int argc, char **argv)
{
    const char *socket_path = (argc > 1) ? argv[1] : VMNET_DEFAULT_SOCKET_PATH;
    size_t max_packet = VMNET_DEFAULT_MAX_PACKET;

    if (argc > 2)
        max_packet = (size_t)strtoul(argv[2], NULL, 10);

    vmnet_peer_install_signal_handlers();

    int server_fd = vmnet_listen(socket_path);

    if (server_fd < 0) {
        perror("vmnet_listen");
        return 1;
    }

    printf("HOST: listening on %s (max_packet=%zu)\n", socket_path, max_packet);
    printf("HOST: Ctrl+C to quit\n");
    fflush(stdout);

    while (!vmnet_peer_stop) {
        VmNetChannel client;

        if (vmnet_channel_init(&client, max_packet) < 0) {
            perror("vmnet_channel_init");
            break;
        }

        printf("HOST: waiting for a VM to connect...\n");
        fflush(stdout);

        if (vmnet_accept(server_fd, &client) < 0) {
            vmnet_channel_close(&client);
            if (vmnet_peer_stop || errno == EINTR)
                break;
            perror("vmnet_accept");
            continue;
        }

        printf("HOST: VM connected\n");
        fflush(stdout);

        vmnet_peer_run(&client, "HOST", "VM");

        vmnet_channel_close(&client);
    }

    close(server_fd);
    unlink(socket_path);

    printf("HOST: shut down\n");
    return 0;
}
