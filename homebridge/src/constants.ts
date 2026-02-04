// These should be kept in sync with the C++ code
export enum Command {
  OPEN_DOOR = "D",
  LISTEN_ON = "L",
  TALK_ON = "T",
  LISTEN_STOP = "S",
  HEARTBEAT = "H",
}

export enum IntercomEventType {
  BUZZER = "B",
  CREDIT_CARD = "C",
}

export const CREDIT_CARD_DATA_LEN = 19;

export const HEARTBEAT_INTERVAL = 1000;
