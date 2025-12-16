#include <Arduino.h>
#include <cstring>
#include "port.h"

static uint32_t lastPingMs[MODULE_PORT_ROWS][MODULE_PORT_COLS];
static bool propsRequested[MODULE_PORT_ROWS][MODULE_PORT_COLS];
static bool demoParamSent[MODULE_PORT_ROWS][MODULE_PORT_COLS];
static constexpr uint32_t PING_INTERVAL_MS = 1000;
static constexpr uint32_t DEMO_PARAM_DELAY_MS = 2000;

void setup()
{
  Serial.begin(115200);
  while (!Serial)
  {
    ;
  }

  initPorts();
  Serial.println("piControl: port scan ready");
}

void loop()
{
  scanPorts();

  uint32_t now = millis();

  for (int r = 0; r < MODULE_PORT_ROWS; r++)
  {
    for (int c = 0; c < MODULE_PORT_COLS; c++)
    {
      Port *p = getPort(r, c);
      if (!p || !p->configured)
      {
        continue;
      }

      if (!propsRequested[r][c])
      {
        sendGetProperties(r, c, 0);
        propsRequested[r][c] = true;
      }

      // Periodic ping to keep modules talking
      if (now - lastPingMs[r][c] > PING_INTERVAL_MS)
      {
        sendPing(r, c);
        lastPingMs[r][c] = now;
      }

      if (propsRequested[r][c] && !demoParamSent[r][c] && (now - lastPingMs[r][c] > DEMO_PARAM_DELAY_MS))
      {
        ModuleParameterValue v;
        v.intValue = 64; // demo value
        sendSetParameter(r, c, 0, ModuleParameterDataType::PARAM_TYPE_INT, v);
        demoParamSent[r][c] = true;
      }
    }
  }

  ModuleMessage msg;
  while (getNextMessage(msg))
  {
    Serial.print("[MSG] Port ");
    Serial.print(msg.moduleRow);
    Serial.print(",");
    Serial.print(msg.moduleCol);
    Serial.print(" cmd=");
    Serial.print(static_cast<int>(msg.commandId), HEX);
    Serial.print(" len=");
    Serial.print(msg.payloadLength);
    if (msg.commandId == ModuleMessageId::CMD_RESPONSE && msg.payloadLength >= 3)
    {
      ModuleMessageResponsePayload resp{};
      uint8_t copyLen = msg.payloadLength;
      if (copyLen > sizeof(resp))
      {
        copyLen = sizeof(resp);
      }
      memcpy(&resp, msg.payload, copyLen);
      Serial.print(" status=");
      Serial.print(static_cast<int>(resp.status));
      Serial.print(" inRespTo=");
      Serial.print(static_cast<int>(resp.inResponseTo));
      Serial.print(" payloadBytes=");
      Serial.print(resp.payloadLength);
      Serial.print(" data=");
      for (uint8_t i = 0; i < resp.payloadLength && i < sizeof(resp.payload); i++)
      {
        Serial.print(resp.payload[i], HEX);
        Serial.print(" ");
      }
      Serial.println();
    }
    else
    {
      Serial.print(" data=");
      for (uint8_t i = 0; i < msg.payloadLength; i++)
      {
        Serial.print(msg.payload[i], HEX);
        Serial.print(" ");
      }
      Serial.println();
    }
  }
}