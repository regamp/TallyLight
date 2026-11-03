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
#include <vector>

#include "driver/rmt.h"
#include "esp_event.h"
#include "esp_log.h"
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
constexpr int kWifiConnected = BIT0;
constexpr uint32_t kInitRequestRetryMs = 1000;
constexpr uint8_t kInitRequestMaxAttempts = 3;

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

EventGroupHandle_t wifi_events;
esp_netif_t* station_netif = nullptr;
int atem_socket = -1;
sockaddr_in switcher_address{};
std::array<uint8_t, kMaxTallySources> tally_flags{};
uint16_t local_packet_id = 0;
uint16_t session_id = 0x53AB;
uint32_t last_atem_packet_ms = 0;
uint32_t last_hello_ms = 0;
uint32_t last_atem_status_log_ms = 0;
uint32_t last_battery_sample_ms = 0;
uint32_t battery_warning_started_ms = 0;
bool atem_connected = false;
bool stream_is_on_air = false;
uint8_t current_state = 0;
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
std::vector<Rgb> strip_pixels;
bool strip_initialized = false;
std::atomic<bool> debug_mode{false};
std::atomic<uint8_t> debug_number{1};
std::atomic<uint8_t> debug_flags{0};

uint16_t read_be16(const uint8_t* data) {
    return static_cast<uint16_t>((static_cast<uint16_t>(data[0]) << 8) | data[1]);
}

void log_atem_packet(const char* direction, const uint8_t* packet, size_t packet_size,
                     const sockaddr_in& endpoint) {
    if (!debug_mode.load()) return;

    char address[INET_ADDRSTRLEN]{};
    inet_ntop(AF_INET, &endpoint.sin_addr, address, sizeof(address));
    const uint8_t flags = packet_size > 0 ? packet[0] >> 3 : 0;
    const uint16_t session = packet_size >= 4 ? read_be16(packet + 2) : 0;
    const uint16_t header_id = packet_size >= 6 ? read_be16(packet + 4) : 0;
    const uint16_t packet_id = packet_size >= 12 ? read_be16(packet + 10) : 0;
    ESP_LOGI(kTag,
             "Test_server ATEM %s %s:%u len=%u flags=0x%02x session=0x%04x header_id=%u packet_id=%u",
             direction, address, static_cast<unsigned>(ntohs(endpoint.sin_port)),
             static_cast<unsigned>(packet_size), static_cast<unsigned>(flags),
             static_cast<unsigned>(session), static_cast<unsigned>(header_id),
             static_cast<unsigned>(packet_id));

    for (size_t offset = 0; offset < packet_size; offset += 16) {
        char hex[16 * 3 + 1]{};
        size_t used = 0;
        const size_t end = std::min(packet_size, offset + 16);
        for (size_t index = offset; index < end; ++index) {
            used += static_cast<size_t>(snprintf(hex + used, sizeof(hex) - used,
                                                "%02X%s", packet[index],
                                                index + 1 == end ? "" : " "));
        }
        ESP_LOGI(kTag, "ATEM %s bytes[%u..%u]: %s", direction,
                 static_cast<unsigned>(offset), static_cast<unsigned>(end - 1), hex);
    }
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
    if (strip_pixels.empty()) return strip_pixels.size();
    if (tally_settings.neopixel_status == 1) return 0;
    if (tally_settings.neopixel_status == 2) return strip_pixels.size() - 1;
    return tally_battery_is_low() ? 0 : strip_pixels.size();
}

bool is_battery_warning_pixel(size_t index) {
    return battery_warning_flash_active && !strip_pixels.empty() &&
           (index == 0 || index == strip_pixels.size() - 1);
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

void show_strip() {
    if (!strip_initialized || strip_pixels.empty()) return;
    const size_t status_index = status_pixel_index();
    std::vector<rmt_item32_t> items;
    items.reserve(strip_pixels.size() * 24 + 1);
    const int brightness = tally_settings.neopixel_brightness;
    for (size_t index = 0; index < strip_pixels.size(); ++index) {
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
                rmt_item32_t item{};
                const bool one = (value & (1U << bit)) != 0;
                item.level0 = 1;
                item.duration0 = one ? 32 : 14;
                item.level1 = 0;
                item.duration1 = one ? 14 : 32;
                items.push_back(item);
            }
        }
    }
    rmt_item32_t reset{};
    reset.level0 = 0;
    reset.duration0 = 1000;
    reset.level1 = 0;
    reset.duration1 = 1000;
    items.push_back(reset);
    ESP_ERROR_CHECK(rmt_write_items(RMT_CHANNEL_0, items.data(), items.size(), true));
}

