#pragma once
#include "WiFiUdp.h"

constexpr int DAC_BCLK_PIN = 14;
constexpr int DAC_WS_PIN = 15;
constexpr int DAC_DOUT_PIN = 27;
constexpr int DAC_RESET_PIN = 17;
constexpr int DAC_SAMPLE_RATE = 16000;
constexpr int TALK_RELAY_PIN = 32;
constexpr int AUDIO_IN_PORT = 9997;

void setupTalk(WiFiUDP& talkUdp);
void writeAudioSamples(uint8_t* audioBuffer, size_t packetSize);