// Module oriented logic runs on core 1, in module_task.cpp

#include <Arduino.h>
#include "usb_device.h"
#include "debug_printf.h"

bool core1_separate_stack = true;

void setup()
{
  dbg_printf_init();
  usb::init();
  delay(50);
  dbg_printf("Picontrol: core0 USB ready\n");
}

static uint32_t lastMillis = 0;
void loop()
{
  usb::task();
  uint32_t now = millis();

  if (now - lastMillis >= 1000)
  {
    lastMillis = now;
    dbg_printf("Core0 alive %lu ms \n", lastMillis);
  }
}
