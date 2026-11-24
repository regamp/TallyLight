#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <atomic>
#include <cstdio>
#include <unistd.h>

#include "driver/rmt.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "nvs_flash.h"
#include "improv_idf.hpp"
#include "tally_server_idf.hpp"
#include "tally_settings.hpp"

TallySettings tally_settings{};

namespace {

constexpr char kTag[] = "atem_tally";
constexpr uint16_t kAtemPort = 9910;
constexpr uint8_t kMaxTallySources = 41;
constexpr uint8_t kTallyProgram = 0x01;
constexpr uint8_t kTallyPreview = 0x02;
constexpr uint16_t kAtemPacketIdModulus = 1U << 15;
constexpr size_t kAtemReceiveBatchSize = 16;
constexpr size_t kAtemOutOfOrderCapacity = 32;
constexpr size_t kAtemMaxPacketSize = 1500;
constexpr size_t kAtemBufferedPacketSize = 256;
constexpr bool kAckDuringInit = false;
constexpr int kWifiConnected = BIT0;
constexpr uint32_t kInitRequestRetryMs = 1000;
constexpr uint32_t kInitRequestCheckMs = 200;
constexpr uint32_t kOutOfOrderGapTimeoutMs = 500;
constexpr uint32_t kHealthReportIntervalMs = 30000;
constexpr uint32_t kDebugAutoDisableMs = 10 * 60 * 1000;
constexpr size_t kMaxNeopixelCount = 64;

enum class Color : uint8_t {
    Off,
    Red,
    Green,
    Blue,
    Pink,
    White,
    Orange,
};

struct Rgb {
    uint8_t red;
    uint8_t green;
    uint8_t blue;
};

struct PendingAtemPacket {
    std::array<uint8_t, kAtemBufferedPacketSize> bytes{};
};

EventGroupHandle_t wifi_events;
esp_netif_t* station_netif = nullptr;
TaskHandle_t atem_task_handle = nullptr;
portMUX_TYPE tally_flags_mux = portMUX_INITIALIZER_UNLOCKED;
int atem_socket = -1;
sockaddr_in switcher_address{};
std::array<uint8_t, kMaxTallySources> tally_flags{};
uint16_t local_packet_id = 0;
uint16_t session_id = 0x53AB;
uint16_t last_remote_packet_id = 0;
bool remote_sequence_initialized = false;
std::array<PendingAtemPacket, kAtemOutOfOrderCapacity> out_of_order_packets{};
uint32_t out_of_order_occupied = 0;
uint32_t out_of_order_gap_started_ms = 0;
uint32_t last_gap_warning_ms = 0;
uint32_t atem_skipped_gap_count = 0;
uint32_t last_atem_send_error_log_ms = 0;
uint32_t suppressed_atem_send_errors = 0;
uint32_t atem_send_failure_count = 0;
uint32_t atem_enomem_send_failure_count = 0;
uint32_t timeout_reconnect_count = 0;
uint32_t wifi_reconnect_count = 0;
uint32_t manual_reconnect_count = 0;
uint32_t last_atem_packet_ms = 0;
uint32_t last_hello_ms = 0;
uint32_t last_battery_sample_ms = 0;
uint32_t battery_warning_started_ms = 0;
std::atomic<bool> atem_connected{false};
std::atomic<bool> stream_is_on_air{false};
std::atomic<uint8_t> current_state{0};
std::atomic<bool> output_refresh_pending{false};
bool battery_warning_scheduled = false;
bool battery_warning_flash_active = false;
bool battery_warning_flash_on = false;
bool soft_ap_active = false;
bool web_server_started = false;
bool init_payload_seen = false;
bool waiting_for_init_packet = false;
uint16_t requested_init_packet_id = 0;
uint32_t init_request_sent_ms = 0;
uint8_t init_request_attempts = 0;
bool atem_initialized = false;
uint16_t init_last_packet_id = 0;
std::array<uint8_t, 6> missed_init_packets{};
std::array<Rgb, kMaxNeopixelCount> strip_pixels{};
std::array<rmt_item32_t, kMaxNeopixelCount * 24 + 1> strip_items{};
size_t strip_pixel_count = 0;
SemaphoreHandle_t strip_mutex = nullptr;
bool strip_initialized = false;
std::atomic<bool> test_server_mode{false};
std::atomic<bool> debug_logging_mode{false};
std::atomic<bool> wifi_lost{false};
std::atomic<bool> manual_reset_requested{false};
std::atomic<bool> debug_tally_pending{false};
std::atomic<uint32_t> debug_logging_started_ms{0};
std::atomic<uint8_t> debug_number{1};
std::atomic<uint8_t> debug_flags{0};
uint32_t atem_rx_packet_count = 0;
uint32_t atem_tx_packet_count = 0;
uint32_t atem_duplicate_packet_count = 0;
uint32_t atem_out_of_order_packet_count = 0;
uint32_t last_atem_debug_summary_ms = 0;
uint32_t last_debug_anomaly_log_ms = 0;
bool fallback_ap_active = false;
uint32_t wifi_connected_since_ms = 0;

uint16_t read_be16(const uint8_t* data) {
    return static_cast<uint16_t>((static_cast<uint16_t>(data[0]) << 8) | data[1]);
}

uint32_t millis_now();

void record_atem_packet(const char* direction) {
    if (strcmp(direction, "RX") == 0) ++atem_rx_packet_count;
    else ++atem_tx_packet_count;
}

bool should_log_debug_anomaly() {
    if (!debug_logging_mode.load()) return false;
    const uint32_t now = millis_now();
    if (now - last_debug_anomaly_log_ms < 1000) return false;
    last_debug_anomaly_log_ms = now;
    return true;
}

void write_be16(uint8_t* data, uint16_t value) {
    data[0] = static_cast<uint8_t>(value >> 8);
    data[1] = static_cast<uint8_t>(value);
}

uint32_t millis_now() {
    return static_cast<uint32_t>(esp_timer_get_time() / 1000);
}

Rgb color_value(Color color) {
    switch (color) {
        case Color::Red: return {255, 0, 0};
        case Color::Green: return {0, 255, 0};
        case Color::Blue: return {0, 0, 255};
        case Color::Pink: return {255, 0, 255};
        case Color::White: return {255, 255, 255};
        case Color::Orange: return {255, 165, 0};
        default: return {0, 0, 0};
    }
}

void show_strip();
void update_tally_leds();

size_t status_pixel_index() {
    if (strip_pixel_count == 0) return strip_pixel_count;
    if (tally_settings.neopixel_status == 1) return 0;
    if (tally_settings.neopixel_status == 2) return strip_pixel_count - 1;
    return tally_battery_is_low() ? 0 : strip_pixel_count;
}

bool is_battery_warning_pixel(size_t index) {
    return battery_warning_flash_active && strip_pixel_count > 0 &&
           (index == 0 || index == strip_pixel_count - 1);
}

void update_battery_warning(uint32_t now) {
    const bool low = tally_settings.battery_monitor_enabled && tally_battery_is_low();
    if (!low) {
        const bool was_warning = battery_warning_scheduled || battery_warning_flash_active;
        battery_warning_scheduled = false;
        battery_warning_flash_active = false;
        battery_warning_flash_on = false;
        if (was_warning) update_tally_leds();
        return;
    }

    if (!battery_warning_scheduled ||
        (!battery_warning_flash_active && now - battery_warning_started_ms >= 30000)) {
        battery_warning_scheduled = true;
        battery_warning_flash_active = true;
        battery_warning_started_ms = now;
    }

    const uint32_t elapsed = now - battery_warning_started_ms;
    const bool active = elapsed < 1500;
    const bool flash_on = elapsed < 500 || (elapsed >= 1000 && elapsed < 1500);
    const bool flash_ended = battery_warning_flash_active && !active;
    if (active != battery_warning_flash_active || flash_on != battery_warning_flash_on) {
        battery_warning_flash_active = active;
        battery_warning_flash_on = flash_on;
        if (strip_initialized) show_strip();
        if (flash_ended) update_tally_leds();
    }
}

void render_strip_locked() {
    if (!strip_initialized || strip_pixel_count == 0) return;
    const size_t status_index = status_pixel_index();
    size_t item_count = 0;
    const int brightness = tally_settings.neopixel_brightness;
    for (size_t index = 0; index < strip_pixel_count; ++index) {
        Rgb pixel = strip_pixels[index];
        if (is_battery_warning_pixel(index)) {
            pixel = battery_warning_flash_on ? color_value(Color::Pink) : color_value(Color::Off);
        } else if (index == status_index) {
            if (current_state == 3) {
                pixel = color_value(Color::Orange);
                pixel.red = static_cast<uint8_t>(pixel.red * 25 / 255);
                pixel.green = static_cast<uint8_t>(pixel.green * 25 / 255);
                pixel.blue = static_cast<uint8_t>(pixel.blue * 25 / 255);
            } else if (current_state == 1) {
                pixel = color_value(Color::Blue);
            } else if (current_state == 2) {
                pixel = color_value(Color::Pink);
            }
        }
        const uint8_t red = static_cast<uint8_t>(pixel.red * brightness / 255);
        const uint8_t green = static_cast<uint8_t>(pixel.green * brightness / 255);
        const uint8_t blue = static_cast<uint8_t>(pixel.blue * brightness / 255);
        uint8_t ordered[3] = {green, red, blue};
        switch (tally_settings.neopixel_color_order) {
            case 1: ordered[0] = red; ordered[1] = green; ordered[2] = blue; break;
            case 2: ordered[0] = blue; ordered[1] = red; ordered[2] = green; break;
            case 3: ordered[0] = red; ordered[1] = blue; ordered[2] = green; break;
            case 4: ordered[0] = blue; ordered[1] = green; ordered[2] = red; break;
            case 5: ordered[0] = green; ordered[1] = blue; ordered[2] = red; break;
            default: break;
        }
        for (uint8_t value : ordered) {
            for (int bit = 7; bit >= 0; --bit) {
                rmt_item32_t& item = strip_items[item_count++];
                item = {};
                const bool one = (value & (1U << bit)) != 0;
                item.level0 = 1;
                item.duration0 = one ? 32 : 14;
                item.level1 = 0;
                item.duration1 = one ? 14 : 32;
            }
        }
    }
    rmt_item32_t& reset = strip_items[item_count++];
    reset = {};
    reset.level0 = 0;
    reset.duration0 = 6000;
    reset.level1 = 0;
    reset.duration1 = 6000;
    const esp_err_t result = rmt_write_items(RMT_CHANNEL_0, strip_items.data(), item_count, true);
    if (result != ESP_OK) {
        ESP_LOGE(kTag, "Could not update LED strip: %s", esp_err_to_name(result));
    }
}

void show_strip() {
    if (!strip_mutex) return;
    xSemaphoreTake(strip_mutex, portMAX_DELAY);
    render_strip_locked();
    xSemaphoreGive(strip_mutex);
}

void initialize_strip() {
    if (tally_settings.neopixel_count == 0) return;
    if (tally_settings.neopixel_count > kMaxNeopixelCount) {
        ESP_LOGW(kTag, "Neopixel count %u exceeds the supported limit; using %u",
                 static_cast<unsigned>(tally_settings.neopixel_count),
                 static_cast<unsigned>(kMaxNeopixelCount));
    }
    strip_mutex = xSemaphoreCreateMutex();
    if (!strip_mutex) {
        ESP_LOGE(kTag, "Could not create LED strip mutex");
        return;
    }
    rmt_config_t config{};
    config.rmt_mode = RMT_MODE_TX;
    config.channel = RMT_CHANNEL_0;
    config.gpio_num = static_cast<gpio_num_t>(tally_settings.neopixel_gpio);
    config.mem_block_num = 1;
    config.clk_div = 2;
    config.tx_config.loop_en = false;
    config.tx_config.carrier_en = false;
    config.tx_config.idle_output_en = true;
    config.tx_config.idle_level = RMT_IDLE_LEVEL_LOW;
    ESP_ERROR_CHECK(rmt_config(&config));
    ESP_ERROR_CHECK(rmt_driver_install(RMT_CHANNEL_0, 0, 0));
    strip_pixel_count = std::min<size_t>(tally_settings.neopixel_count, kMaxNeopixelCount);
    strip_initialized = true;
    show_strip();
}

void set_strip_tally(Color color) {
    if (!strip_mutex || !strip_initialized) return;
    xSemaphoreTake(strip_mutex, portMAX_DELAY);
    const size_t status_index = status_pixel_index();
    for (size_t index = 0; index < strip_pixel_count; ++index) {
        if (index == status_index || is_battery_warning_pixel(index)) continue;
        strip_pixels[index] = color_value(color);
    }
    render_strip_locked();
    xSemaphoreGive(strip_mutex);
}

void set_state(uint8_t state) {
    if (current_state == state) return;
    current_state = state;
    output_refresh_pending.store(true);
}

Color tally_color(uint8_t mode, uint8_t tally_flag) {
    if (mode == 4) return stream_is_on_air ? Color::Red : Color::Off;
    if (tally_flag & kTallyProgram) return Color::Red;
    if (mode != 3 && ((tally_flag & kTallyPreview) || mode == 2)) return Color::Green;
    return Color::Off;
}

void update_tally_leds() {
    if (test_server_mode.load()) {
        const uint8_t flags = debug_flags.load();
        const Color color = (flags & kTallyProgram) ? Color::Red :
                            ((flags & kTallyPreview) ? Color::Green : Color::Off);
        set_strip_tally(color);
        return;
    }
    const unsigned source = tally_settings.tally_number - 1;
    uint8_t flag = 0;
    if (source < tally_flags.size()) {
        portENTER_CRITICAL(&tally_flags_mux);
        flag = tally_flags[source];
        portEXIT_CRITICAL(&tally_flags_mux);
    }
    set_strip_tally(tally_color(tally_settings.led1_mode, flag));
}

void apply_debug_tally() {
    if (!test_server_mode.load()) return;
    std::array<uint8_t, kMaxTallySources> debug_tally_flags{};
    const uint8_t number = debug_number.load();
    if (number >= 1 && number <= kMaxTallySources)
        debug_tally_flags[number - 1] = debug_flags.load();
    tally_server_set_all_flags(kMaxTallySources, debug_tally_flags.data());
    output_refresh_pending.store(true);
}

void wifi_event_handler(void*, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        if (tally_settings.wifi_ssid[0] != '\0') {
            const esp_err_t result = esp_wifi_connect();
            if (result != ESP_OK) {
                ESP_LOGE(kTag, "Could not connect to Wi-Fi: %s", esp_err_to_name(result));
            }
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_START) {
        debug_tally_pending.store(true);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(wifi_events, kWifiConnected);
        wifi_lost.store(true);
        const auto* event = static_cast<wifi_event_sta_disconnected_t*>(event_data);
        ESP_LOGW(kTag, "Wi-Fi disconnected (reason %u); reconnecting",
                 event ? static_cast<unsigned>(event->reason) : 0U);
        const esp_err_t result = esp_wifi_connect();
        if (result != ESP_OK) {
            ESP_LOGE(kTag, "Could not reconnect to Wi-Fi: %s", esp_err_to_name(result));
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(wifi_events, kWifiConnected);
        debug_tally_pending.store(true);
    }
}

wifi_config_t make_setup_ap_config() {
    wifi_config_t config{};
    strlcpy(reinterpret_cast<char*>(config.ap.ssid), "Tally Light setup", sizeof(config.ap.ssid));
    config.ap.ssid_len = strlen(reinterpret_cast<char*>(config.ap.ssid));
    config.ap.channel = 1;
    config.ap.max_connection = 4;
    config.ap.authmode = WIFI_AUTH_OPEN;
    return config;
}

void initialize_wifi() {
    wifi_events = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    station_netif = esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();
    if (station_netif) {
        char hostname[sizeof(tally_settings.device_name)]{};
        strlcpy(hostname, tally_settings.device_name, sizeof(hostname));
        for (char* character = hostname; *character; ++character) {
            const unsigned char value = static_cast<unsigned char>(*character);
            if (!((value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
                  (value >= '0' && value <= '9') || value == '-')) {
                *character = '-';
            }
        }
        size_t hostname_length = strlen(hostname);
        while (hostname_length && hostname[hostname_length - 1] == '-')
            hostname[--hostname_length] = '\0';
        size_t leading_hyphens = 0;
        while (hostname[leading_hyphens] == '-') ++leading_hyphens;
        if (leading_hyphens) memmove(hostname, hostname + leading_hyphens,
                                     hostname_length - leading_hyphens + 1);
        if (!hostname[0]) strlcpy(hostname, "tally-light", sizeof(hostname));
        ESP_ERROR_CHECK(esp_netif_set_hostname(station_netif, hostname));
    }

    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_config));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, nullptr));

    wifi_config_t config{};
    strlcpy(reinterpret_cast<char*>(config.sta.ssid), tally_settings.wifi_ssid, sizeof(config.sta.ssid));
    strlcpy(reinterpret_cast<char*>(config.sta.password), tally_settings.wifi_password, sizeof(config.sta.password));
    config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;

    if (tally_settings.static_ip && station_netif) {
        esp_netif_ip_info_t ip_info{};
        ip_info.ip.addr = tally_settings.tally_ip;
        ip_info.netmask.addr = tally_settings.subnet_mask;
        ip_info.gw.addr = tally_settings.gateway_ip;
        ESP_ERROR_CHECK(esp_netif_dhcpc_stop(station_netif));
        ESP_ERROR_CHECK(esp_netif_set_ip_info(station_netif, &ip_info));
    }

    const bool has_saved_wifi = tally_settings.wifi_ssid[0] != '\0';
    const esp_err_t mode_result = esp_wifi_set_mode(has_saved_wifi ? WIFI_MODE_STA : WIFI_MODE_AP);
    if (mode_result != ESP_OK) {
        ESP_LOGE(kTag, "Could not set Wi-Fi mode: %s", esp_err_to_name(mode_result));
    }
    if (has_saved_wifi) {
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &config));
    } else {
        wifi_config_t ap_config = make_setup_ap_config();
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    }
    ESP_ERROR_CHECK(esp_wifi_start());
    if (!tally_settings.wifi_ssid[0]) soft_ap_active = true;
}

