/*
 * pidp_proto.h - the link between the emulator and the PiDP-8/I panel
 * interface (a Raspberry Pi Pico or Pico 2 wired to the PiDP-8/I's 40-pin
 * header, connected to the Fruit Jam by USB as a CDC serial device).
 *
 * Every message starts with a byte that has bit 7 set; all following bytes
 * of the message are 0x00-0x7F, so a receiver can always resynchronise.
 *
 * Emulator -> panel
 *   PIDP_LAMPS   96 bytes: brightness 0..31 of each lamp, rows 0-7, and in
 *                each row columns 0-11 (the PiDP-8/I's own matrix order:
 *                column 0 is bit 0, the rightmost lamp, of a register row).
 *
 * Panel -> emulator
 *   PIDP_HELLO   5 bytes: 'P','8', protocol version, firmware major, minor.
 *                Sent when the port is opened and once a second while no
 *                lamp frames arrive.
 *   PIDP_SWITCH  6 bytes: switch rows 0-2 as two 6-bit halves each (low
 *                half first); 1 = switch closed (up / pressed).  Sent on
 *                every debounced change and every 100 ms.
 *
 * Matrix, as wired on the PiDP-8/I PCB (see the pidp8i software):
 *   lamp row 0 PC, 1 MA, 2 MB, 3 AC, 4 MQ,
 *            5 instruction type (col 11 AND .. col 4 OPR), FETCH 3, EXEC 2, DEFER 1
 *            6 RUN 7, PAUSE 8, ION 9, step counter in cols 2-6
 *            7 LINK 5, IF in cols 6-8, DF in cols 9-11
 *   switch row 0 switch register (col = bit)
 *              1 DF (cols 9-11), IF (cols 6-8)
 *              2 START 11, LOAD ADD 10, DEP 9, EXAM 8, CONT 7, STOP 6,
 *                SING STEP 5, SING INST 4
 */
#ifndef PIDP_PROTO_H
#define PIDP_PROTO_H

#define PIDP_PROTO_VERSION 1

#define PIDP_LAMPS  0x81
#define PIDP_HELLO  0x90
#define PIDP_SWITCH 0x91

#define PIDP_ROWS      8
#define PIDP_COLS      12
#define PIDP_SWROWS    3
#define PIDP_LEVELS    32              /* brightness 0..31 */

#define PIDP_LAMPS_LEN  (PIDP_ROWS * PIDP_COLS)
#define PIDP_HELLO_LEN  5
#define PIDP_SWITCH_LEN 6

/* switch row 2 */
#define PIDP_SW_START  04000
#define PIDP_SW_LOAD   02000
#define PIDP_SW_DEP    01000
#define PIDP_SW_EXAM   00400
#define PIDP_SW_CONT   00200
#define PIDP_SW_STOP   00100
#define PIDP_SW_SSTEP  00040
#define PIDP_SW_SINST  00020

#endif
