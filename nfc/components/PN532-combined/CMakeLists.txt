set(src
    NDEF/MifareClassic.cpp
    NDEF/Ndef.cpp
    NDEF/NdefRecord.cpp
    NDEF/NfcTag.cpp
    NDEF/MifareUltralight.cpp
    NDEF/NdefMessage.cpp
    NDEF/NfcAdapter.cpp
    PN532/src/emulatetag.cpp
    PN532/src/PN532_HSU.cpp
    PN532/src/PN532_SWHSU.cpp
    PN532/src/llcp.cpp
    PN532/src/PN532_I2C.cpp
    PN532/src/PN532.cpp
    PN532/src/mac_link.cpp
    PN532/src/PN532_SPI.cpp
    PN532/src/snep.cpp
)

idf_component_register(
    SRCS "${src}"
    INCLUDE_DIRS "PN532/src" "NDEF"
    REQUIRES arduino-esp32
)
target_compile_definitions(${COMPONENT_LIB} PUBLIC NFC_INTERFACE_SPI)
