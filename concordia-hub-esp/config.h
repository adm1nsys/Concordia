/*
 * Build-time configuration for the ESP32-C6 companion of the nRF54L15 Matter
 * bridge. Everything worth retuning lives here.
 */
#pragma once

#include <Arduino.h>

/* ---------------------------------------------------------------- branding */
#define BRAND_NAME "Concordia"
#define FW_VERSION "1.0.0"

/* ------------------------------------------------------------ nRF54L15 link
 * D6/D7 carry the silkscreen TX/RX labels but they are UART0, where the ROM
 * bootloader prints on every reset - that would arrive in the nRF shell as
 * garbage. D2/D3 stay quiet.
 *
 *   ESP D2 (GPIO2)  TX --> nRF P2.07 (UART_RX)
 *   ESP D3 (GPIO21) RX <-- nRF P2.08 (UART_TX)
 *   ESP GND            --- nRF GND      <- mandatory common ground
 */
static const int PIN_TX_TO_NRF = D2;
static const int PIN_RX_FROM_NRF = D3;
static const unsigned long NRF_LINK_BAUD = 115200;

/* `bridge types` prints ~60 lines, so an answer needs both room and time.
 * The reply lands in one static buffer - see nrf_link.cpp for why that matters. */
static const size_t NRF_REPLY_MAX_BYTES = 8192;
static const unsigned long NRF_REPLY_TIMEOUT_MS = 4000;
static const unsigned long NRF_REPLY_IDLE_MS = 150;

/* Creating or deleting an endpoint rebuilds Matter's attribute storage and
 * writes the new state to flash. With 100 slots configured that regularly
 * takes longer than the normal timeout, and a `bridge add` that had actually
 * succeeded came back empty - the panel then reported a refusal for a device
 * that was sitting right there in the list. */
static const unsigned long NRF_REPLY_TIMEOUT_SLOW_MS = 15000;

/* ...and the quiet-line timeout has to grow with it. The shell echoes the
 * command back the instant it is sent, so bytes always arrive immediately;
 * what takes seconds is the answer that follows. With the fast idle limit the
 * reader gave up 150 ms in, having collected nothing but the echo, and every
 * successful `bridge add` looked like a refusal. */
static const unsigned long NRF_REPLY_IDLE_SLOW_MS = 2500;

/* --------------------------------------------------------------- user input
 * The ESP32-C6 BOOT button is GPIO9. Held through power-up it wipes this ESP
 * and asks the nRF to clear its Matter pairings too.
 */
static const int PIN_BOOT_BUTTON = 9;
static const unsigned long FACTORY_RESET_HOLD_MS = 3000;
static const int PIN_STATUS_LED = LED_BUILTIN;

/* ----------------------------------------------------------------- network
 * The defaults are used on a blank device (first flash, or after a factory
 * reset) so the bridge joins the house network without anyone opening the
 * portal first. Anything saved later takes precedence.
 */
#define WIFI_DEFAULT_SSID "Meisel_Beckeln27243"
#define WIFI_DEFAULT_PASS "3237323433"

/* Must match CONFIG_BRIDGE_MAX_ENDPOINTS on the nRF. Anything smaller and the
 * extra endpoints exist on the board but never appear in the panel. */
static const uint8_t kMaxDevices = 50;

static const uint8_t MAX_WIFI_NETWORKS = 5;
static const unsigned long WIFI_CONNECT_TIMEOUT_MS = 12000;
static const unsigned long WIFI_RETRY_INTERVAL_MS = 30000;

/* ----------------------------------------------------------------- SD card
 * Where the panel and every configuration file live. The ESP's own flash is the
 * scarce resource here - the interface alone had grown to 95% of it - while the
 * card has gigabytes going spare. So the card holds everything that can grow,
 * and the firmware holds only code.
 *
 * CS is deliberately *not* the variant's default SS. That is GPIO21, which
 * carries the receive line from the nRF; taking it for the card would cut the
 * bridge in half.
 */
static const int PIN_SD_CS = D5;    /* GPIO23 */
static const int PIN_SD_MOSI = D10; /* GPIO18 */
static const int PIN_SD_MISO = D9;  /* GPIO20 */
static const int PIN_SD_SCK = D8;   /* GPIO19 */

/* 20 MHz is comfortable for SPI cards on jumper wires. Cards that misbehave
 * usually want less, not more. */
static const uint32_t SD_SPI_HZ = 20000000;

/* --------------------------------------------------------- external sensors */
static const uint8_t MAX_BINDINGS = 8;
static const uint16_t MIN_POLL_SECONDS = 30;
static const unsigned long HTTP_FETCH_TIMEOUT_MS = 8000;

/* -------------------------------------------------------------------- users
 * Accounts live in /Concordia/security/users.json, not here - these are just
 * the table sizes and the password-hashing cost. */
static const uint8_t MAX_USERS = 12;
static const uint8_t MAX_SESSIONS = 6;
static const uint32_t SESSION_LIFETIME_HOURS_DEFAULT = 12;

/* PBKDF2-HMAC-SHA256 iterations for a stored password. 10000 costs roughly a
 * tenth of a second on this core, which is nothing for a login and everything
 * for an offline guesser working through a stolen file. */
static const uint32_t PBKDF2_ITERATIONS = 10000;

/* ------------------------------------------------------------------ safety
 * A hardware watchdog on the Arduino loop task. The previous firmware could
 * wedge silently - no panic, no reboot, just off the air - and the only cure
 * was pulling the USB cable. Anything that blocks the loop for this long now
 * gets a reset instead of an indefinite hang. It is generous on purpose: one
 * nRF exchange plus one HTTP fetch must fit inside it comfortably.
 */
static const uint32_t LOOP_WATCHDOG_MS = 30000;
