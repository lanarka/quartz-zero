#ifndef VMNET_PEER_H
#define VMNET_PEER_H

#include "vmnet.h"
#include <signal.h>

/*
  Shared tool for both demo processes (HOST and VM):
 
    - reads lines from stdin and sends them to the other side,
    - when it receives a message from the other side, it prints
      it and AUTOMATICALLY sends it back as an echo,
    - when it receives an already-echoed message (a reply to its
      own previously sent message), it only prints it - it does
      not echo it further (otherwise an infinite ping-pong loop
      would occur between the two sides).
 
  So: I type text in the VM -> HOST prints it and sends an echo
  back -> VM prints the echo. And exactly the same in reverse:
  I type text in the HOST -> VM prints it and sends an echo back
  -> HOST prints the echo.
*/


// Sets up a shared SIGINT/SIGTERM handler that sets
// vmnet_peer_stop to 1. Call once at the start of main().
//
void vmnet_peer_install_signal_handlers(void);


// Set to 1 when SIGINT/SIGTERM is received. vmnet_peer_run()
// exits cleanly upon noticing it; outer loops (e.g. the
// accept-loop on the host) can also check it.
//
extern volatile sig_atomic_t vmnet_peer_stop;


// Starts interactive bidirectional communication on an already
// connected/accepted channel 'ch'. Returns when the other side
// closes the connection, an error occurs, or a signal arrives
// (vmnet_peer_stop).
//
// 'my_label'/'peer_label' are just text for display purposes
// (e.g. "HOST"/"VM").

void vmnet_peer_run(VmNetChannel *ch, const char *my_label, const char *peer_label);

#endif