void record_atem_send_failure(const char* operation, int error);

bool send_atem_packet(uint8_t flags, uint16_t packet_id, const uint8_t* payload, size_t payload_size,
                      uint8_t header_byte_9 = 0, uint8_t header_byte_6 = 0,
                      uint8_t header_byte_7 = 0, uint8_t header_byte_8 = 0) {
    uint8_t packet[32]{};
    const size_t packet_size = 12 + payload_size;
    if (atem_socket < 0 || packet_size > sizeof(packet)) return false;

    packet[0] = static_cast<uint8_t>((flags << 3) | ((packet_size >> 8) & 0x07));
    packet[1] = static_cast<uint8_t>(packet_size);
    write_be16(packet + 2, session_id);
    write_be16(packet + 4, packet_id);
    packet[6] = header_byte_6;
    packet[7] = header_byte_7;
    packet[8] = header_byte_8;
    packet[9] = header_byte_9;
    if (flags != 0x02 && flags != 0x10 && flags != 0x08) {
        local_packet_id = (local_packet_id + 1) & 0x7FFF;
        write_be16(packet + 10, local_packet_id);
    }
    if (payload_size > 0) memcpy(packet + 12, payload, payload_size);

    const int sent = sendto(atem_socket, packet, packet_size, 0,
                            reinterpret_cast<const sockaddr*>(&switcher_address), sizeof(switcher_address));
    if (sent < 0) {
        record_atem_send_failure("ATEM UDP send", errno);
        return false;
    } else if (static_cast<size_t>(sent) != packet_size) {
        record_atem_send_failure("Short ATEM UDP send", errno);
        return false;
    } else {
        record_atem_packet("TX");
        return true;
    }
}

