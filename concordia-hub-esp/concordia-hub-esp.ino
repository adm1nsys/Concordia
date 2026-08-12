/*
 * XIAO ESP32-C6 - companion controller for the XIAO nRF54L15 Matter bridge.
 *
 * Three ways to drive it, one command set behind all of them:
 *   - USB console  : open a serial monitor at 115200 and type `help`
 *   - web panel    : http://<ip>/  (captive portal when Wi-Fi is unavailable)
 *   - the link     : every command ends up as a `bridge ...` shell line on the
 *                    node
 *
 * Roles rather than part numbers in anything a user reads: this board is the
 * **hub**, the Matter board is the **node**, and the UART between them is the
 * **link**. Part numbers survive only where they are facts about hardware -
 * the wiring below, and the board to select in the IDE.
 *
 * ---------------------------------------------------------------------------
 * Wiring (both boards are 3V3, no level shifter needed):
 *
 *   ESP D2  (GPIO2)  TX --->  nRF P2.07  (UART_RX)
 *   ESP D3  (GPIO21) RX <---  nRF P2.08  (UART_TX)
 *   ESP GND             ----  nRF GND      <- mandatory common ground
 *
 * Power each board from its own USB port; do not bridge 5V/3V3 between them.
 *
 * Arduino IDE:
 *   Board:                            XIAO_ESP32C6
 *   USB CDC On Boot:                  Enabled
 *   Erase All Flash Before Upload:    Disabled   <- keeps Wi-Fi settings
 *   Monitor speed:                    115200
 *
 * Holding BOOT while powering up wipes this hub and asks the node to clear its
 * Matter pairings too.
 * ---------------------------------------------------------------------------
 */

#include <esp_task_wdt.h>

#include "commands.h"
#include "config.h"
#include "console.h"
#include "ext_api.h"
#include "log_buf.h"
#include "net_manager.h"
#include "nrf_link.h"
#include "sd_card.h"
#include "storage.h"
#include "users.h"
#include "warnings.h"
#include "web_ui.h"

namespace {

/* Blink pattern tells the state without opening anything:
 * fast = setup portal, slow blip = online, steady off = still booting. */
void updateStatusLed() {
  static unsigned long last = 0;
  static bool on = false;

  const unsigned long period = Net::mode() == NET_PORTAL ? 400 : 2000;
  if (millis() - last < (on ? 80 : period)) {
    return;
  }
  last = millis();
  on = !on;
  digitalWrite(PIN_STATUS_LED, on ? HIGH : LOW);
}

/* BOOT held through power-up: wipe both boards. Checked before anything else
 * touches storage, so a corrupt setting can never lock the user out. */
void checkFactoryResetButton() {
  pinMode(PIN_BOOT_BUTTON, INPUT_PULLUP);
  delay(20);
  if (digitalRead(PIN_BOOT_BUTTON) != LOW) {
    return;
  }

  Serial.println("[sys] BOOT held - keep holding to factory reset...");
  const unsigned long start = millis();
  while (digitalRead(PIN_BOOT_BUTTON) == LOW) {
    if (millis() - start >= FACTORY_RESET_HOLD_MS) {
      Serial.println("[sys] wiping this hub");
      Storage::begin();
      Storage::factoryReset();

      /* Card::begin() would otherwise run later in setup() - safe to do it
       * early here because the branch always ends in a restart, so setup()
       * never runs a second time on top of it. Needed to reach users.json,
       * which is on the card, not in NVS: this is the "forgot every
       * password" recovery path. */
      Card::begin();
      Users::factoryReset();

      Serial.println("[sys] asking the node to clear its Matter pairings");
      NrfLink::begin();
      NrfLink::factoryReset();

      for (int i = 0; i < 20; ++i) { /* obvious fast blink */
        digitalWrite(PIN_STATUS_LED, i % 2);
        delay(80);
      }
      Serial.println("[sys] done, restarting");
      delay(200);
      ESP.restart();
    }
    delay(50);
  }
  Serial.println("[sys] BOOT released early - nothing was reset");
}

/* A hang used to mean the board simply vanished: no panic, no reboot, off the
 * air until someone pulled the cable. Now the loop has to check in. */
void startWatchdog() {
  esp_task_wdt_config_t cfg = {};
  cfg.timeout_ms = LOOP_WATCHDOG_MS;
  cfg.idle_core_mask = 0;
  cfg.trigger_panic = true;

  /* The Arduino core usually initialises the task WDT already, in which case
   * init() refuses and reconfigure() is the way in. */
  if (esp_task_wdt_init(&cfg) == ESP_ERR_INVALID_STATE) {
    esp_task_wdt_reconfigure(&cfg);
  }
  esp_task_wdt_add(nullptr);
}

} // namespace

void setup() {
  pinMode(PIN_STATUS_LED, OUTPUT);
  digitalWrite(PIN_STATUS_LED, LOW);

  Serial.begin(115200);
  const unsigned long deadline = millis() + 1500;
  while (!Serial && (long)(millis() - deadline) < 0) {
    delay(10);
  }
  Log::begin();

  Log::add("=== " BRAND_NAME " companion v" FW_VERSION " ===");

  checkFactoryResetButton();

  Storage::begin();
  Card::begin();
  Log::loadConfig(); /* only now: the card has to be up to hold it */
  Users::begin();
  Warn::begin();
  NrfLink::begin();
  Log::addf("[link] on TX=GPIO%d RX=GPIO%d @ %lu", PIN_TX_TO_NRF, PIN_RX_FROM_NRF,
            NRF_LINK_BAUD);
  Log::addf("[link] node %s", NrfLink::online() ? "responding" : "silent");
  /* Earliest reasonable point to ask - the node holds BLE advertising off
   * until told to start (see nRF54l15_Bridge/app_task.cpp), so the line is
   * at its cleanest right here, before Wi-Fi or anything else has had a
   * chance to run. A silent node here just means recover()'s retries pick
   * it up later - see NrfLink::recover(). */
  NrfLink::refreshPairingCache();

  Net::begin();
  WebUi::begin();
  if (Net::mode() == NET_PORTAL) {
    Log::addf("[web] join \"%s\" to set Wi-Fi up", Net::apSsid().c_str());
  }

  startWatchdog();
  Console::begin();
}

void loop() {
  esp_task_wdt_reset();

  Net::loop();
  WebUi::loop();
  if (NrfLink::linkSuspect()) {
    NrfLink::recover();
  }
  Console::loop();
  ExtApi::loop(Net::mode() == NET_ONLINE);
  Warn::loop(Net::mode() == NET_ONLINE);
  updateStatusLed();
}
