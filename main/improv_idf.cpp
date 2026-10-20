#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "driver/uart.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tally_settings.hpp"

namespace {

constexpr char kTag[] = "improv";
constexpr uart_port_t kPort = UART_NUM_0;
constexpr uint8_t kVersion = 1;
constexpr uint8_t kStateFrame = 1;
constexpr uint8_t kErrorFrame = 2;
constexpr uint8_t kRpcFrame = 3;
constexpr uint8_t kRpcResponseFrame = 4;
constexpr uint8_t kWifiSettings = 1;
constexpr uint8_t kCurrentState = 2;
constexpr uint8_t kDeviceInfo = 3;
constexpr uint8_t kWifiNetworks = 4;
constexpr uint8_t kUnableToConnect = 3;

std::array<uint8_t, 128> frame_buffer{};
size_t frame_position = 0;

void send_frame(uint8_t type, const uint8_t* payload, size_t payload_size) {
    if (payload_size > 250) return;
    std::vector<uint8_t> frame{'I', 'M', 'P', 'R', 'O', 'V', kVersion, type,
                               static_cast<uint8_t>(payload_size)};
    frame.insert(frame.end(), payload, payload + payload_size);
    uint8_t checksum = 0;
    for (uint8_t value : frame) checksum = static_cast<uint8_t>(checksum + value);
    frame.push_back(checksum);
    uart_write_bytes(kPort, frame.data(), frame.size());
}

void send_state(uint8_t state) {
    send_frame(kStateFrame, &state, 1);
}

void send_error(uint8_t error) {
    send_frame(kErrorFrame, &error, 1);
}

std::vector<uint8_t> rpc_payload(uint8_t command, const std::vector<std::string>& values) {
    std::vector<uint8_t> payload{command, 0};
    for (const std::string& value : values) {
        const size_t size = std::min<size_t>(value.size(), 255);
        payload.push_back(static_cast<uint8_t>(size));
        payload.insert(payload.end(), value.begin(), value.begin() + size);
    }
    payload[1] = static_cast<uint8_t>(payload.size() - 2);
    return payload;
}

void send_rpc_response(uint8_t command, const std::vector<std::string>& values) {
    const std::vector<uint8_t> payload = rpc_payload(command, values);
    send_frame(kRpcResponseFrame, payload.data(), payload.size());
}

void apply_wifi_settings(const uint8_t* data, size_t length) {
    if (length < 2 || data[1] + 2 > length) {
        send_error(1);
        return;
    }
    const size_t ssid_length = data[1];
    const size_t password_length_offset = 2 + ssid_length;
    if (password_length_offset >= length) {
        send_error(1);
        return;
    }
    const size_t password_length = data[password_length_offset];
    if (password_length_offset + 1 + password_length > length || ssid_length == 0 || ssid_length > 32 || password_length > 64) {
        send_error(1);
        return;
    }

    TallySettings updated = tally_settings;
    memcpy(updated.wifi_ssid, data + 2, ssid_length);
    updated.wifi_ssid[ssid_length] = '\0';
    memcpy(updated.wifi_password, data + password_length_offset + 1, password_length);
    updated.wifi_password[password_length] = '\0';

    send_state(3);
    wifi_config_t config{};
    strlcpy(reinterpret_cast<char*>(config.sta.ssid), updated.wifi_ssid, sizeof(config.sta.ssid));
    strlcpy(reinterpret_cast<char*>(config.sta.password), updated.wifi_password, sizeof(config.sta.password));
    config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    if (esp_wifi_set_mode(WIFI_MODE_APSTA) != ESP_OK ||
        esp_wifi_set_config(WIFI_IF_STA, &config) != ESP_OK || esp_wifi_connect() != ESP_OK) {
        send_state(0);
        send_error(kUnableToConnect);
        return;
    }

    bool connected = false;
    for (int attempt = 0; attempt < 20; ++attempt) {
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        esp_netif_ip_info_t ip_info{};
        if (netif && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK && ip_info.ip.addr != 0) {
            connected = true;
            break;
        }
    }
    if (!connected) {
        send_state(0);
        send_error(kUnableToConnect);
        return;
    }

    tally_settings = updated;
    if (tally_settings_save() != ESP_OK) {
        send_state(0);
        send_error(0xFF);
        return;
    }
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    send_error(0);
    send_state(4);
    esp_netif_ip_info_t ip_info{};
    esp_netif_get_ip_info(esp_netif_get_handle_from_ifkey("WIFI_STA_DEF"), &ip_info);
    char url[48];
    snprintf(url, sizeof(url), "http://%d.%d.%d.%d", IP2STR(&ip_info.ip));
    send_rpc_response(kWifiSettings, {url});
}

void handle_rpc(const uint8_t* data, size_t length) {
    if (length < 2 || data[1] != length - 2) {
        send_error(1);
        return;
    }
    switch (data[0]) {
        case kWifiSettings:
            apply_wifi_settings(data, length);
            break;
        case kCurrentState: {
            esp_netif_ip_info_t ip_info{};
            esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
            const bool connected = netif && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK && ip_info.ip.addr;
            send_state(connected ? 4 : 2);
            if (connected) {
                char url[48];
                snprintf(url, sizeof(url), "http://%d.%d.%d.%d", IP2STR(&ip_info.ip));
                send_rpc_response(kCurrentState, {url});
            }
            break;
        }
        case kDeviceInfo:
            send_rpc_response(kDeviceInfo, {"ATEM Tally Light", "ESP-IDF", "ESP32-C3/S3", tally_settings.device_name});
            break;
        case kWifiNetworks: {
            wifi_scan_config_t scan{};
            if (esp_wifi_scan_start(&scan, true) != ESP_OK) {
                send_rpc_response(kWifiNetworks, {});
                break;
            }
            uint16_t count = 0;
            esp_wifi_scan_get_ap_num(&count);
            std::vector<wifi_ap_record_t> records(count);
            esp_wifi_scan_get_ap_records(&count, records.data());
            for (uint16_t index = 0; index < count; ++index) {
                send_rpc_response(kWifiNetworks, {
                    reinterpret_cast<const char*>(records[index].ssid),
                    std::to_string(records[index].rssi),
                    records[index].authmode == WIFI_AUTH_OPEN ? "NO" : "YES"});
                vTaskDelay(1);
            }
            send_rpc_response(kWifiNetworks, {});
            break;
        }
        default:
            send_error(2);
            break;
    }
}

void consume_byte(uint8_t byte) {
    if (frame_position >= frame_buffer.size()) frame_position = 0;
    static constexpr uint8_t magic[] = {'I', 'M', 'P', 'R', 'O', 'V'};
    if (frame_position < sizeof(magic) && byte != magic[frame_position]) {
        frame_position = byte == magic[0] ? 1 : 0;
        return;
    }
    frame_buffer[frame_position++] = byte;
    if (frame_position == 9) {
        const size_t expected = 10 + frame_buffer[8];
        if (expected > frame_buffer.size()) frame_position = 0;
        return;
    }
    if (frame_position < 9) return;
    const size_t expected = 10 + frame_buffer[8];
    if (frame_position < expected) return;

    uint8_t checksum = 0;
    for (size_t index = 0; index + 1 < expected; ++index)
        checksum = static_cast<uint8_t>(checksum + frame_buffer[index]);
    if (checksum == byte && frame_buffer[6] == kVersion && frame_buffer[7] == kRpcFrame)
        handle_rpc(frame_buffer.data() + 9, frame_buffer[8]);
    else if (checksum != byte)
        send_error(1);
    frame_position = 0;
}

void improv_task(void*) {
    uint8_t input[32];
    while (true) {
        const int count = uart_read_bytes(kPort, input, sizeof(input), pdMS_TO_TICKS(100));
        for (int index = 0; index < count; ++index) consume_byte(input[index]);
    }
}

}  // namespace

void improv_serial_start() {
    uart_config_t config{};
    config.baud_rate = 115200;
    config.data_bits = UART_DATA_8_BITS;
    config.parity = UART_PARITY_DISABLE;
    config.stop_bits = UART_STOP_BITS_1;
    config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    config.source_clk = UART_SCLK_DEFAULT;
    ESP_ERROR_CHECK(uart_driver_install(kPort, 512, 512, 0, nullptr, 0));
    ESP_ERROR_CHECK(uart_param_config(kPort, &config));
    ESP_ERROR_CHECK(uart_set_pin(kPort, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    xTaskCreate(improv_task, "improv_serial", 4096, nullptr, 4, nullptr);
    ESP_LOGI(kTag, "Improv serial provisioning active on UART0 at 115200 baud");
}