void send_hello() {
    uint8_t payload[8]{};
    payload[0] = 0x01;
    send_atem_packet(0x02, 0, payload, sizeof(payload), 0x3A);
    last_hello_ms = millis_now();
}

void record_atem_send_failure(const char* operation, int error) {
    ++atem_send_failure_count;
    if (error == ENOMEM) ++atem_enomem_send_failure_count;
    const uint32_t now = millis_now();
    if (last_atem_send_error_log_ms == 0 || now - last_atem_send_error_log_ms >= 1000) {
        ESP_LOGW(kTag,
                 "%s failed: errno %d; free heap=%u, largest block=%u, failures=%u, ENOMEM=%u, suppressed=%u",
                 operation, error,
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_8BIT)),
                 static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)),
                 static_cast<unsigned>(atem_send_failure_count),
                 static_cast<unsigned>(atem_enomem_send_failure_count),
                 static_cast<unsigned>(suppressed_atem_send_errors));
        last_atem_send_error_log_ms = now;
        suppressed_atem_send_errors = 0;
    } else {
        ++suppressed_atem_send_errors;
    }
}

void process_atem_payload(const uint8_t* packet, size_t packet_size) {
    size_t offset = 12;
    char command_summary[192]{};
    size_t summary_length = 0;
    size_t command_count = 0;
    while (offset + 8 <= packet_size) {
        const uint16_t command_size = read_be16(packet + offset);
        if (command_size < 8 || offset + command_size > packet_size) break;
        const uint8_t* command = packet + offset;
        const size_t data_size = command_size - 8;
        const bool time_command = memcmp(command + 4, "Time", 4) == 0;
        if (debug_logging_mode.load() && !time_command &&
            summary_length < sizeof(command_summary) - 8) {
            const int written = snprintf(command_summary + summary_length,
                                         sizeof(command_summary) - summary_length,
                                         "%s%.4s", command_count == 0 ? "" : ",", command + 4);
            if (written > 0) summary_length += std::min<size_t>(written, sizeof(command_summary) - summary_length - 1);
            ++command_count;
        }

        if (!test_server_mode.load() && memcmp(command + 4, "TlIn", 4) == 0 && data_size >= 2) {
            const uint16_t sources = std::min<uint16_t>(read_be16(command + 8), kMaxTallySources);
            const size_t count = std::min<size_t>(sources, data_size - 2);
            std::array<uint8_t, kMaxTallySources> flags_snapshot{};
            portENTER_CRITICAL(&tally_flags_mux);
            memset(tally_flags.data(), 0, tally_flags.size());
            memcpy(tally_flags.data(), command + 10, count);
            flags_snapshot = tally_flags;
            portEXIT_CRITICAL(&tally_flags_mux);
            tally_server_set_all_flags(static_cast<uint8_t>(sources), flags_snapshot.data());
            output_refresh_pending.store(true);
        } else if (!test_server_mode.load() && memcmp(command + 4, "StRS", 4) == 0 && data_size >= 2) {
            const uint16_t stream_flags = read_be16(command + 8);
            stream_is_on_air = (stream_flags & (1U << 2)) != 0;
            output_refresh_pending.store(true);
        }

        offset += command_size;
    }
    const uint32_t now = millis_now();
    if (debug_logging_mode.load() && command_count > 0 &&
        now - last_atem_debug_summary_ms >= 1000) {
        ESP_LOGI(kTag, "ATEM commands RX (%u): %s%s",
                 static_cast<unsigned>(command_count), command_summary,
                 command_count > 32 ? ",..." : "");
        last_atem_debug_summary_ms = now;
    }
}

