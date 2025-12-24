/**
 * @file client.c
 * @brief Module client protocol implementation
 *
 * Pure C implementation with C++ compatibility.
 * Uses HAL abstraction for platform-specific operations.
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "client_hal.h"
#include "ch32fun.h"
#include <stdio.h>

#include "ws2812_driver.h"
#include <stdbool.h>

/*******************************************************************************
 * Protocol Constants
 ******************************************************************************/

// reduce max payload 2048 -> 512 to save RAM on small MCU
#define MODULE_MAX_PAYLOAD 512
#define FRAME_START 0xAA
#define PARSER_TIMEOUT_MS 50

/*******************************************************************************
 * Protocol Types
 ******************************************************************************/

typedef enum
{
    CMD_PING = 0x00,
    CMD_GET_PROPERTIES = 0x01,
    CMD_SET_PARAMETER = 0x02,
    CMD_GET_PARAMETER = 0x03,
    CMD_RESET_MODULE = 0x04,
    CMD_SET_AUTOUPDATE = 0x05,
    CMD_RESPONSE = 0x80
} ModuleMessageId;

typedef enum
{
    MODULE_STATUS_OK = 0,
    MODULE_STATUS_ERROR = 1,
    MODULE_STATUS_UNSUPPORTED = 2
} ModuleStatus;

typedef enum
{
    PARAM_TYPE_INT = 0,
    PARAM_TYPE_FLOAT = 1,
    PARAM_TYPE_BOOL = 2
} ModuleParameterDataType;

typedef enum
{
    ACCESS_READ = 1 << 0,
    ACCESS_WRITE = 1 << 1,
    ACCESS_READ_WRITE = (1 << 0) | (1 << 1)
} ModuleParameterAccess;

typedef enum
{
    PROTOCOL_UART = 0
} ModuleProtocol;

typedef enum
{
    FADER = 0,
    KNOB = 1,
    BUTTON = 2,
    BUTTON_MATRIX = 3,
    ENCODER = 4,
    JOYSTICK = 5,
    PROXIMITY = 6
} ModuleType;

/*******************************************************************************
 * Packed Payload Structures
 ******************************************************************************/

#define PACKED_STRUCT __attribute__((packed))
typedef union PACKED_STRUCT
{
    int32_t intValue;
    float floatValue;
    uint8_t boolValue;
} ModuleParameterValue;

typedef union PACKED_STRUCT
{
    struct
    {
        int32_t intMin;
        int32_t intMax;
    } intRange;
    struct
    {
        float floatMin;
        float floatMax;
    } floatRange;
} ModuleParameterMinMax;

typedef struct PACKED_STRUCT
{
    uint8_t id;
    char name[32];
    uint8_t dataType; /* ModuleParameterDataType */
    uint8_t access;   /* ModuleParameterAccess */
    ModuleParameterValue value;
    ModuleParameterMinMax minMax;
} ModuleParameter;

typedef struct PACKED_STRUCT
{
    uint8_t protocol; /* ModuleProtocol */
    uint8_t type;     /* ModuleType */
    char name[32];
    char manufacturer[32];
    char fwVersion[16];
    uint8_t compatibleHostVersion;
    uint8_t capabilities;
    uint8_t physicalSizeRow;
    uint8_t physicalSizeCol;
    uint8_t portLocationRow;
    uint8_t portLocationCol;
    uint8_t parameterCount;
    ModuleParameter parameters[8];
} Module;

typedef struct PACKED_STRUCT
{
    uint8_t status;       /* ModuleStatus */
    uint8_t inResponseTo; /* ModuleMessageId */
    uint16_t payloadLength;
    uint8_t payload[MODULE_MAX_PAYLOAD - 4];
} ModuleMessageResponsePayload;

typedef struct PACKED_STRUCT
{
    uint8_t parameterId;
    uint8_t dataType; /* ModuleParameterDataType */
    union
    {
        int32_t intValue;
        float floatValue;
        uint8_t boolValue;
    } value;
} ModuleMessageSetParameterPayload;

typedef struct PACKED_STRUCT
{
    uint8_t parameterId;
} ModuleMessageGetParameterPayload;

typedef struct PACKED_STRUCT
{
    uint8_t requestId;
    Module module;
} ModuleMessageGetPropertiesPayload;

typedef struct PACKED_STRUCT
{
    uint8_t magic; /* 0x55 */
} ModuleMessagePingPayload;

typedef struct PACKED_STRUCT
{
    uint8_t magic; /* 0xA5 */
} ModuleMessageResetPayload;

