#include "Radio.h"
#include "../../../constants.h"
#include "errors.h"
#include "utils.h"
#include <RHReliableDatagram.h>
#include <RH_RF69.h>

namespace Radio {

RH_RF69 driver(15, 26);
RHReliableDatagram manager(driver, RADIO_SCANNER_ADDRESS);
constexpr int RADIO_RESET_PIN = 16;

bool setup() {
  pinMode(RADIO_RESET_PIN, OUTPUT);
  digitalWrite(RADIO_RESET_PIN, LOW);
  delay(100);
  digitalWrite(RADIO_RESET_PIN, HIGH);
  delay(100);
  digitalWrite(RADIO_RESET_PIN, LOW);
  delay(100);

  bool radioInitialized = manager.init();
  while (!radioInitialized) {
    ESP_LOGI(TAG, "Waiting for radio to initialize...");
    radioInitialized = manager.init();
    delay(1000);
  }

  driver.setTxPower(RADIO_POWER, true);
  driver.setFrequency(RADIO_FREQUENCY);
  CHECK_PRINT_RETURN_BOOL("Failed to set modem config",
                          driver.setModemConfig(RH_RF69::FSK_Rb2Fd5));

  return true;
}

bool send(const std::span<uint8_t> data) {
  return manager.sendtoWait(data.data(), data.size(), RADIO_INTERCOM_ADDRESS);
}

} // namespace Radio