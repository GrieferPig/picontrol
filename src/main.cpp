// Module oriented logic runs on core 1, in module_task.cpp

#include <Arduino.h>
#include "usb_device.h"

bool core1_separate_stack = true;

void setup()
{
  usb::init();
  delay(50);
  UsbSerial.println(F("piControl: core0 USB ready"));
}

void loop()
{
  usb::task();
  delay(1);
}