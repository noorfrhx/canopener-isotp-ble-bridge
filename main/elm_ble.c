#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_err.h"
#include "esp_timer.h"
#include "ble_server.h"
#include "isotp_bridge.h"
#include "elm_ble.h"

#define ELM_VERSION "ELM327 v1.5"
#define ELM_MAX_REPLY 255
#define ELM_COMMAND_SIZE 32

static StaticSemaphore_t session_mutex_storage;
static SemaphoreHandle_t session_mutex;
static esp_timer_handle_t timeout_timer;
static void (*send_notification)(const char *, size_t);

static struct {
    bool active, headers, spaces, echo, linefeeds, automatic;
    bool overflow;
    size_t used;
    char command[ELM_COMMAND_SIZE];
    char previous[ELM_COMMAND_SIZE];
    uint16_t tx_id, rx_id;
    unsigned timeout_ms;
    int64_t deadline_us;
    uint8_t service, parameters[2];
    size_t parameter_count;
} elm;

static char reply[ELM_MAX_REPLY * 5 + 16];

static void defaults(void)
{
    elm.headers = false;
    elm.spaces = true;
    elm.echo = true;
    elm.linefeeds = false;
    elm.automatic = true;
    elm.tx_id = 0x7DF;
    elm.rx_id = 0x7E8;
    elm.timeout_ms = 200;
    elm.deadline_us = 0;
    elm.previous[0] = '\0';
}

static void respond(const char *result)
{
    size_t size = strlen(result);
    if (result != reply)
        memcpy(reply, result, size);
    const char *prompt = elm.linefeeds ? "\r\n\r\n>" : "\r\r>";
    size_t prompt_size = strlen(prompt);
    memcpy(reply + size, prompt, prompt_size);
    send_notification(reply, size + prompt_size);
}

static void request_timeout(void *arg)
{
    tMUTEX(session_mutex);
    if (elm.active && elm.deadline_us && esp_timer_get_time() >= elm.deadline_us) {
        elm.deadline_us = 0;
        respond("NO DATA");
    }
    rMUTEX(session_mutex);
}

static int hex_value(const char *text, size_t count)
{
    int value = 0;
    for (size_t i = 0; i < count; i++) {
        const char c = text[i];
        if (c >= '0' && c <= '9')
            value = value * 16 + c - '0';
        else if (c >= 'A' && c <= 'F')
            value = value * 16 + c - 'A' + 10;
        else
            return -1;
    }
    return value;
}

static const char *at_command(const char *command)
{
    if (!strcmp(command, "Z") || !strcmp(command, "WS") || !strcmp(command, "D")) {
        defaults();
        return *command == 'D' ? "OK" : ELM_VERSION;
    }
    if (!strcmp(command, "I"))
        return ELM_VERSION;
    if (!strcmp(command, "DP"))
        return elm.automatic ? "AUTO, ISO 15765-4 (CAN 11/500)" : "ISO 15765-4 (CAN 11/500)";
    if (!strcmp(command, "DPN"))
        return elm.automatic ? "A6" : "6";
    if (!strcmp(command, "SP0") || !strcmp(command, "SPA6") || !strcmp(command, "SP6")) {
        elm.automatic = strcmp(command, "SP6") != 0;
        return "OK";
    }

    size_t size = strlen(command);
    if (size == 2 && (command[1] == '0' || command[1] == '1')) {
        bool enabled = command[1] == '1';
        switch (command[0]) {
        case 'E': elm.echo = enabled; return "OK";
        case 'H': elm.headers = enabled; return "OK";
        case 'L': elm.linefeeds = enabled; return "OK";
        case 'S': elm.spaces = enabled; return "OK";
        }
    }
    if (size == 4 && !strncmp(command, "ST", 2)) {
        int value = hex_value(command + 2, 2);
        if (value >= 0) {
            elm.timeout_ms = value ? value * 4 : 200;
            return "OK";
        }
    }
    if (size == 5 && !strncmp(command, "SH", 2)) {
        int id = hex_value(command + 2, 3);
        if (id >= 0x700 && id <= 0x7E7) {
            elm.tx_id = id;
            elm.rx_id = id == 0x7DF ? 0x7E8 : id + 8;
            return "OK";
        }
    }
    if (size == 6 && !strncmp(command, "CRA", 3)) {
        int id = hex_value(command + 3, 3);
        if (id >= 0x700 && id <= 0x7FF && id != elm.tx_id &&
            (elm.tx_id != 0x7DF || id == 0x7E8)) {
            elm.rx_id = id;
            return "OK";
        }
    }
    if (!strcmp(command, "CRA")) {
        elm.rx_id = elm.tx_id == 0x7DF ? 0x7E8 : elm.tx_id + 8;
        return "OK";
    }
    // ISO-TP formatting/flow control is always enabled. The timeout is fixed
    // by AT ST, including for clients that send AT AT during initialization.
    if (!strcmp(command, "CAF1") || !strcmp(command, "CFC1") ||
        !strcmp(command, "FCSM0") ||
        !strcmp(command, "AT0") || !strcmp(command, "AT1") || !strcmp(command, "AT2"))
        return "OK";
    return "?";
}

