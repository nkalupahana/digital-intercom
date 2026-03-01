#pragma once

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
  DIGITAL_ID = 'D',
};

void connectToTCPServer();
std::optional<Command> getCommand();
void sendBuzzerEvent();
void sendData(std::span<uint8_t> buffer);