void request_missing_initialization_packet() {
    if (atem_initialized || !init_payload_seen) return;

    if (waiting_for_init_packet) {
        const bool packet_received =
            (missed_init_packets[requested_init_packet_id >> 3] & (1U << (requested_init_packet_id & 7))) == 0;
        if (packet_received) {
            waiting_for_init_packet = false;
            init_request_attempts = 0;
        } else if (millis_now() - init_request_sent_ms >= kInitRequestRetryMs) {
            if (init_request_attempts >= 5) {
                missed_init_packets[requested_init_packet_id >> 3] &=
                    static_cast<uint8_t>(~(1U << (requested_init_packet_id & 7)));
                ESP_LOGW(kTag, "Giving up on missing ATEM init packet %u after 5 attempts",
                         static_cast<unsigned>(requested_init_packet_id));
                waiting_for_init_packet = false;
                init_request_attempts = 0;
            } else {
                const uint16_t previous_id = requested_init_packet_id - 1;
                if (should_log_debug_anomaly()) {
                    ESP_LOGI(kTag, "Requesting missing ATEM init packet %u (attempt %u)",
                             requested_init_packet_id,
                             static_cast<unsigned>(init_request_attempts) + 1);
                }
                send_atem_packet(0x08, 0, nullptr, 0, 0,
                                 static_cast<uint8_t>(previous_id >> 8),
                                 static_cast<uint8_t>(previous_id), 0x01);
                init_request_sent_ms = millis_now();
                ++init_request_attempts;
            }
        }
        if (waiting_for_init_packet) return;
    }

    for (uint16_t packet_id = 1; packet_id < init_last_packet_id && packet_id < 41; ++packet_id) {
        if (missed_init_packets[packet_id >> 3] & (1U << (packet_id & 7))) {
            requested_init_packet_id = packet_id;
            const uint16_t previous_id = requested_init_packet_id - 1;
            if (should_log_debug_anomaly()) {
                ESP_LOGI(kTag, "Requesting missing ATEM init packet %u (attempt 1)",
                         requested_init_packet_id);
            }
            send_atem_packet(0x08, 0, nullptr, 0, 0,
                             static_cast<uint8_t>(previous_id >> 8),
                             static_cast<uint8_t>(previous_id), 0x01);
            waiting_for_init_packet = true;
            init_request_sent_ms = millis_now();
            init_request_attempts = 1;
            return;
        }
    }
    atem_initialized = true;
    ESP_LOGI(kTag, "ATEM initialization complete");
}

void clear_out_of_order_packets() {
    out_of_order_occupied = 0;
    out_of_order_gap_started_ms = 0;
}

void process_ordered_atem_packet(const uint8_t* packet, size_t packet_size, uint16_t packet_id) {
    if (packet_id < 41) {
        missed_init_packets[packet_id >> 3] &=
            static_cast<uint8_t>(~(1U << (packet_id & 7)));
    }
    if (!init_payload_seen && packet_size == 12 && packet_id > 1) {
        init_payload_seen = true;
        init_last_packet_id = packet_id;
    }
    if (packet_size > 12 && packet_size <= kAtemMaxPacketSize)
        process_atem_payload(packet, packet_size);
    request_missing_initialization_packet();
}

PendingAtemPacket* find_pending_atem_packet(uint16_t packet_id) {
    for (size_t index = 0; index < out_of_order_packets.size(); ++index) {
        if ((out_of_order_occupied & (uint32_t{1} << index)) &&
            read_be16(out_of_order_packets[index].bytes.data() + 10) == packet_id) {
            return &out_of_order_packets[index];
        }
    }
    return nullptr;
}

bool store_pending_atem_packet(const uint8_t* packet, size_t packet_size, uint16_t packet_id) {
    if (packet_size > kAtemBufferedPacketSize || find_pending_atem_packet(packet_id)) return false;
    for (size_t index = 0; index < out_of_order_packets.size(); ++index) {
        const uint32_t slot = uint32_t{1} << index;
        if (!(out_of_order_occupied & slot)) {
            memcpy(out_of_order_packets[index].bytes.data(), packet, packet_size);
            out_of_order_occupied |= slot;
            return true;
        }
    }
    return false;
}