void initialize_strip() {
    if (tally_settings.neopixel_count == 0 || tally_settings.neopixel_count > 1000) return;
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
    strip_pixels.resize(tally_settings.neopixel_count);
    strip_initialized = true;
    show_strip();
}

void set_strip_tally(Color color) {
    if (!strip_initialized) return;
    const size_t status_index = status_pixel_index();
    for (size_t index = 0; index < strip_pixels.size(); ++index) {
        if (index == status_index || is_battery_warning_pixel(index)) continue;
        strip_pixels[index] = color_value(color);
    }
    show_strip();
}

void set_state(uint8_t state) {
    if (current_state == state) return;
    current_state = state;
    if (strip_initialized) show_strip();
}

Color tally_color(uint8_t mode, uint8_t tally_flag) {
    if (mode == 4) return stream_is_on_air ? Color::Red : Color::Off;
    if (tally_flag & kTallyProgram) return Color::Red;
    if (mode != 3 && ((tally_flag & kTallyPreview) || mode == 2)) return Color::Green;
    return Color::Off;
}

void update_tally_leds() {
    if (debug_mode.load()) {
        const uint8_t flags = debug_flags.load();
        const Color color = (flags & kTallyProgram) ? Color::Red :
                            ((flags & kTallyPreview) ? Color::Green : Color::Off);
        set_strip_tally(color);
        return;
    }
    const unsigned source = tally_settings.tally_number - 1;
    const uint8_t flag = source < tally_flags.size() ? tally_flags[source] : 0;
    set_strip_tally(tally_color(tally_settings.led1_mode, flag));
}

void apply_debug_tally() {
    if (!debug_mode.load()) return;
    tally_flags.fill(0);
    const uint8_t number = debug_number.load();
    if (number >= 1 && number <= kMaxTallySources) tally_flags[number - 1] = debug_flags.load();
    tally_server_set_all_flags(kMaxTallySources, tally_flags.data());
    update_tally_leds();
}

void wifi_event_handler(void*, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        if (tally_settings.wifi_ssid[0] != '\0') ESP_ERROR_CHECK(esp_wifi_connect());
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_START) {
        tally_server_start();
        apply_debug_tally();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(wifi_events, kWifiConnected);
        atem_connected = false;
        tally_flags.fill(0);
        tally_server_reset_flags();
        set_state(1);
        ESP_LOGW(kTag, "Wi-Fi disconnected; reconnecting");
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        tally_server_start();
        apply_debug_tally();
        xEventGroupSetBits(wifi_events, kWifiConnected);
        const auto* event = static_cast<ip_event_got_ip_t*>(event_data);
        ESP_LOGI(kTag, "Wi-Fi connected; setup page at http://" IPSTR, IP2STR(&event->ip_info.ip));
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
    if (station_netif) ESP_ERROR_CHECK(esp_netif_set_hostname(station_netif, tally_settings.device_name));

    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_config));
    ESP_ERROR_CHECK(esp_wifi_set_ps(tally_settings.battery_monitor_enabled ? WIFI_PS_MIN_MODEM : WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, nullptr));

    wifi_config_t config{};
    strlcpy(reinterpret_cast<char*>(config.sta.ssid), tally_settings.wifi_ssid, sizeof(config.sta.ssid));
    strlcpy(reinterpret_cast<char*>(config.sta.password), tally_settings.wifi_password, sizeof(config.sta.password));
    config.sta.threshold.authmode = WIFI_AUTH_OPEN;

    if (tally_settings.static_ip && station_netif) {
        esp_netif_ip_info_t ip_info{};
        ip_info.ip.addr = tally_settings.tally_ip;
        ip_info.netmask.addr = tally_settings.subnet_mask;
        ip_info.gw.addr = tally_settings.gateway_ip;
        ESP_ERROR_CHECK(esp_netif_dhcpc_stop(station_netif));
        ESP_ERROR_CHECK(esp_netif_set_ip_info(station_netif, &ip_info));
    }

    const bool has_saved_wifi = tally_settings.wifi_ssid[0] != '\0';
    ESP_ERROR_CHECK(esp_wifi_set_mode(has_saved_wifi ? WIFI_MODE_STA : WIFI_MODE_AP));
    if (has_saved_wifi) {
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &config));
    } else {
        wifi_config_t ap_config = make_setup_ap_config();
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    }
    ESP_ERROR_CHECK(esp_wifi_start());
    if (!tally_settings.wifi_ssid[0]) soft_ap_active = true;
}

