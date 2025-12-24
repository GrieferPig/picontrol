// Module oriented logic runs on core 1, in module_task.cpp

#include <Arduino.h>
#include "usb_device.h"

bool core1_separate_stack = true;

void setup()
{
  Serial.begin(115200);
  usb::init();
  delay(50);
  UsbSerial.println("piControl: core0 USB ready");
}

void loop()
{
  // Serial.println("core0 loop");
  usb::task();
  delay(1);
}