void drain_pending_atem_packets() {
    while (true) {
        const uint16_t next_packet_id = static_cast<uint16_t>(
            (last_remote_packet_id + 1) % kAtemPacketIdModulus);
        PendingAtemPacket* pending = find_pending_atem_packet(next_packet_id);
        if (!pending) break;
        const uint16_t pending_packet_id = read_be16(pending->bytes.data() + 10);
        const size_t pending_packet_size =
            ((pending->bytes[0] & 0x07) << 8) | pending->bytes[1];
        last_remote_packet_id = pending_packet_id;
        process_ordered_atem_packet(pending->bytes.data(), pending_packet_size,
                                    pending_packet_id);
        const size_t slot_index = static_cast<size_t>(pending - out_of_order_packets.data());
        out_of_order_occupied &= ~(uint32_t{1} << slot_index);
    }
}

void discard_stale_pending_atem_packets() {
    for (size_t index = 0; index < out_of_order_packets.size(); ++index) {
        const uint32_t slot = uint32_t{1} << index;
        if (!(out_of_order_occupied & slot)) continue;
        const uint16_t packet_id = read_be16(out_of_order_packets[index].bytes.data() + 10);
        const uint16_t distance = static_cast<uint16_t>(
            (packet_id + kAtemPacketIdModulus - last_remote_packet_id) %
            kAtemPacketIdModulus);
        if (distance > kAtemPacketIdModulus / 2)
            out_of_order_occupied &= ~slot;
    }
}

bool skip_gap_to_packet(uint16_t packet_id, const char* reason) {
    const uint16_t distance = static_cast<uint16_t>(
        (packet_id + kAtemPacketIdModulus - last_remote_packet_id) %
        kAtemPacketIdModulus);
    if (distance <= 1 || distance > kAtemPacketIdModulus / 2) return false;

    ++atem_skipped_gap_count;
    const uint32_t now = millis_now();
    if (!last_gap_warning_ms || now - last_gap_warning_ms >= 1000) {
        ESP_LOGW(kTag, "Skipping ATEM sequence gap (%s): last=%u next=%u missing=%u",
                 reason, static_cast<unsigned>(last_remote_packet_id),
                 static_cast<unsigned>(packet_id), static_cast<unsigned>(distance - 1));
        last_gap_warning_ms = now;
    }
    last_remote_packet_id = static_cast<uint16_t>(
        (packet_id + kAtemPacketIdModulus - 1) % kAtemPacketIdModulus);
    return true;
}

bool skip_to_lowest_pending_packet(const char* reason) {
    PendingAtemPacket* lowest = nullptr;
    uint16_t lowest_distance = kAtemPacketIdModulus;
    for (size_t index = 0; index < out_of_order_packets.size(); ++index) {
        if (!(out_of_order_occupied & (uint32_t{1} << index))) continue;
        const uint16_t packet_id = read_be16(out_of_order_packets[index].bytes.data() + 10);
        const uint16_t distance = static_cast<uint16_t>(
            (packet_id + kAtemPacketIdModulus - last_remote_packet_id) %
            kAtemPacketIdModulus);
        if (distance > 0 && distance <= kAtemPacketIdModulus / 2 &&
            distance < lowest_distance) {
            lowest = &out_of_order_packets[index];
            lowest_distance = distance;
        }
    }
    if (!lowest) return false;

    const uint16_t packet_id = read_be16(lowest->bytes.data() + 10);
    skip_gap_to_packet(packet_id, reason);
    drain_pending_atem_packets();
    return true;
}

uint16_t lowest_pending_distance() {
    uint16_t lowest_distance = kAtemPacketIdModulus;
    for (size_t index = 0; index < out_of_order_packets.size(); ++index) {
        if (!(out_of_order_occupied & (uint32_t{1} << index))) continue;
        const uint16_t packet_id = read_be16(out_of_order_packets[index].bytes.data() + 10);
        const uint16_t distance = static_cast<uint16_t>(
            (packet_id + kAtemPacketIdModulus - last_remote_packet_id) %
            kAtemPacketIdModulus);
        if (distance > 0 && distance <= kAtemPacketIdModulus / 2 &&
            distance < lowest_distance)
            lowest_distance = distance;
    }
    return lowest_distance;
}

void update_out_of_order_gap_timer() {
    if (!out_of_order_occupied) {
        out_of_order_gap_started_ms = 0;
        return;
    }
    const uint16_t next_packet_id = static_cast<uint16_t>(
        (last_remote_packet_id + 1) % kAtemPacketIdModulus);
    if (find_pending_atem_packet(next_packet_id)) {
        out_of_order_gap_started_ms = 0;
    } else if (!out_of_order_gap_started_ms) {
        out_of_order_gap_started_ms = millis_now();
    }
}

void recover_timed_out_out_of_order_gap() {
    update_out_of_order_gap_timer();
    if (out_of_order_gap_started_ms &&
        millis_now() - out_of_order_gap_started_ms >= kOutOfOrderGapTimeoutMs) {
        skip_to_lowest_pending_packet("timeout");
        update_out_of_order_gap_timer();
    }
}

void process_atem_ack_request(const uint8_t* packet, size_t packet_size,
                              uint16_t remote_packet_id) {
    if (packet_size == 12 && remote_packet_id > 1) {
        init_payload_seen = true;
        init_last_packet_id = remote_packet_id;
    }
    if (kAckDuringInit || init_payload_seen)
        send_atem_packet(0x10, remote_packet_id, nullptr, 0);

    if (remote_packet_id >= kAtemPacketIdModulus) {
        if (should_log_debug_anomaly()) {
            ESP_LOGW(kTag, "Ignoring invalid ATEM packet sequence ID %u",
                     static_cast<unsigned>(remote_packet_id));
        }
        return;
    }
    if (!remote_sequence_initialized) {
        last_remote_packet_id = static_cast<uint16_t>((remote_packet_id + kAtemPacketIdModulus - 1) %
                                                      kAtemPacketIdModulus);
        remote_sequence_initialized = true;
    }

    const uint16_t distance = static_cast<uint16_t>(
        (remote_packet_id + kAtemPacketIdModulus - last_remote_packet_id) %
        kAtemPacketIdModulus);
    if (distance == 1) {
        last_remote_packet_id = remote_packet_id;
        process_ordered_atem_packet(packet, packet_size, remote_packet_id);
        drain_pending_atem_packets();
        update_out_of_order_gap_timer();
        return;
    }

    if (distance == 0 || distance > kAtemPacketIdModulus / 2) {
        ++atem_duplicate_packet_count;
        if (should_log_debug_anomaly()) {
            ESP_LOGW(kTag, "Duplicate ATEM packet %u; ACKed with its own ID",
                     static_cast<unsigned>(remote_packet_id));
        }
        return;
    }

    if (distance <= kAtemOutOfOrderCapacity) {
        ++atem_out_of_order_packet_count;
        const bool already_buffered = find_pending_atem_packet(remote_packet_id) != nullptr;
        if (already_buffered) {
            update_out_of_order_gap_timer();
            return;
        }

        const char* recovery_reason = nullptr;
        if (packet_size > kAtemBufferedPacketSize) {
            recovery_reason = "packet exceeds reorder storage";
        } else if (store_pending_atem_packet(packet, packet_size, remote_packet_id)) {
            update_out_of_order_gap_timer();
            return;
        } else {
            recovery_reason = "reorder buffer full";
        }

        if (distance < lowest_pending_distance()) {
            skip_gap_to_packet(remote_packet_id, recovery_reason);
            last_remote_packet_id = remote_packet_id;
            process_ordered_atem_packet(packet, packet_size, remote_packet_id);
            drain_pending_atem_packets();
            discard_stale_pending_atem_packets();
            update_out_of_order_gap_timer();
            return;
        }
        skip_to_lowest_pending_packet(recovery_reason);
    } else {
        ++atem_out_of_order_packet_count;
        if (distance < lowest_pending_distance()) {
            skip_gap_to_packet(remote_packet_id, "sequence distance exceeded buffer");
            last_remote_packet_id = remote_packet_id;
            process_ordered_atem_packet(packet, packet_size, remote_packet_id);
            drain_pending_atem_packets();
            discard_stale_pending_atem_packets();
            update_out_of_order_gap_timer();
            return;
        }
        skip_to_lowest_pending_packet("sequence distance exceeded buffer");
    }

    const uint16_t updated_distance = static_cast<uint16_t>(
        (remote_packet_id + kAtemPacketIdModulus - last_remote_packet_id) %
        kAtemPacketIdModulus);
    if (updated_distance == 0 || updated_distance > kAtemPacketIdModulus / 2) {
        ++atem_duplicate_packet_count;
    } else if (updated_distance == 1) {
        last_remote_packet_id = remote_packet_id;
        process_ordered_atem_packet(packet, packet_size, remote_packet_id);
        drain_pending_atem_packets();
    } else {
        skip_gap_to_packet(remote_packet_id, "unrecoverable sequence gap");
        last_remote_packet_id = remote_packet_id;
        process_ordered_atem_packet(packet, packet_size, remote_packet_id);
        drain_pending_atem_packets();
    }
    discard_stale_pending_atem_packets();
    update_out_of_order_gap_timer();
}