static bool read_request(const uint8_t *data, size_t size)
{
    switch (data[0]) {
    case 0x01: case 0x06: case 0x09:
        return size == 2;
    case 0x02:
        return size == 3;
    case 0x03: case 0x07: case 0x0A:
        return size == 1;
    case 0x19:
        return elm.tx_id != 0x7DF &&
            ((size == 3 && (data[1] == 1 || data[1] == 2)) ||
             (size == 2 && data[1] == 0x0A));
    case 0x22:
        return elm.tx_id != 0x7DF && size == 3;
    default:
        return false;
    }
}

static void run_command(const char *command)
{
    if (elm.deadline_us) {
        esp_timer_stop(timeout_timer);
        elm.deadline_us = 0;
        respond("STOPPED");
    }
    if (elm.echo && *command) {
        send_notification(command, strlen(command));
        send_notification(elm.linefeeds ? "\r\n" : "\r", elm.linefeeds ? 2 : 1);
    }
    if (!strncmp(command, "AT", 2)) {
        respond(at_command(command + 2));
        return;
    }

    uint8_t data[7];
    size_t length = strlen(command);
    // ELM clients can append an expected response count (one hex digit).
    // This adapter returns only the selected controller's first response.
    if (length & 1) {
        char count = command[length - 1];
        if (count < '1' || count > '9') {
            respond("?");
            return;
        }
        length--;
    }
    if (!length || length > sizeof(data) * 2) {
        respond("?");
        return;
    }
    size_t size = length / 2;
    for (size_t i = 0; i < size; i++) {
        int value = hex_value(command + i * 2, 2);
        if (value < 0) {
            respond("?");
            return;
        }
        data[i] = value;
    }
    if (!read_request(data, size)) {
        respond("?");
        return;
    }

    uint8_t *payload = malloc(size);
    if (!payload) {
        respond("BUS ERROR");
        return;
    }
    memcpy(payload, data, size);
    send_message_t request = {
        .msg_length = size,
        .buffer = payload,
        .rxID = elm.tx_id == 0x7DF ? 0x7E0 : elm.tx_id,
        .txID = elm.rx_id,
        .flags = elm.tx_id == 0x7DF ? ISOTP_SEND_ELM_FUNCTIONAL : ISOTP_SEND_ELM_PHYSICAL,
    };
    elm.service = data[0];
    elm.parameter_count = elm.service == 0x22 ? 2 : size > 1 ? 1 : 0;
    memcpy(elm.parameters, data + 1, elm.parameter_count);
    elm.deadline_us = esp_timer_get_time() + elm.timeout_ms * 1000;
    if (bridge_send_isotp(&request) != pdTRUE) {
        free(payload);
        elm.deadline_us = 0;
        respond("BUS ERROR");
        return;
    }
    ESP_ERROR_CHECK(esp_timer_start_once(timeout_timer, elm.timeout_ms * 1000));
}

void elm_ble_init(void (*notify)(const char *, size_t))
{
    send_notification = notify;
    if (!session_mutex)
        session_mutex = xSemaphoreCreateMutexStatic(&session_mutex_storage);
    assert(session_mutex);
    const esp_timer_create_args_t args = {
        .callback = request_timeout,
        .name = "elm_timeout",
    };
    ESP_ERROR_CHECK(esp_timer_create(&args, &timeout_timer));
}

