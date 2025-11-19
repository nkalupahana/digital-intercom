#include "Arduino.h"

void setup() {
  Serial.begin(115200);
  Serial.println("Hello, Digital Intercom!");
}

void loop() {
  // Your main code here
  Serial.println("Test");
  delay(1000);
}