void process_atem_packet(const uint8_t* packet, size_t packet_size, const sockaddr_in& source) {
    if (packet_size < 12) {
        record_atem_packet("RX");
        if (should_log_debug_anomaly()) {
            ESP_LOGW(kTag, "Ignoring short ATEM UDP packet (%u bytes)",
                     static_cast<unsigned>(packet_size));
        }
        return;
    }
    const uint16_t declared_size = static_cast<uint16_t>(((packet[0] & 0x07) << 8) | packet[1]);
    if (declared_size != packet_size) {
        record_atem_packet("RX");
        if (should_log_debug_anomaly()) {
            ESP_LOGW(kTag, "Ignoring ATEM packet with declared size %u, received %u",
                     static_cast<unsigned>(declared_size), static_cast<unsigned>(packet_size));
        }
        return;
    }

    session_id = read_be16(packet + 2);
    const uint8_t flags = packet[0] >> 3;
    const uint16_t remote_packet_id = read_be16(packet + 10);
    last_atem_packet_ms = millis_now();

    if (flags & 0x02) {
        atem_connected = packet_size <= 12 || packet[12] != 3;
        clear_out_of_order_packets();
        last_remote_packet_id = remote_packet_id;
        remote_sequence_initialized = true;
        send_atem_packet(0x10, 0, nullptr, 0, 0x03);
        if (atem_connected) {
            set_state(3);
            ESP_LOGI(kTag, "ATEM switcher connected");
        } else {
            ESP_LOGE(kTag, "ATEM rejected the connection (status=%u; no free client slot)",
                     packet_size > 12 ? packet[12] : 0);
        }
        record_atem_packet("RX");
        return;
    }

    if (packet_size == 12 && remote_packet_id > 1) {
        init_payload_seen = true;
        init_last_packet_id = remote_packet_id;
    }

    if (!(flags & 0x02) && (flags & 0x01)) {
        process_atem_ack_request(packet, packet_size, remote_packet_id);
    }

    if ((flags & 0x08) && !(flags & 0x02)) {
        if (should_log_debug_anomaly()) {
            ESP_LOGW(kTag, "ATEM requested resend after sequence %u",
                     static_cast<unsigned>((packet[6] << 8) | packet[7]));
        }
        uint8_t response[12]{};
        response[0] = static_cast<uint8_t>(0x01 << 3);
        response[1] = 12;
        write_be16(response + 2, session_id);
        response[10] = packet[6];
        response[11] = packet[7];
        const int sent = sendto(atem_socket, response, sizeof(response), 0,
                                reinterpret_cast<const sockaddr*>(&switcher_address),
                                sizeof(switcher_address));
        if (sent == sizeof(response)) {
            record_atem_packet("TX");
        } else {
            record_atem_send_failure("ATEM retransmit response", errno);
        }
    }

    if (!(flags & (0x02 | 0x01)) && packet_size > 12 &&
               packet_size <= kAtemMaxPacketSize) {
        process_atem_payload(packet, packet_size);
    }
    record_atem_packet("RX");
}

bool open_atem_socket() {
    atem_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (atem_socket < 0) {
        ESP_LOGE(kTag, "Could not create ATEM UDP socket: errno %d", errno);
        return false;
    }

    sockaddr_in local_address{};
    local_address.sin_family = AF_INET;
    local_address.sin_port = htons(0);
    local_address.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(atem_socket, reinterpret_cast<sockaddr*>(&local_address), sizeof(local_address)) != 0) {
        ESP_LOGE(kTag, "Could not bind ATEM UDP socket: errno %d", errno);
        close(atem_socket);
        atem_socket = -1;
        return false;
    }

    timeval timeout{};
    timeout.tv_usec = 5000;
    setsockopt(atem_socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    switcher_address.sin_family = AF_INET;
    switcher_address.sin_port = htons(kAtemPort);
    if (inet_aton(tally_settings.switcher_ip, &switcher_address.sin_addr) == 0) {
        ESP_LOGE(kTag, "Invalid ATEM switcher IPv4 address: %s", tally_settings.switcher_ip);
        close(atem_socket);
        atem_socket = -1;
        return false;
    }
    sockaddr_in bound_address{};
    socklen_t bound_address_length = sizeof(bound_address);
    if (getsockname(atem_socket, reinterpret_cast<sockaddr*>(&bound_address),
                    &bound_address_length) != 0) {
        ESP_LOGE(kTag, "Could not read ATEM UDP local port: errno %d", errno);
        close(atem_socket);
        atem_socket = -1;
        return false;
    }
    ESP_LOGI(kTag, "ATEM UDP socket ready; local port %u, destination %s:%u",
             static_cast<unsigned>(ntohs(bound_address.sin_port)),
             tally_settings.switcher_ip, kAtemPort);
    last_atem_packet_ms = millis_now();
    return true;
}