void elm_ble_deinit(void)
{
    elm_ble_set_active(false);
    ESP_ERROR_CHECK(esp_timer_delete(timeout_timer));
}

void elm_ble_set_active(bool active)
{
    tMUTEX(session_mutex);
    if (elm.active != active) {
        esp_timer_stop(timeout_timer);
        memset(&elm, 0, sizeof(elm));
        defaults();
        elm.active = active;
    }
    rMUTEX(session_mutex);
}

void elm_ble_receive(const uint8_t *data, size_t size)
{
    tMUTEX(session_mutex);
    for (size_t i = 0; elm.active && i < size; i++) {
        if (data[i] == '\r') {
            if (elm.overflow) {
                respond("?");
            } else if (elm.used) {
                elm.command[elm.used] = '\0';
                run_command(elm.command);
                memcpy(elm.previous, elm.command, elm.used + 1);
            } else if (*elm.previous) {
                run_command(elm.previous);
            } else {
                respond("?");
            }
            elm.used = 0;
            elm.overflow = false;
        } else if (data[i] != ' ' && data[i] != '\n' && data[i] != '\t') {
            if (elm.used == sizeof(elm.command) - 1 || data[i] < 0x21 || data[i] > 0x7E)
                elm.overflow = true;
            else if (!elm.overflow)
                elm.command[elm.used++] = toupper(data[i]);
        }
    }
    rMUTEX(session_mutex);
}

static void format_reply(uint32_t source, const uint8_t *data, size_t size)
{
    const char *space = elm.spaces ? " " : "";
    const char *newline = elm.linefeeds ? "\r\n" : "\r";
    bool multi = size > 7;
    // The buffer allows five characters per payload byte, plus the first frame header.
    char *out = reply;
    if (multi && !elm.headers)
        out += snprintf(out, sizeof(reply) - (out - reply), "%03X%s", (unsigned)size, newline);
    for (size_t offset = 0, frame = 0; offset < size; frame++) {
        size_t count = multi && !frame ? 6 : 7;
        if (count > size - offset)
            count = size - offset;
        if (elm.headers) {
            out += snprintf(out, sizeof(reply) - (out - reply), "%03lX%s", (unsigned long)source, space);
            unsigned pci = !multi ? size : !frame ? 0x10 : (0x20 | (frame & 0xF));
            out += snprintf(out, sizeof(reply) - (out - reply), "%02X%s", pci, space);
            if (multi && !frame)
                out += snprintf(out, sizeof(reply) - (out - reply), "%02X%s", (unsigned)size, space);
        } else if (multi) {
            out += snprintf(out, sizeof(reply) - (out - reply), "%X:%s", (unsigned)(frame & 0xF), space);
        }
        for (size_t i = 0; i < count; i++)
            out += snprintf(out, sizeof(reply) - (out - reply), "%02X%s", data[offset + i], i + 1 == count ? "" : space);
        offset += count;
        if (offset < size)
            out += snprintf(out, sizeof(reply) - (out - reply), "%s", newline);
    }
    respond(reply);
}

bool elm_ble_reply(uint32_t source, uint32_t destination, uint8_t flags,
                   const uint8_t *data, size_t size)
{
    tMUTEX(session_mutex);
    bool active = elm.active;
    uint16_t tx_id = elm.tx_id == 0x7DF ? 0x7E0 : elm.tx_id;
    if (active && elm.deadline_us > esp_timer_get_time() && !flags &&
        source == elm.rx_id && destination == tx_id && size) {
        bool negative = size >= 3 && data[0] == 0x7F && data[1] == elm.service && data[2] != 0x78;
        bool positive = data[0] == elm.service + 0x40 &&
            (size > elm.parameter_count && !memcmp(data + 1, elm.parameters, elm.parameter_count));
        if (negative || positive) {
            esp_timer_stop(timeout_timer);
            elm.deadline_us = 0;
            if (size <= ELM_MAX_REPLY)
                format_reply(source, data, size);
            else
                respond("BUFFER FULL");
        }
    }
    rMUTEX(session_mutex);
    return active;
}
