// These should be kept in sync with the C++ code
export enum Command {
    OPEN_DOOR = "D",
    LISTEN_ON = "L",
    LISTEN_STOP = "S",
}

export enum IntercomEventType {
    BUZZER = "B",
    CREDIT_CARD = "C",
}

export const CREDIT_CARD_DATA_LEN = 19;