void reset_atem_session(bool close_socket) {
    atem_connected = false;
    local_packet_id = 0;
    session_id = 0x53AB;
    last_remote_packet_id = 0;
    remote_sequence_initialized = false;
    init_payload_seen = false;
    waiting_for_init_packet = false;
    requested_init_packet_id = 0;
    init_request_sent_ms = 0;
    init_request_attempts = 0;
    atem_initialized = false;
    init_last_packet_id = 0;
    missed_init_packets.fill(0xFF);
    clear_out_of_order_packets();
    portENTER_CRITICAL(&tally_flags_mux);
    tally_flags.fill(0);
    portEXIT_CRITICAL(&tally_flags_mux);
    tally_server_reset_flags();
    stream_is_on_air = false;
    last_hello_ms = 0;
    last_atem_packet_ms = millis_now();
    output_refresh_pending.store(true);
    if (test_server_mode.load()) debug_tally_pending.store(true);
    if (close_socket && atem_socket >= 0) {
        close(atem_socket);
        atem_socket = -1;
    }
}

void report_atem_health() {
    wifi_ap_record_t access_point{};
    const int rssi = (xEventGroupGetBits(wifi_events) & kWifiConnected) &&
                     esp_wifi_sta_get_ap_info(&access_point) == ESP_OK
                         ? access_point.rssi
                         : -127;
    ESP_LOGI(kTag,
             "Health: heap=%u min_heap=%u largest_block=%u RSSI=%d dBm send_failures=%u ENOMEM=%u "
             "reconnects(timeout/wifi/manual)=%u/%u/%u ATEM_stack_highwater=%u "
             "ATEM_packets(rx/tx/duplicate/reordered)=%u/%u/%u/%u skipped_gaps=%u",
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_8BIT)),
             static_cast<unsigned>(esp_get_minimum_free_heap_size()),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)), rssi,
             static_cast<unsigned>(atem_send_failure_count),
             static_cast<unsigned>(atem_enomem_send_failure_count),
             static_cast<unsigned>(timeout_reconnect_count),
             static_cast<unsigned>(wifi_reconnect_count),
             static_cast<unsigned>(manual_reconnect_count),
             static_cast<unsigned>(atem_task_handle
                                      ? uxTaskGetStackHighWaterMark(atem_task_handle)
                                      : 0),
             static_cast<unsigned>(atem_rx_packet_count),
             static_cast<unsigned>(atem_tx_packet_count),
             static_cast<unsigned>(atem_duplicate_packet_count),
             static_cast<unsigned>(atem_out_of_order_packet_count),
             static_cast<unsigned>(atem_skipped_gap_count));
}

void run_atem_client() {
    static uint8_t packet[kAtemMaxPacketSize];
    if (!atem_connected && (last_hello_ms == 0 ||
                            millis_now() - last_hello_ms >= 1000)) {
        send_hello();
    }

    bool first_receive = true;
    size_t received_packets = 0;
    while (true) {
        sockaddr_in source{};
        socklen_t source_length = sizeof(source);
        const int flags = first_receive ? 0 : MSG_DONTWAIT;
        const int length = recvfrom(atem_socket, packet, sizeof(packet), flags,
                                    reinterpret_cast<sockaddr*>(&source), &source_length);
        first_receive = false;
        if (length <= 0) {
            if (length < 0 && errno != EAGAIN && errno != EWOULDBLOCK &&
                errno != EINTR && should_log_debug_anomaly()) {
                ESP_LOGW(kTag, "ATEM UDP receive failed: errno %d", errno);
            }
            break;
        }
        ++received_packets;

        if (source.sin_addr.s_addr != switcher_address.sin_addr.s_addr ||
            source.sin_port != switcher_address.sin_port) {
            if (should_log_debug_anomaly()) {
                char source_ip[INET_ADDRSTRLEN]{};
                inet_ntop(AF_INET, &source.sin_addr, source_ip, sizeof(source_ip));
                ESP_LOGW(kTag, "Ignoring UDP packet from unexpected source %s:%u",
                         source_ip, static_cast<unsigned>(ntohs(source.sin_port)));
            }
            if (received_packets >= kAtemReceiveBatchSize) {
                vTaskDelay(1);
                break;
            }
            continue;
        }
        process_atem_packet(packet, static_cast<size_t>(length), source);
        if (received_packets >= kAtemReceiveBatchSize) {
            vTaskDelay(1);
            break;
        }
    }

}

void atem_client_task(void*) {
    reset_atem_session(false);
    set_state(1);
    uint32_t last_health_report_ms = millis_now();
    uint32_t last_init_request_check_ms = millis_now();

    while (true) {
        if (wifi_lost.exchange(false)) {
            ++wifi_reconnect_count;
            reset_atem_session(true);
            set_state(1);
        }
        if (manual_reset_requested.exchange(false)) {
            ++manual_reconnect_count;
            reset_atem_session(false);
            set_state(2);
        }

        const EventBits_t bits = xEventGroupGetBits(wifi_events);
        if (bits & kWifiConnected) {
            if (atem_socket < 0 && !open_atem_socket()) {
                vTaskDelay(pdMS_TO_TICKS(1000));
            } else {
                if (!atem_connected) set_state(2);
                run_atem_client();
                const uint32_t now = millis_now();
                if (init_payload_seen && !atem_initialized &&
                    now - last_init_request_check_ms >= kInitRequestCheckMs) {
                    request_missing_initialization_packet();
                    last_init_request_check_ms = now;
                }
                recover_timed_out_out_of_order_gap();
                if (atem_socket >= 0 && now - last_atem_packet_ms >= 5000) {
                    ++timeout_reconnect_count;
                    ESP_LOGW(kTag, "No valid ATEM packet for 5000 ms; reconnecting");
                    reset_atem_session(true);
                    set_state(2);
                }
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(50));
        }

        const uint32_t now = millis_now();
        if (now - last_health_report_ms >= kHealthReportIntervalMs) {
            report_atem_health();
            last_health_report_ms = now;
        }
    }
}

}  // namespace

const char* tally_atem_connection_status() {
    if (!wifi_events || !(xEventGroupGetBits(wifi_events) & kWifiConnected)) return "Waiting for Wi-Fi";
    if (atem_connected) return "Connected";
    if (atem_socket >= 0) return "Connecting; waiting for ATEM response";
    return "ATEM socket unavailable";
}

void tally_refresh_outputs() {
    output_refresh_pending.store(true);
}

esp_err_t tally_test_server_set_enabled(bool enabled) {
    const bool previous = tally_settings.test_server_enabled;
    tally_settings.test_server_enabled = enabled;
    const esp_err_t result = tally_settings_save();
    if (result != ESP_OK) {
        tally_settings.test_server_enabled = previous;
        return result;
    }

    test_server_mode.store(enabled);
    if (enabled) {
        debug_tally_pending.store(true);
        return ESP_OK;
    }

    debug_flags.store(0);
    manual_reset_requested.store(true);
    output_refresh_pending.store(true);
    return ESP_OK;
}

esp_err_t tally_debug_logging_set_enabled(bool enabled) {
    debug_logging_started_ms.store(enabled ? millis_now() : 0);
    debug_logging_mode.store(enabled);
    return ESP_OK;
}

