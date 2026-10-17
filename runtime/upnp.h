/* Opening a port on the home router for the host of a session (UPnP). */
#pragma once
#include <stddef.h>

/* 1: open, ext_ip = the public address; 2: open but the public address is
 * itself private (carrier-grade NAT); 0: failed (why says why). lan_ip =
 * this PC on the home network. Blocks for up to a few seconds. */
int upnp_open(int port, char* ext_ip, size_t en, char* lan_ip, size_t ln, char* why, size_t wn);
void upnp_close(void);                 /* take the forwarding away again */
