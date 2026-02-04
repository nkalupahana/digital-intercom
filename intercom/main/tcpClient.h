#pragma once

#include "../../constants.h"

#include <cstdint>
#include <optional>
#include <span>

enum class Command {
  OPEN_DOOR = 'D',
  LISTEN_ON = 'L',
  TALK_ON = 'T',
  LISTEN_STOP = 'S',
  HEARTBEAT = 'H',
  RESET = 'R', // Internal only command. Not sent by the TCP server
};

enum class OutputEvent {
  BUZZER = 'B',
  CREDIT_CARD = 'C',
};

void connectToTCPServer();
std::optional<Command> getCommand();
void sendBuzzerEvent();
void sendCreditCardEvent(std::span<uint8_t, CREDIT_CARD_DATA_LEN> data);