typedef struct PACKED_STRUCT
{
    uint8_t enable;      /* 0/1 */
    uint16_t intervalMs; /* 0 = on change only */
} ModuleMessageSetAutoupdatePayload;

/*******************************************************************************
 * Module State
 ******************************************************************************/

static uint8_t g_btn_state = 0;  /* Param 1: Button (Bool) */
static int32_t g_led_r = 0;      /* Param 2: Red (Int) */
static int32_t g_led_g = 0;      /* Param 3: Green (Int) */
static int32_t g_led_b = 0;      /* Param 4: Blue (Int) */
static uint8_t g_led_status = 0; /* Param 5: LED Status (Bool) */

static int g_autoupdate_enabled = 0;
static uint16_t g_autoupdate_interval_ms = 0;

static uint8_t g_last_sent_btn_state = 0;
static uint32_t g_last_periodic_sent_ms = 0;

/*******************************************************************************
 * Frame Parser State
 ******************************************************************************/

static uint8_t g_rx_buf[MODULE_MAX_PAYLOAD + 5];
static uint16_t g_rx_len = 0;
static uint16_t g_rx_expected = 0;
static uint32_t g_last_byte_ms = 0;

/*******************************************************************************
 * Utility Functions
 ******************************************************************************/

static uint8_t calc_checksum(const uint8_t *data, size_t len)
{
    uint16_t sum = 0;
    size_t i;
    for (i = 0; i < len; i++)
    {
        sum += data[i];
    }
    return (uint8_t)(sum & 0xFF);
}

static void safe_strncpy(char *dest, const char *src, size_t dest_size)
{
    if (dest_size == 0)
        return;
    size_t i;
    for (i = 0; i < dest_size - 1 && src[i] != '\0'; i++)
    {
        dest[i] = src[i];
    }
    dest[i] = '\0';
}

/*******************************************************************************
 * Frame Transmission
 ******************************************************************************/

static void send_frame(uint8_t cmd, const uint8_t *payload, uint16_t len)
{
    uint8_t hdr[4];
    uint8_t checksum;
    uint16_t i;

    hal_debug_printf("[TX] Sending frame: cmd=0x%02X, len=%u\n", cmd, len);

    if (len > MODULE_MAX_PAYLOAD)
    {
        hal_debug_printf("[TX-WARN] Payload truncated from %u to %u bytes\n",
                         len, MODULE_MAX_PAYLOAD);
        len = MODULE_MAX_PAYLOAD;
    }

    hdr[0] = FRAME_START;
    hdr[1] = cmd;
    hdr[2] = (uint8_t)(len & 0xFF);
    hdr[3] = (uint8_t)((len >> 8) & 0xFF);

    checksum = calc_checksum(hdr, 4);
    for (i = 0; i < len; i++)
    {
        checksum = (uint8_t)(checksum + payload[i]);
    }

    hal_uart_write(hdr, 4);
    if (len > 0)
    {
        hal_uart_write(payload, len);
    }
    hal_uart_write_byte(checksum);

    hal_debug_printf("[TX] Frame sent successfully, checksum=0x%02X\n", checksum);
}

/*******************************************************************************
 * Response Helper
 ******************************************************************************/

static void respond_status(uint8_t in_resp_to, uint8_t status,
                           const uint8_t *data, uint16_t data_len)
{
    static uint8_t buf[4 + MODULE_MAX_PAYLOAD];
    uint16_t copy_len;

    hal_debug_printf("[RESP] Responding to cmd=0x%02X with status=%u, dataLen=%u\n",
                     in_resp_to, status, data_len);

    copy_len = data_len;
    if (copy_len > (uint16_t)(MODULE_MAX_PAYLOAD - 4))
    {
        hal_debug_printf("[RESP-WARN] Response payload truncated from %u to %zu bytes\n",
                         copy_len, (size_t)(MODULE_MAX_PAYLOAD - 4));
        copy_len = (uint16_t)(MODULE_MAX_PAYLOAD - 4);
    }

    /* Build ModuleMessageResponsePayload in-place to avoid large stack usage */
    buf[0] = status;
    buf[1] = in_resp_to;
    buf[2] = (uint8_t)(copy_len & 0xFF);
    buf[3] = (uint8_t)((copy_len >> 8) & 0xFF);

    if (copy_len > 0 && data != NULL)
    {
        memcpy(&buf[4], data, copy_len);
    }

    send_frame(CMD_RESPONSE, buf, (uint16_t)(4 + copy_len));
}

