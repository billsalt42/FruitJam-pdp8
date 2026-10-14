#ifndef PIDP_H
#define PIDP_H
/*
 * PiDP-8/I hardware front panel, driven through a Pico/Pico 2 "panel
 * interface" on the USB host port (see ../pidp8-panel).  This module is
 * hardware-independent: the caller moves bytes to and from the USB link.
 */
#include <stdint.h>
#include <stdbool.h>
#include "../core/pdp8.h"
#include "../pidp8-panel/pidp_proto.h"

#define PIDP_MSG_MAX (1 + PIDP_LAMPS_LEN)

void pidp_link(bool up);                        /* panel interface plugged in / removed */
bool pidp_present(void);                        /* link up and the panel has said hello */
void pidp_rx(const uint8_t *buf, uint32_t n);   /* bytes received from the panel */

/* Act on switch changes, and when l is a fresh lamp sample (or NULL while
 * nothing new was sampled) build a lamp message into out.  Returns the
 * number of bytes to send, 0 for none. */
int  pidp_update(uint32_t now_ms, const pdp8_lamps_t *l, uint8_t *out);

uint32_t pidp_switches(int row);                /* debounced switch row, 1 = closed */

#endif
