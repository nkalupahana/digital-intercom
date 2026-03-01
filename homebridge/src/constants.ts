import type { PlatformConfig } from "homebridge";

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
  DIGITAL_ID = "D",
}

export const CREDIT_CARD_DATA_LEN = 34;
export const HEARTBEAT_INTERVAL = 1000;

export interface DigitalIntercomPlatformConfig extends PlatformConfig {
  allowedCards: {
    hash: string;
    description: string;
  }[];
  allowedDigitalIds: {
    familyName: string;
    givenName: string;
    birthDate: string;
  }[];
}