void send_atem_packet(uint8_t flags, uint16_t packet_id, const uint8_t* payload, size_t payload_size,
                      uint8_t header_byte_9 = 0, uint8_t header_byte_6 = 0,
                      uint8_t header_byte_7 = 0, uint8_t header_byte_8 = 0) {
    uint8_t packet[32]{};
    const size_t packet_size = 12 + payload_size;
    if (packet_size > sizeof(packet)) return;

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
        ESP_LOGW(kTag, "ATEM UDP send failed (flags 0x%02x): errno %d", flags, errno);
    } else if (static_cast<size_t>(sent) != packet_size) {
        ESP_LOGW(kTag, "ATEM UDP send was short: %d of %u bytes", sent,
                 static_cast<unsigned>(packet_size));
    } else {
        log_atem_packet("TX", packet, packet_size, switcher_address);
    }
}

void send_hello() {
    uint8_t payload[8]{};
    payload[0] = 0x01;
    send_atem_packet(0x02, 0, payload, sizeof(payload), 0x3A);
    last_hello_ms = millis_now();
}

void process_atem_payload(const uint8_t* packet, size_t packet_size) {
    size_t offset = 12;
    while (offset + 8 <= packet_size) {
        const uint16_t command_size = read_be16(packet + offset);
        if (command_size < 8 || offset + command_size > packet_size) break;
        const uint8_t* command = packet + offset;
        const size_t data_size = command_size - 8;
        if (debug_mode.load()) {
            ESP_LOGI(kTag, "ATEM command RX: %.4s size=%u data=%u",
                     command + 4, static_cast<unsigned>(command_size),
                     static_cast<unsigned>(data_size));
        }

        if (memcmp(command + 4, "TlIn", 4) == 0 && data_size >= 2) {
            const uint16_t sources = std::min<uint16_t>(read_be16(command + 8), kMaxTallySources);
            if (debug_mode.load()) {
                ESP_LOGI(kTag, "ATEM TlIn: %u sources; raw tally flags are in the packet dump",
                         static_cast<unsigned>(sources));
            }
            if (debug_mode.load()) {
                offset += command_size;
                continue;
            }
            const size_t count = std::min<size_t>(sources, data_size - 2);
            memset(tally_flags.data(), 0, tally_flags.size());
            memcpy(tally_flags.data(), command + 10, count);
            tally_server_set_sources(static_cast<uint8_t>(sources));
            for (uint8_t index = 0; index < sources; ++index)
                tally_server_set_flag(index, tally_flags[index]);
            update_tally_leds();
        } else if (memcmp(command + 4, "StRS", 4) == 0 && data_size >= 2) {
            const uint16_t stream_flags = read_be16(command + 8);
            stream_is_on_air = (stream_flags & (1U << 2)) != 0;
            if (debug_mode.load()) {
                ESP_LOGI(kTag, "ATEM StRS: flags=0x%04x on_air=%s",
                         static_cast<unsigned>(stream_flags), stream_is_on_air ? "yes" : "no");
            }
            update_tally_leds();
        }

        offset += command_size;
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
            if (init_request_attempts >= kInitRequestMaxAttempts) {
                ESP_LOGW(kTag, "ATEM init packet %u did not arrive after %u retries; continuing",
                         requested_init_packet_id, kInitRequestMaxAttempts);
                missed_init_packets[requested_init_packet_id >> 3] &=
                    static_cast<uint8_t>(~(1U << (requested_init_packet_id & 7)));
                waiting_for_init_packet = false;
                init_request_attempts = 0;
            } else {
                const uint16_t previous_id = requested_init_packet_id - 1;
                if (debug_mode.load()) {
                    ESP_LOGI(kTag, "Requesting missing ATEM init packet %u (attempt %u)",
                             requested_init_packet_id, init_request_attempts + 1);
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
            if (debug_mode.load()) {
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

void process_atem_packet(const uint8_t* packet, size_t packet_size, const sockaddr_in& source) {
    log_atem_packet("RX", packet, packet_size, source);
    if (packet_size < 12) {
        if (debug_mode.load()) {
            ESP_LOGW(kTag, "Ignoring short ATEM UDP packet (%u bytes)",
                     static_cast<unsigned>(packet_size));
        }
        return;
    }
    const uint16_t declared_size = static_cast<uint16_t>(((packet[0] & 0x07) << 8) | packet[1]);
    if (declared_size != packet_size) {
        if (debug_mode.load()) {
            ESP_LOGW(kTag, "Ignoring ATEM packet with declared size %u, received %u",
                     static_cast<unsigned>(declared_size), static_cast<unsigned>(packet_size));
        }
        return;
    }

    session_id = read_be16(packet + 2);
    const uint8_t flags = packet[0] >> 3;
    const uint16_t remote_packet_id = read_be16(packet + 10);
    last_atem_packet_ms = millis_now();
    if (remote_packet_id < 41) missed_init_packets[remote_packet_id >> 3] &= ~(1U << (remote_packet_id & 7));

    if (flags & 0x02) {
        atem_connected = packet_size <= 12 || packet[12] != 3;
        if (!atem_connected) {
            ESP_LOGE(kTag, "ATEM rejected the connection (status=%u; no free client slot)",
                     packet_size > 12 ? packet[12] : 0);
            return;
        }
        send_atem_packet(0x10, 0, nullptr, 0, 0x03);
        set_state(3);
        ESP_LOGI(kTag, "ATEM switcher connected");
    }

    if ((flags & 0x01) && !(flags & 0x04)) {
        if (debug_mode.load()) {
            ESP_LOGI(kTag, "Sending ACK for ATEM packet %u",
                     static_cast<unsigned>(remote_packet_id));
        }
        send_atem_packet(0x10, remote_packet_id, nullptr, 0);
    }
    if ((flags & 0x08) && !(flags & 0x02)) {
        if (debug_mode.load()) {
            ESP_LOGI(kTag, "ATEM requested resend after sequence %u",
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
            log_atem_packet("TX", response, sizeof(response), switcher_address);
        } else {
            ESP_LOGW(kTag, "ATEM resend response failed: sent=%d errno=%d", sent, errno);
        }
    }
    if (!init_payload_seen && packet_size == 12 && remote_packet_id > 1) {
        init_payload_seen = true;
        init_last_packet_id = remote_packet_id;
    }
    if (!(flags & 0x02) && packet_size > 12) process_atem_payload(packet, packet_size);
    request_missing_initialization_packet();
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
    timeout.tv_usec = 100000;
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
    return true;
}

void run_atem_client() {
    uint8_t packet[1500];
    if (!atem_connected && millis_now() - last_hello_ms >= 1000) send_hello();

    bool first_receive = true;
    while (true) {
        sockaddr_in source{};
        socklen_t source_length = sizeof(source);
        const int flags = first_receive ? 0 : MSG_DONTWAIT;
        const int length = recvfrom(atem_socket, packet, sizeof(packet), flags,
                                    reinterpret_cast<sockaddr*>(&source), &source_length);
        first_receive = false;
        if (length <= 0) {
            if (length < 0 && errno != EAGAIN && errno != EWOULDBLOCK &&
                errno != EINTR && debug_mode.load()) {
                ESP_LOGW(kTag, "ATEM UDP receive failed: errno %d", errno);
            }
            break;
        }

        if (source.sin_addr.s_addr != switcher_address.sin_addr.s_addr ||
            source.sin_port != switcher_address.sin_port) {
            if (debug_mode.load()) {
                char source_ip[INET_ADDRSTRLEN]{};
                inet_ntop(AF_INET, &source.sin_addr, source_ip, sizeof(source_ip));
                ESP_LOGW(kTag, "Ignoring UDP packet from unexpected source %s:%u",
                         source_ip, static_cast<unsigned>(ntohs(source.sin_port)));
            }
            continue;
        }
        process_atem_packet(packet, static_cast<size_t>(length), source);
    }

    const uint32_t now = millis_now();
    if (!atem_connected && last_hello_ms != 0 && now - last_hello_ms >= 5000 &&
        now - last_atem_status_log_ms >= 5000) {
        ESP_LOGW(kTag, "No response from ATEM at %s:%u; retrying hello every second",
                 tally_settings.switcher_ip, kAtemPort);
        last_atem_status_log_ms = now;
    }

    if (atem_connected && now - last_atem_packet_ms >= 5000) {
        ESP_LOGW(kTag, "ATEM connection timed out; reconnecting");
        atem_connected = false;
        tally_flags.fill(0);
        tally_server_reset_flags();
        atem_initialized = false;
        init_payload_seen = false;
        waiting_for_init_packet = false;
        init_request_attempts = 0;
        missed_init_packets.fill(0xFF);
        local_packet_id = 0;
        session_id = 0x53AB;
        last_hello_ms = 0;
        set_state(2);
        close(atem_socket);
        atem_socket = -1;
        ESP_LOGI(kTag, "ATEM socket closed; reconnect will use a new local UDP port");
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
    update_tally_leds();
    if (strip_initialized) show_strip();
}

void tally_debug_set_enabled(bool enabled) {
    debug_mode.store(enabled);
    tally_settings.debug_mode_enabled = enabled;
    const esp_err_t result = tally_settings_save();
    if (result != ESP_OK) ESP_LOGE(kTag, "Could not save DEBUG mode setting: %s", esp_err_to_name(result));
    if (enabled) {
        apply_debug_tally();
        return;
    }

    debug_flags.store(0);
    tally_server_reset_flags();
    tally_flags.fill(0);
    atem_connected = false;
    atem_initialized = false;
    init_payload_seen = false;
    waiting_for_init_packet = false;
    missed_init_packets.fill(0xFF);
    last_hello_ms = 0;
    update_tally_leds();
}

void tally_debug_set_test(uint8_t number, uint8_t state) {
    if (number < 1 || number > kMaxTallySources || state > 2) return;
    debug_number.store(number);
    const uint8_t flags = state == 2 ? kTallyProgram : (state == 1 ? kTallyPreview : 0);
    debug_flags.store(flags);
    if (debug_mode.load()) apply_debug_tally();
}

bool tally_debug_is_enabled() { return debug_mode.load(); }
uint8_t tally_debug_tally_number() { return debug_number.load(); }
uint8_t tally_debug_state() {
    const uint8_t flags = debug_flags.load();
    return (flags & kTallyProgram) ? 2 : ((flags & kTallyPreview) ? 1 : 0);
}

extern "C" void app_main(void) {
    esp_err_t nvs_result = nvs_flash_init();
    if (nvs_result == ESP_ERR_NVS_NO_FREE_PAGES || nvs_result == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_result = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_result);

    tally_settings_init();
    debug_mode.store(tally_settings.debug_mode_enabled);
    debug_number.store(tally_settings.tally_number);
    debug_flags.store(0);
    missed_init_packets.fill(0xFF);
    tally_battery_monitor_init();
    initialize_strip();
    set_state(1);
    tally_battery_update();
    if (strip_initialized) show_strip();
    improv_serial_start();
    initialize_wifi();

    const uint32_t boot_ms = millis_now();
    if (soft_ap_active) {
        tally_web_server_start();
        web_server_started = true;
    }
    while (true) {
        const EventBits_t bits = xEventGroupWaitBits(wifi_events, kWifiConnected, pdFALSE, pdTRUE,
                                pdMS_TO_TICKS(50));
        const uint32_t now = millis_now();
        if (tally_settings.battery_monitor_enabled && now - last_battery_sample_ms >= 10000) {
            tally_battery_update();
            last_battery_sample_ms = now;
            if (strip_initialized) show_strip();
        }
        update_battery_warning(now);
        if ((bits & kWifiConnected) && !web_server_started) {
            tally_web_server_start();
            web_server_started = true;
        }
        if (!(bits & kWifiConnected)) {
            if (!soft_ap_active && millis_now() - boot_ms >= 10000) {
                ESP_LOGW(kTag, "Wi-Fi unavailable; enabling setup access point");
                ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
                wifi_config_t ap_config = make_setup_ap_config();
                ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
                soft_ap_active = true;
                if (!web_server_started) {
                    tally_web_server_start();
                    web_server_started = true;
                }
            }
            continue;
        }

        if (atem_socket < 0 && !open_atem_socket()) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        if (!atem_connected) set_state(2);
        run_atem_client();
        if (atem_connected) update_tally_leds();
    }
}