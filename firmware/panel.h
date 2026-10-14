#ifndef PANEL_H
#define PANEL_H
#include <stdint.h>
#include <stdbool.h>
#include "term.h"
#include "../core/pdp8.h"

/* PDP-8/E style front panel drawn on its own virtual screen. */

#define PANEL_KEY_UP    0x200     /* cursor keys, decoded by the caller */
#define PANEL_KEY_DOWN  0x201

term_t *panel_screen(void);
void panel_init(void);
void panel_set_system(const char *name);      /* shown in the header */
void panel_show(bool visible);
/* call often; lamps = a fresh sample (~40-50 a second) or NULL, redraws when given one */
void panel_update(uint32_t now_ms, const pdp8_lamps_t *lamps);
bool panel_key(int k);                        /* false = leave the panel */

#endif
