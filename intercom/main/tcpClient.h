#pragma once

#include <optional>

enum class Command {
  OPEN_DOOR = 'D',
  LISTEN_ON = 'L',
  LISTEN_STOP = 'S',
  RESET = 'R', // Internal only command. Not sent by the TCP server
};

void connectToTCPServer();
std::optional<Command> getCommand();