/*******************************************************************************
 * Auto-update Helper
 ******************************************************************************/

static void send_param_update_if_needed(int force)
{
    uint32_t now;
    int changed;
    int periodic;
    uint8_t buf[2];

    if (!g_autoupdate_enabled)
    {
        return;
    }

    now = hal_millis();
    changed = (g_btn_state != g_last_sent_btn_state);
    periodic = (g_autoupdate_interval_ms > 0) &&
               ((now - g_last_periodic_sent_ms) >= g_autoupdate_interval_ms);

    if (!force && !changed && !periodic)
    {
        return;
    }

    /* Send Button Update (Param 0) */
    buf[0] = 0;
    buf[1] = g_btn_state;
    respond_status(CMD_GET_PARAMETER, MODULE_STATUS_OK, buf, 2);

    g_last_sent_btn_state = g_btn_state;
    g_last_periodic_sent_ms = now;
}

/*******************************************************************************
 * Command Handlers
 ******************************************************************************/

static void handle_ping(void)
{
    ModuleMessagePingPayload p;

    hal_debug_print("[CMD] Handling PING command\n");

    p.magic = 0x55;
    respond_status(CMD_PING, MODULE_STATUS_OK, (const uint8_t *)&p, sizeof(p));
}

static void handle_get_properties(uint8_t request_id)
{
    /* Large payload: keep off the stack to avoid overflow on small MCUs */
    static ModuleMessageGetPropertiesPayload props;

    hal_debug_printf("[CMD] Handling GET_PROPERTIES command, requestId=%u\n", request_id);

    memset(&props, 0, sizeof(props));

    props.requestId = request_id;
    props.module.protocol = PROTOCOL_UART;
    props.module.type = FADER;
    safe_strncpy(props.module.name, "RGB Module", sizeof(props.module.name));
    safe_strncpy(props.module.manufacturer, "DemoCo", sizeof(props.module.manufacturer));
    safe_strncpy(props.module.fwVersion, "1.1.0", sizeof(props.module.fwVersion));
    props.module.compatibleHostVersion = 1;
    props.module.capabilities = 0x01; /* MODULE_CAP_AUTOUPDATE */
    props.module.physicalSizeRow = 1;
    props.module.physicalSizeCol = 1;
    props.module.portLocationRow = 0;
    props.module.portLocationCol = 0;

    props.module.parameterCount = 5;

    // Param 1: Button
    props.module.parameters[0].id = 0;
    safe_strncpy(props.module.parameters[0].name, "Button", sizeof(props.module.parameters[0].name));
    props.module.parameters[0].dataType = PARAM_TYPE_BOOL;
    props.module.parameters[0].access = ACCESS_READ;
    props.module.parameters[0].value.boolValue = g_btn_state;

    // Param 2: Red
    props.module.parameters[1].id = 1;
    safe_strncpy(props.module.parameters[1].name, "Red", sizeof(props.module.parameters[1].name));
    props.module.parameters[1].dataType = PARAM_TYPE_INT;
    props.module.parameters[1].access = ACCESS_WRITE;
    props.module.parameters[1].value.intValue = g_led_r;
    props.module.parameters[1].minMax.intRange.intMin = 0;
    props.module.parameters[1].minMax.intRange.intMax = 255;

    // Param 3: Green
    props.module.parameters[2].id = 2;
    safe_strncpy(props.module.parameters[2].name, "Green", sizeof(props.module.parameters[2].name));
    props.module.parameters[2].dataType = PARAM_TYPE_INT;
    props.module.parameters[2].access = ACCESS_WRITE;
    props.module.parameters[2].value.intValue = g_led_g;
    props.module.parameters[2].minMax.intRange.intMin = 0;
    props.module.parameters[2].minMax.intRange.intMax = 255;

    // Param 4: Blue
    props.module.parameters[3].id = 3;
    safe_strncpy(props.module.parameters[3].name, "Blue", sizeof(props.module.parameters[3].name));
    props.module.parameters[3].dataType = PARAM_TYPE_INT;
    props.module.parameters[3].access = ACCESS_WRITE;
    props.module.parameters[3].value.intValue = g_led_b;
    props.module.parameters[3].minMax.intRange.intMin = 0;
    props.module.parameters[3].minMax.intRange.intMax = 255;

    // Param 5: Status
    props.module.parameters[4].id = 4;
    safe_strncpy(props.module.parameters[4].name, "Status", sizeof(props.module.parameters[4].name));
    props.module.parameters[4].dataType = PARAM_TYPE_BOOL;
    props.module.parameters[4].access = ACCESS_WRITE;
    props.module.parameters[4].value.boolValue = g_led_status;

    respond_status(CMD_GET_PROPERTIES, MODULE_STATUS_OK,
                   (const uint8_t *)&props, sizeof(props));
}