void tally_debug_set_test(uint8_t number, uint8_t state) {
    if (number < 1 || number > kMaxTallySources || state > 2) return;
    debug_number.store(number);
    const uint8_t flags = state == 2 ? kTallyProgram : (state == 1 ? kTallyPreview : 0);
    debug_flags.store(flags);
    if (test_server_mode.load()) debug_tally_pending.store(true);
}

bool tally_test_server_is_enabled() { return test_server_mode.load(); }
bool tally_debug_logging_is_enabled() { return debug_logging_mode.load(); }
uint8_t tally_debug_tally_number() { return debug_number.load(); }
uint8_t tally_debug_state() {
    const uint8_t flags = debug_flags.load();
    return (flags & kTallyProgram) ? 2 : ((flags & kTallyPreview) ? 1 : 0);
}

namespace {

void validate_stored_gpio_settings() {
    bool changed = false;
#if CONFIG_IDF_TARGET_ESP32C3
    if (tally_settings.battery_adc_gpio > 4) {
        ESP_LOGW(kTag, "Stored battery ADC GPIO %u is invalid for ESP32-C3; using default GPIO 3",
                 static_cast<unsigned>(tally_settings.battery_adc_gpio));
        tally_settings.battery_adc_gpio = 3;
        changed = true;
    }

    const uint8_t configured_neopixel_gpio = CONFIG_TALLY_NEOPIXEL_GPIO;
    if (tally_settings.neopixel_gpio > 21 ||
        (tally_settings.neopixel_gpio >= 12 && tally_settings.neopixel_gpio <= 21)) {
        ESP_LOGW(kTag, "Stored NeoPixel GPIO %u is unavailable on ESP32-C3; using default GPIO %u",
                 static_cast<unsigned>(tally_settings.neopixel_gpio),
                 static_cast<unsigned>(configured_neopixel_gpio));
        tally_settings.neopixel_gpio = configured_neopixel_gpio;
        changed = true;
    }
    if (tally_settings.neopixel_gpio == 2 || tally_settings.neopixel_gpio == 8 ||
        tally_settings.neopixel_gpio == 9) {
        ESP_LOGW(kTag, "NeoPixel GPIO %u is a strapping pin; verify external circuitry",
                 static_cast<unsigned>(tally_settings.neopixel_gpio));
    }
#endif
    if (changed) {
        const esp_err_t result = tally_settings_save();
        if (result != ESP_OK) {
            ESP_LOGE(kTag, "Could not save corrected GPIO settings: %s", esp_err_to_name(result));
        }
    }
}

}  // namespace

extern "C" void app_main(void) {
    ESP_LOGI(kTag, "Boot reset reason: %d", static_cast<int>(esp_reset_reason()));
    esp_err_t nvs_result = nvs_flash_init();
    if (nvs_result == ESP_ERR_NVS_NO_FREE_PAGES || nvs_result == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_result = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_result);

    tally_settings_init();
    validate_stored_gpio_settings();
    test_server_mode.store(tally_settings.test_server_enabled);
    const bool persisted_debug_logging = tally_settings.debug_logging_enabled;
    tally_settings.debug_logging_enabled = false;
    debug_logging_mode.store(false);
    debug_logging_started_ms.store(0);
    if (persisted_debug_logging) {
        const esp_err_t save_result = tally_settings_save();
        if (save_result != ESP_OK) {
            ESP_LOGE(kTag, "Could not clear persisted debug mode: %s", esp_err_to_name(save_result));
        }
    }
    debug_number.store(tally_settings.tally_number);
    debug_flags.store(0);
    debug_tally_pending.store(test_server_mode.load());
    tally_battery_monitor_init();
    initialize_strip();
    set_state(1);
    tally_battery_update();
    if (strip_initialized) show_strip();
    improv_serial_start();
    initialize_wifi();
    tally_server_start();
    if (xTaskCreate(atem_client_task, "atem_client", 6144, nullptr, 6,
                    &atem_task_handle) != pdPASS) {
        ESP_LOGE(kTag, "Could not start ATEM client task");
    }

    const uint32_t boot_ms = millis_now();
    if (soft_ap_active) {
        tally_web_server_start();
        web_server_started = true;
    }
    bool was_wifi_connected = false;
    while (true) {
        const EventBits_t bits = xEventGroupWaitBits(wifi_events, kWifiConnected, pdFALSE, pdTRUE,
                                pdMS_TO_TICKS(50));
        const uint32_t now = millis_now();
        if (debug_logging_mode.load() &&
            now - debug_logging_started_ms.load() >= kDebugAutoDisableMs) {
            debug_logging_mode.store(false);
            debug_logging_started_ms.store(0);
            ESP_LOGI(kTag, "Detailed debug logging automatically disabled after 10 minutes");
        }
        if (debug_tally_pending.exchange(false)) apply_debug_tally();
        const bool wifi_connected = (bits & kWifiConnected) != 0;
        if (wifi_connected && !was_wifi_connected) {
            was_wifi_connected = true;
            wifi_connected_since_ms = now;
        } else if (!wifi_connected) {
            was_wifi_connected = false;
            wifi_connected_since_ms = 0;
        }
        if (wifi_connected && fallback_ap_active &&
            now - wifi_connected_since_ms >= 120000) {
            const esp_err_t mode_result = esp_wifi_set_mode(WIFI_MODE_STA);
            if (mode_result == ESP_OK) {
                fallback_ap_active = false;
                soft_ap_active = false;
                ESP_LOGI(kTag, "Setup access point disabled after stable Wi-Fi connection");
            } else {
                ESP_LOGE(kTag, "Could not disable fallback access point: %s",
                         esp_err_to_name(mode_result));
                wifi_connected_since_ms = now;
            }
        }
        if (output_refresh_pending.exchange(false)) {
            update_tally_leds();
            if (strip_initialized) show_strip();
        }
        if (tally_settings.battery_monitor_enabled && now - last_battery_sample_ms >= 10000) {
            tally_battery_update();
            last_battery_sample_ms = now;
        }
        update_battery_warning(now);
        if ((bits & kWifiConnected) && !web_server_started) {
            tally_web_server_start();
            web_server_started = true;
        }
        if (!wifi_connected) {
            if (tally_settings.wifi_ssid[0] != '\0' && !fallback_ap_active &&
                millis_now() - boot_ms >= 10000) {
                ESP_LOGW(kTag, "Wi-Fi unavailable; enabling setup access point");
                const esp_err_t mode_result = esp_wifi_set_mode(WIFI_MODE_APSTA);
                if (mode_result != ESP_OK) {
                    ESP_LOGE(kTag, "Could not enable setup access point mode: %s",
                             esp_err_to_name(mode_result));
                } else {
                    wifi_config_t ap_config = make_setup_ap_config();
                    const esp_err_t config_result = esp_wifi_set_config(WIFI_IF_AP, &ap_config);
                    if (config_result != ESP_OK) {
                        ESP_LOGE(kTag, "Could not configure setup access point: %s",
                                 esp_err_to_name(config_result));
                    } else {
                        soft_ap_active = true;
                        fallback_ap_active = true;
                        if (!web_server_started) {
                            tally_web_server_start();
                            web_server_started = true;
                        }
                    }
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}