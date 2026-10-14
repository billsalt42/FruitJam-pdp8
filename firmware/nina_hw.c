/*
 * nina_hw.c - Fruit Jam wiring of the ESP32-C6 WiFi coprocessor (nina-fw)
 * for net/nina.c: SPI1 at 8 MHz, mode 0.  Pins from the Pico SDK's
 * adafruit_fruit_jam board file.
 */
#include "pico/stdlib.h"
#include "hardware/spi.h"
#include "hardware/gpio.h"
#include "../net/nina.h"

#define NINA_SPI   spi1
#define PIN_MISO   ADAFRUIT_FRUIT_JAM_WIFI_MISO_PIN    /* 28 */
#define PIN_SCK    ADAFRUIT_FRUIT_JAM_WIFI_SCK_PIN     /* 30 */
#define PIN_MOSI   ADAFRUIT_FRUIT_JAM_WIFI_MOSI_PIN    /* 31 */
#define PIN_CS     ADAFRUIT_FRUIT_JAM_WIFI_CS_PIN      /* 46 */
#define PIN_BUSY   ADAFRUIT_FRUIT_JAM_WIFI_ACK_PIN     /* 3: READY/BUSY handshake */
#define PIN_RESET  ADAFRUIT_FRUIT_JAM_WIFI_RESET_PIN   /* 22: shared with the audio DAC */

static bool inited;

static void init(void)
{
    if (inited) return;
    inited = true;
    spi_init(NINA_SPI, 8000000);
    spi_set_format(NINA_SPI, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
    gpio_set_function(PIN_MISO, GPIO_FUNC_SPI);
    gpio_set_function(PIN_SCK, GPIO_FUNC_SPI);
    gpio_set_function(PIN_MOSI, GPIO_FUNC_SPI);
    gpio_init(PIN_CS);
    gpio_put(PIN_CS, 1);
    gpio_set_dir(PIN_CS, GPIO_OUT);
    gpio_init(PIN_BUSY);
    gpio_set_dir(PIN_BUSY, GPIO_IN);
    gpio_init(PIN_RESET);
    gpio_put(PIN_RESET, 1);
    gpio_set_dir(PIN_RESET, GPIO_OUT);
}

void nina_hw_cs(bool asserted) { init(); gpio_put(PIN_CS, !asserted); }
bool nina_hw_busy(void) { init(); return gpio_get(PIN_BUSY); }
void nina_hw_write(const uint8_t *buf, size_t n) { spi_write_blocking(NINA_SPI, buf, n); }
void nina_hw_read(uint8_t *buf, size_t n) { spi_read_blocking(NINA_SPI, 0xFF, buf, n); }
void nina_hw_reset(bool asserted) { init(); gpio_put(PIN_RESET, !asserted); }
uint32_t nina_hw_us(void) { return time_us_32(); }