static void handle_set_parameter(const ModuleMessageSetParameterPayload *p)
{
    hal_debug_printf("[CMD] Handling SET_PARAMETER command, paramId=%u, dataType=%u\n",
                     p->parameterId, p->dataType);

    switch (p->parameterId)
    {
    case 0: // Button (Read-only)
        hal_debug_print("[CMD-WARN] SET_PARAMETER on read-only param 0\n");
        respond_status(CMD_SET_PARAMETER, MODULE_STATUS_UNSUPPORTED, NULL, 0);
        return;

    case 1: // Red
        if (p->dataType != PARAM_TYPE_INT)
            goto error_type;
        g_led_r = p->value.intValue;
        break;

    case 2: // Green
        if (p->dataType != PARAM_TYPE_INT)
            goto error_type;
        g_led_g = p->value.intValue;
        break;

    case 3: // Blue
        if (p->dataType != PARAM_TYPE_INT)
            goto error_type;
        g_led_b = p->value.intValue;
        break;

    case 4: // Status
        if (p->dataType != PARAM_TYPE_BOOL)
            goto error_type;
        g_led_status = p->value.boolValue;
        break;

    default:
        hal_debug_printf("[CMD-ERROR] SET_PARAMETER unknown paramId=%u\n", p->parameterId);
        respond_status(CMD_SET_PARAMETER, MODULE_STATUS_UNSUPPORTED, NULL, 0);
        return;
    }

    hal_debug_printf("[CMD] SET_PARAMETER success: paramId=%u\n", p->parameterId);
    respond_status(CMD_SET_PARAMETER, MODULE_STATUS_OK, NULL, 0);
    return;

error_type:
    hal_debug_printf("[CMD-ERROR] SET_PARAMETER type mismatch: paramId=%u, dataType=%u\n",
                     p->parameterId, p->dataType);
    respond_status(CMD_SET_PARAMETER, MODULE_STATUS_ERROR, NULL, 0);
}

static void handle_get_parameter(const ModuleMessageGetParameterPayload *p)
{
    uint8_t buf[1 + sizeof(int32_t)];

    hal_debug_printf("[CMD] Handling GET_PARAMETER command, paramId=%u\n", p->parameterId);

    buf[0] = p->parameterId;

    switch (p->parameterId)
    {
    case 0: // Button
        buf[1] = g_btn_state;
        respond_status(CMD_GET_PARAMETER, MODULE_STATUS_OK, buf, 2);
        break;

    case 1: // Red
        memcpy(&buf[1], &g_led_r, sizeof(int32_t));
        respond_status(CMD_GET_PARAMETER, MODULE_STATUS_OK, buf, 5);
        break;

    case 2: // Green
        memcpy(&buf[1], &g_led_g, sizeof(int32_t));
        respond_status(CMD_GET_PARAMETER, MODULE_STATUS_OK, buf, 5);
        break;

    case 3: // Blue
        memcpy(&buf[1], &g_led_b, sizeof(int32_t));
        respond_status(CMD_GET_PARAMETER, MODULE_STATUS_OK, buf, 5);
        break;

    case 4: // Status
        buf[1] = g_led_status;
        respond_status(CMD_GET_PARAMETER, MODULE_STATUS_OK, buf, 2);
        break;

    default:
        hal_debug_printf("[CMD-ERROR] GET_PARAMETER invalid paramId=%u\n", p->parameterId);
        respond_status(CMD_GET_PARAMETER, MODULE_STATUS_ERROR, NULL, 0);
        break;
    }
}

static void handle_set_autoupdate(const ModuleMessageSetAutoupdatePayload *p)
{
    hal_debug_printf("[CMD] Handling SET_AUTOUPDATE command: enable=%u intervalMs=%u\n",
                     p->enable, p->intervalMs);

    g_autoupdate_enabled = (p->enable != 0);
    g_autoupdate_interval_ms = p->intervalMs;

    /* If enabling, emit a first update immediately so host has a starting value */
    send_param_update_if_needed(1);
    respond_status(CMD_SET_AUTOUPDATE, MODULE_STATUS_OK, NULL, 0);
}

