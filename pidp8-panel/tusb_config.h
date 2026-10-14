#ifndef _TUSB_CONFIG_H_
#define _TUSB_CONFIG_H_
/* PiDP-8/I panel interface: one CDC serial port on the Pico's USB. */
#define CFG_TUSB_OS               OPT_OS_PICO
#define CFG_TUD_ENABLED           1
#define BOARD_TUD_RHPORT          0
#ifndef CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_SECTION
#endif
#ifndef CFG_TUSB_MEM_ALIGN
#define CFG_TUSB_MEM_ALIGN        __attribute__ ((aligned(4)))
#endif
#define CFG_TUD_ENDPOINT0_SIZE    64
#define CFG_TUD_CDC               1
#define CFG_TUD_CDC_RX_BUFSIZE    512
#define CFG_TUD_CDC_TX_BUFSIZE    256
#define CFG_TUD_CDC_EP_BUFSIZE    64
#endif