static void handle_reset_module(const ModuleMessageResetPayload *p)
{
    (void)p;

    hal_debug_print("[CMD] Handling RESET_MODULE command\n");

    g_btn_state = 0;
    g_led_r = 0;
    g_led_g = 0;
    g_led_b = 0;
    g_led_status = 0;

    hal_debug_print("[CMD] RESET_MODULE success: parameters reset\n");
    respond_status(CMD_RESET_MODULE, MODULE_STATUS_OK, NULL, 0);
}

/*******************************************************************************
 * Frame Processing
 ******************************************************************************/

static void hsv_to_rgb(uint16_t h, uint8_t s, uint8_t v, uint8_t *r, uint8_t *g, uint8_t *b)
{
    uint8_t region, remainder, p, q, t;

    if (s == 0)
    {
        *r = v;
        *g = v;
        *b = v;
        return;
    }

    region = h / 43;
    remainder = (h - (region * 43)) * 6;

    p = (v * (255 - s)) >> 8;
    q = (v * (255 - ((s * remainder) >> 8))) >> 8;
    t = (v * (255 - ((s * (255 - remainder)) >> 8))) >> 8;

    switch (region)
    {
    case 0:
        *r = v;
        *g = t;
        *b = p;
        break;
    case 1:
        *r = q;
        *g = v;
        *b = p;
        break;
    case 2:
        *r = p;
        *g = v;
        *b = t;
        break;
    case 3:
        *r = p;
        *g = q;
        *b = v;
        break;
    case 4:
        *r = t;
        *g = p;
        *b = v;
        break;
    default:
        *r = v;
        *g = p;
        *b = q;
        break;
    }
}

void show_startup_led_anim()
{
    WS2812_Color_t color;
    for (int i = 0; i < 100; i++)
    {
        uint16_t hue = (i * 255) / 100;
        uint8_t brightness = (255 * (50 - (i > 50 ? i - 50 : 50 - i))) / 50;
        hsv_to_rgb(hue, 255, brightness, &color.red, &color.green, &color.blue);
        WS2812_DMA_Send(&color, 1);
        Delay_Ms(50);
    }
    WS2812_DMA_Send(&(WS2812_Color_t){0, 0, 0}, 1);
}

static void process_frame(void)
{
    uint8_t cmd;
    uint16_t payload_len;
    const uint8_t *payload;

    if (g_rx_len < 5)
    {
        return;
    }

    cmd = g_rx_buf[1];
    payload_len = (uint16_t)g_rx_buf[2] | ((uint16_t)g_rx_buf[3] << 8);
    payload = &g_rx_buf[4];

    hal_debug_printf("[RX] Processing frame: cmd=0x%02X, payloadLen=%u\n", cmd, payload_len);

    switch (cmd)
    {
    case CMD_PING:
        handle_ping();
        break;

    case CMD_GET_PROPERTIES:
        if (payload_len >= 1)
        {
            handle_get_properties(payload[0]);
        }
        break;

    case CMD_SET_PARAMETER:
        if (payload_len >= sizeof(ModuleMessageSetParameterPayload))
        {
            handle_set_parameter((const ModuleMessageSetParameterPayload *)payload);
        }
        break;

    case CMD_GET_PARAMETER:
        if (payload_len >= sizeof(ModuleMessageGetParameterPayload))
        {
            handle_get_parameter((const ModuleMessageGetParameterPayload *)payload);
        }
        break;

    case CMD_RESET_MODULE:
        if (payload_len >= sizeof(ModuleMessageResetPayload))
        {
            handle_reset_module((const ModuleMessageResetPayload *)payload);
        }
        break;

    case CMD_SET_AUTOUPDATE:
        if (payload_len >= sizeof(ModuleMessageSetAutoupdatePayload))
        {
            handle_set_autoupdate((const ModuleMessageSetAutoupdatePayload *)payload);
        }
        break;

    default:
        hal_debug_printf("[RX-ERROR] Unknown command: cmd=0x%02X\n", cmd);
        respond_status(cmd, MODULE_STATUS_UNSUPPORTED, NULL, 0);
        break;
    }
}

/*******************************************************************************
 * Frame Parser
 ******************************************************************************/

static void clear_parser(void)
{
    g_rx_len = 0;
    g_rx_expected = 0;
    g_last_byte_ms = 0;
}

static void reset_parser_error(const char *reason)
{
    if (g_rx_len > 0)
    {
        hal_debug_print("[RX-ERROR] Parser reset");
        if (reason != NULL)
        {
            hal_debug_print(": ");
            hal_debug_print(reason);
        }
        hal_debug_printf(", discarding %u bytes\n", g_rx_len);
    }
    clear_parser();
}

static void feed_byte(uint8_t b)
{
    uint32_t now = hal_millis();
    uint16_t payload_len;
    uint32_t total;
    uint8_t checksum;
    uint8_t expected;

    if (g_rx_len > 0 && (now - g_last_byte_ms) > PARSER_TIMEOUT_MS)
    {
        hal_debug_print("[RX-ERROR] Parser timeout, resyncing\n");
        reset_parser_error("timeout");
    }
    g_last_byte_ms = now;

    if (g_rx_len == 0)
    {
        if (b == FRAME_START)
        {
            g_rx_buf[0] = b;
            g_rx_len = 1;
        }
        return;
    }

    if (g_rx_len >= sizeof(g_rx_buf))
    {
        hal_debug_print("[RX-ERROR] Buffer overflow, resetting parser\n");
        reset_parser_error("overflow");
        return;
    }

    g_rx_buf[g_rx_len++] = b;

    if (g_rx_len == 4)
    {
        payload_len = (uint16_t)g_rx_buf[2] | ((uint16_t)g_rx_buf[3] << 8);
        if (payload_len > MODULE_MAX_PAYLOAD)
        {
            hal_debug_printf("[RX-ERROR] Payload too large: %u > %u bytes\n",
                             payload_len, MODULE_MAX_PAYLOAD);
            reset_parser_error("payload too large");
            return;
        }
        total = 5u + payload_len;
        g_rx_expected = (total <= sizeof(g_rx_buf)) ? (uint16_t)total : 0;
        if (g_rx_expected == 0)
        {
            reset_parser_error("expected length invalid");
        }
    }

    if (g_rx_expected > 0 && g_rx_len == g_rx_expected)
    {
        checksum = g_rx_buf[g_rx_len - 1];
        expected = calc_checksum(g_rx_buf, g_rx_len - 1);
        if (checksum == expected)
        {
            hal_debug_printf("[RX] Frame complete, checksum OK (0x%02X)\n", checksum);
            process_frame();
        }
        else
        {
            hal_debug_printf("[RX-ERROR] Checksum mismatch: got=0x%02X, expected=0x%02X\n",
                             checksum, expected);
        }
        clear_parser();
    }
}

/*******************************************************************************
 * Public API
 ******************************************************************************/

/**
 * @brief Initialize the module client
 * Call this once at startup.
 */
void client_init(void)
{
    hal_init();
    hal_debug_print("Module client ready\n");
}

/**
 * @brief Process incoming data and handle auto-updates
 * Call this repeatedly in your main loop.
 */
void client_process(void)
{
    uint8_t b;

    /* Process all available bytes */
    while (hal_uart_available() > 0)
    {
        if (hal_uart_read_byte(&b))
        {
            feed_byte(b);
        }
    }

    /* In autoupdate mode, periodically push updates (or on change) */
    send_param_update_if_needed(0);
}

int main(void)
{
    client_init();

    WS2812_DMA_Init();

    show_startup_led_anim();

    hal_uart_init(115200, -1, -1);

    funGpioInitAll();

    funPinMode(PC2, GPIO_CFGLR_IN_PUPD);
    funDigitalWrite(PC2, FUN_HIGH);

    for (;;)
    {
        client_process();
        /* Do module-specific sensor/logic work here */
        int status = funDigitalRead(PC2);

        // Param 1: Button (Active Low)
        g_btn_state = (status == FUN_LOW) ? 1 : 0;

        // Update LED if parameters changed
        static uint8_t last_r = 255, last_g = 255, last_b = 255, last_stat = 255;

        if (last_r != (uint8_t)g_led_r || last_g != (uint8_t)g_led_g ||
            last_b != (uint8_t)g_led_b || last_stat != g_led_status)
        {
            WS2812_Color_t color;
            if (g_led_status)
            {
                color.red = (uint8_t)g_led_r;
                color.green = (uint8_t)g_led_g;
                color.blue = (uint8_t)g_led_b;
            }
            else
            {
                color.red = 0;
                color.green = 0;
                color.blue = 0;
            }
            WS2812_DMA_Send(&color, 1);

            last_r = (uint8_t)g_led_r;
            last_g = (uint8_t)g_led_g;
            last_b = (uint8_t)g_led_b;
            last_stat = g_led_status;
        }
    }
    return 0;
}
