#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "nvs.h"
#include "tally_settings.hpp"

namespace {

constexpr char kTag[] = "tally_web";

struct LegacyTallySettings {
    char device_name[32];
    char wifi_ssid[33];
    char wifi_password[65];
    char switcher_ip[16];
    uint8_t tally_number;
    uint8_t led1_mode;
    uint8_t led2_mode;
    uint8_t reserved_legacy_led_brightness;
    uint16_t neopixel_count;
    uint8_t neopixel_status;
    uint8_t neopixel_brightness;
    uint8_t neopixel_gpio;
    bool static_ip;
    uint32_t tally_ip;
    uint32_t subnet_mask;
    uint32_t gateway_ip;
};

struct PreviousTallySettings {
    char device_name[32];
    char wifi_ssid[33];
    char wifi_password[65];
    char switcher_ip[16];
    uint8_t tally_number;
    uint8_t led1_mode;
    uint8_t led2_mode;
    uint8_t reserved_legacy_led_brightness;
    uint16_t neopixel_count;
    uint8_t neopixel_status;
    uint8_t neopixel_brightness;
    uint8_t neopixel_gpio;
    bool static_ip;
    uint32_t tally_ip;
    uint32_t subnet_mask;
    uint32_t gateway_ip;
    bool battery_monitor_enabled;
    uint8_t battery_adc_gpio;
    uint16_t battery_low_mv;
    uint16_t battery_scale_milli;
};

std::string html_escape(const char* text) {
    std::string escaped;
    for (const char* p = text; *p; ++p) {
        switch (*p) {
            case '&': escaped += "&amp;"; break;
            case '<': escaped += "&lt;"; break;
            case '>': escaped += "&gt;"; break;
            case '"': escaped += "&quot;"; break;
            case '\'': escaped += "&#39;"; break;
            default: escaped += *p;
        }
    }
    return escaped;
}

std::string ip_to_string(uint32_t address) {
    in_addr value{};
    value.s_addr = address;
    char buffer[INET_ADDRSTRLEN]{};
    inet_ntop(AF_INET, &value, buffer, sizeof(buffer));
    return buffer;
}

std::string field(const char* label, const char* name, const char* value,
                  const char* type = "text", int max_length = 64) {
    char row[320];
    snprintf(row, sizeof(row),
             "<label>%s<input type='%s' name='%s' value='%s' maxlength='%d' required></label>",
             label, type, name, html_escape(value).c_str(), max_length);
    return row;
}

std::string select_field(const char* label, const char* name, uint8_t value,
                         bool modes) {
    const char* mode_names[] = {"Normal", "Preview stay on", "Program only", "On Air"};
    const char* status_names[] = {"First LED", "Last LED", "None"};
    std::string html = "<label>" + std::string(label) + "<select name='" + name + "'>";
    const int option_count = modes ? 4 : 3;
    for (int index = 0; index < option_count; ++index) {
        const int option_value = modes ? index + 1 : index + 1;
        const char* option_name = modes ? mode_names[index] : status_names[index];
        html += "<option value='" + std::to_string(option_value) + "'";
        if (value == option_value) html += " selected";
        html += ">" + std::string(option_name) + "</option>";
    }
    html += "</select></label>";
    return html;
}

esp_err_t root_handler(httpd_req_t* request) {
    esp_netif_ip_info_t ip_info{};
    esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif) esp_netif_get_ip_info(netif, &ip_info);

    std::string page = R"HTML(<!doctype html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>Tally Light setup</title><style>
body{font:16px system-ui,sans-serif;margin:0;background:#edf1ed;color:#182b2a}header{background:#163f3a;color:#fff;padding:18px max(20px,calc((100% - 780px)/2))}main{max-width:780px;margin:24px auto;padding:0 16px}section{border-top:1px solid #b8c8c2;padding:14px 0}h1{font-size:24px;margin:0}h2{font-size:17px}form{display:grid;grid-template-columns:repeat(auto-fit,minmax(230px,1fr));gap:14px}label{display:grid;gap:5px;font-size:14px}input,select{box-sizing:border-box;width:100%;padding:9px;border:1px solid #9aaca6;border-radius:4px;background:white;color:#182b2a}input[type=range]{padding:0;accent-color:#b7482d}input[type=checkbox]{width:auto}button{padding:10px 16px;background:#b7482d;color:white;border:0;border-radius:4px;font-weight:700;cursor:pointer}small{color:#536963}@media(max-width:500px){main{margin:12px auto}}
</style></head><body><header><h1>Tally Light setup</h1></header><main>)HTML";
    page += "<section><h2>Status</h2><p>Device IP: " + ip_to_string(ip_info.ip.addr) + "</p>";
    page += "<p>Wi-Fi: " + html_escape(tally_settings.wifi_ssid) + "</p>";
    page += "<p>ATEM switcher: " + html_escape(tally_settings.switcher_ip) + "</p>";
    page += "<p>ATEM connection: " + html_escape(tally_atem_connection_status()) + "</p>";
    page += "<p>DEBUG tally: " + std::string(tally_debug_is_enabled() ? "Enabled" : "Disabled") + "</p>";
    if (tally_debug_is_enabled()) {
        const char* debug_state_names[] = {"Off", "Preview", "Program"};
        page += "<p>DEBUG source: " + std::to_string(tally_debug_tally_number()) + " / " +
                debug_state_names[tally_debug_state()] + "</p>";
    }
    if (tally_settings.battery_monitor_enabled) {
        const uint16_t voltage_mv = tally_battery_voltage_mv();
        page += "<p>LiPo voltage: " + std::to_string(voltage_mv / 1000) + ".";
        if (voltage_mv % 1000 < 100) page += "0";
        if (voltage_mv % 1000 < 10) page += "0";
        page += std::to_string(voltage_mv % 1000) + " V";
        page += tally_battery_is_low() ? " (low)" : "";
        page += "</p>";
    } else {
        page += "<p>LiPo monitoring: Disabled</p>";
    }
    page += "</section>";
    page += "<form action='/save' method='post'><section><h2>Device</h2>";
    page += field("Tally Light name", "tName", tally_settings.device_name, "text", 31);
    page += "<label>Tally number (1-41)<input name='tNo' type='number' min='1' max='41' value='" +
            std::to_string(tally_settings.tally_number) + "' required></label>";
    page += select_field("LED 1 mode", "tModeLED1", tally_settings.led1_mode, true);
    page += select_field("LED 2 mode", "tModeLED2", tally_settings.led2_mode, true);
    page += "</section>";
    page += "<section><h2>Addressable LED strip</h2>";
    page += "<label>Neopixel count<input name='neoPxAmount' type='number' min='0' max='1000' value='" +
            std::to_string(tally_settings.neopixel_count) + "' required></label>";
    page += select_field("Status LED position", "neoPxStatus", tally_settings.neopixel_status, false);
        page += "<label>Neopixel brightness <output id='neoPxBrightValue'>" +
            std::to_string(tally_settings.neopixel_brightness) +
            "</output><input name='neoPxBright' type='range' min='0' max='255' value='" +
            std::to_string(tally_settings.neopixel_brightness) +
            "' oninput=\"document.getElementById('neoPxBrightValue').value=this.value\" required></label>";
    page += "<label>WS2812B colour order<select name='neoPxOrder'>";
    const char* color_orders[] = {"GRB", "RGB", "BRG", "RBG", "BGR", "GBR"};
    for (uint8_t order = 0; order < 6; ++order) {
        page += "<option value='" + std::to_string(order) + "'";
        if (tally_settings.neopixel_color_order == order) page += " selected";
        page += ">" + std::string(color_orders[order]) + "</option>";
    }
    page += "</select></label>";
        page += "<label>WS2812B data GPIO<input name='neoPxGPIO' type='number' min='0' max='" +
#ifdef CONFIG_IDF_TARGET_ESP32C3
                std::string("21")
#else
                std::string("48")
#endif
            + "' value='" + std::to_string(tally_settings.neopixel_gpio) + "' required></label></section>";
    page += "<section><h2>LiPo battery</h2>";
    page += "<input type='hidden' name='batteryEnabled' value='false'><label><span>Enable battery monitor</span><input name='batteryEnabled' type='checkbox' value='true'";
    if (tally_settings.battery_monitor_enabled) page += " checked";
    page += "></label>";
    page += "<label>Battery ADC GPIO<input name='batteryGPIO' type='number' min='0' max='";
#ifdef CONFIG_IDF_TARGET_ESP32C3
    page += "21";
#else
    page += "48";
#endif
    page += "' value='" + std::to_string(tally_settings.battery_adc_gpio) + "' required></label>";
    page += "<label>Low battery threshold (mV)<input name='batteryLowMv' type='number' min='2500' max='4500' value='" +
            std::to_string(tally_settings.battery_low_mv) + "' required></label>";
    page += "<label>Voltage divider ratio (e.g. 2.000)<input name='batteryScale' type='number' min='1000' max='10000' step='1' value='" +
            std::to_string(tally_settings.battery_scale_milli) + "' required></label></section>";
    page += "<section><h2>Network</h2>";
    page += field("Wi-Fi SSID", "ssid", tally_settings.wifi_ssid, "text", 32);
    page += "<label>Wi-Fi password (leave blank to keep current)<input type='password' name='pwd' maxlength='64'></label>";
    page += field("ATEM switcher IP", "aIP", tally_settings.switcher_ip, "text", 15);
    page += "<input type='hidden' name='staticIP' value='false'><label><span>Use static IP</span><input name='staticIP' type='checkbox' value='true'";
    if (tally_settings.static_ip) page += " checked";
    page += "></label>";
    page += field("Tally IP", "tIP", ip_to_string(tally_settings.tally_ip).c_str(), "text", 15);
    page += field("Subnet mask", "mask", ip_to_string(tally_settings.subnet_mask).c_str(), "text", 15);
    page += field("Gateway", "gate", ip_to_string(tally_settings.gateway_ip).c_str(), "text", 15);
    page += "</section><button type='submit'>Save and restart</button></form>";
    page += "<section><h2>DEBUG tally source</h2><form action='/debug' method='post'>";
    page += "<input type='hidden' name='enabled' value='false'><label><span>Enable DEBUG tally mode</span><input name='enabled' type='checkbox' value='true'";
    if (tally_debug_is_enabled()) page += " checked";
    page += "></label>";
    page += "<label>Test tally number (1-41)<input name='number' type='number' min='1' max='41' value='" +
            std::to_string(tally_debug_tally_number()) + "' required></label>";
    page += "<label>Test tally state<select name='state'>";
    const char* debug_states[] = {"Off", "Preview", "Program"};
    for (uint8_t state = 0; state < 3; ++state) {
        page += "<option value='" + std::to_string(state) + "'";
        if (tally_debug_state() == state) page += " selected";
        page += ">" + std::string(debug_states[state]) + "</option>";
    }
    page += "</select></label><button type='submit'>Apply DEBUG tally</button></form></section>";
    page += "<small>Setup page served by the tally light.</small></main></body></html>";

    httpd_resp_set_type(request, "text/html; charset=utf-8");
    return httpd_resp_send(request, page.c_str(), page.size());
}

bool url_decode(const char* input, std::string& output) {
    output.clear();
    for (size_t index = 0; input[index] != '\0'; ++index) {
        if (input[index] == '+') {
            output += ' ';
        } else if (input[index] == '%') {
            if (input[index + 1] == '\0' || input[index + 2] == '\0') return false;
            if (!isxdigit(static_cast<unsigned char>(input[index + 1])) ||
                !isxdigit(static_cast<unsigned char>(input[index + 2]))) return false;
            char hex[] = {input[index + 1], input[index + 2], '\0'};
            output += static_cast<char>(strtol(hex, nullptr, 16));
            index += 2;
        } else {
            output += input[index];
        }
    }
    return true;
}

bool parse_form(const char* body, TallySettings& settings) {
    char* copy = strdup(body);
    if (!copy) return false;
    bool valid = true;
    for (char* token = strtok(copy, "&"); token && valid; token = strtok(nullptr, "&")) {
        char* separator = strchr(token, '=');
        if (!separator) continue;
        *separator++ = '\0';
        std::string key;
        std::string value;
        valid = url_decode(token, key) && url_decode(separator, value);
        if (!valid) break;

        if (key == "tName") strlcpy(settings.device_name, value.c_str(), sizeof(settings.device_name));
        else if (key == "tNo") settings.tally_number = static_cast<uint8_t>(std::clamp(atoi(value.c_str()), 1, 41));
        else if (key == "tModeLED1") settings.led1_mode = static_cast<uint8_t>(std::clamp(atoi(value.c_str()), 1, 4));
        else if (key == "tModeLED2") settings.led2_mode = static_cast<uint8_t>(std::clamp(atoi(value.c_str()), 1, 4));
        else if (key == "neoPxAmount") settings.neopixel_count = static_cast<uint16_t>(std::clamp(atoi(value.c_str()), 0, 1000));
        else if (key == "neoPxStatus") settings.neopixel_status = static_cast<uint8_t>(std::clamp(atoi(value.c_str()), 1, 3));
        else if (key == "neoPxBright") settings.neopixel_brightness = static_cast<uint8_t>(std::clamp(atoi(value.c_str()), 0, 255));
        else if (key == "neoPxOrder") settings.neopixel_color_order = static_cast<uint8_t>(std::clamp(atoi(value.c_str()), 0, 5));
        else if (key == "neoPxGPIO") {
    #ifdef CONFIG_IDF_TARGET_ESP32C3
            constexpr int max_gpio = 21;
    #else
            constexpr int max_gpio = 48;
    #endif
            const int gpio = atoi(value.c_str());
            if (gpio < 0 || gpio > max_gpio) valid = false;
            else settings.neopixel_gpio = static_cast<uint8_t>(gpio);
        }
        else if (key == "batteryEnabled") settings.battery_monitor_enabled = value == "true";
        else if (key == "batteryGPIO") {
#ifdef CONFIG_IDF_TARGET_ESP32C3
            constexpr int max_gpio = 21;
#else
            constexpr int max_gpio = 48;
#endif
            const int gpio = atoi(value.c_str());
            if (gpio < 0 || gpio > max_gpio) valid = false;
            else settings.battery_adc_gpio = static_cast<uint8_t>(gpio);
        }
        else if (key == "batteryLowMv") settings.battery_low_mv = static_cast<uint16_t>(std::clamp(atoi(value.c_str()), 2500, 4500));
        else if (key == "batteryScale") settings.battery_scale_milli = static_cast<uint16_t>(std::clamp(atoi(value.c_str()), 1000, 10000));
        else if (key == "ssid") strlcpy(settings.wifi_ssid, value.c_str(), sizeof(settings.wifi_ssid));
        else if (key == "pwd" && !value.empty()) strlcpy(settings.wifi_password, value.c_str(), sizeof(settings.wifi_password));
        else if (key == "aIP") strlcpy(settings.switcher_ip, value.c_str(), sizeof(settings.switcher_ip));
        else if (key == "staticIP") settings.static_ip = value == "true";
        else if (key == "tIP") settings.tally_ip = inet_addr(value.c_str());
        else if (key == "mask") settings.subnet_mask = inet_addr(value.c_str());
        else if (key == "gate") settings.gateway_ip = inet_addr(value.c_str());
    }
    free(copy);
    return valid && settings.wifi_ssid[0] != '\0';
}

bool requires_restart(const TallySettings& old_settings, const TallySettings& new_settings) {
    return strcmp(old_settings.device_name, new_settings.device_name) != 0 ||
           strcmp(old_settings.wifi_ssid, new_settings.wifi_ssid) != 0 ||
           strcmp(old_settings.wifi_password, new_settings.wifi_password) != 0 ||
           strcmp(old_settings.switcher_ip, new_settings.switcher_ip) != 0 ||
           old_settings.static_ip != new_settings.static_ip ||
           old_settings.tally_ip != new_settings.tally_ip ||
           old_settings.subnet_mask != new_settings.subnet_mask ||
           old_settings.gateway_ip != new_settings.gateway_ip ||
           old_settings.neopixel_count != new_settings.neopixel_count ||
           old_settings.neopixel_status != new_settings.neopixel_status ||
           old_settings.neopixel_gpio != new_settings.neopixel_gpio ||
           old_settings.battery_monitor_enabled != new_settings.battery_monitor_enabled ||
           old_settings.battery_adc_gpio != new_settings.battery_adc_gpio;
}

esp_err_t debug_handler(httpd_req_t* request) {
    if (request->method != HTTP_POST || request->content_len <= 0 || request->content_len > 128) {
        httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Invalid DEBUG request");
        return ESP_FAIL;
    }
    char body[129]{};
    size_t received = 0;
    while (received < static_cast<size_t>(request->content_len)) {
        const int count = httpd_req_recv(request, body + received,
                                         std::min(sizeof(body) - received - 1,
                                                  static_cast<size_t>(request->content_len) - received));
        if (count <= 0) {
            httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Could not read DEBUG request");
            return ESP_FAIL;
        }
        received += static_cast<size_t>(count);
    }

    int number = tally_debug_tally_number();
    int state = tally_debug_state();
    bool enabled = false;
    bool enabled_value_seen = false;
    char* saveptr = nullptr;
    for (char* token = strtok_r(body, "&", &saveptr); token; token = strtok_r(nullptr, "&", &saveptr)) {
        char* equal = strchr(token, '=');
        if (!equal) continue;
        *equal++ = '\0';
        if (strcmp(token, "number") == 0) number = atoi(equal);
        else if (strcmp(token, "state") == 0) state = atoi(equal);
        else if (strcmp(token, "enabled") == 0) {
            enabled = strcmp(equal, "true") == 0;
            enabled_value_seen = true;
        }
    }
    if (!enabled_value_seen) enabled = false;
    if (number < 1 || number > 41 || state < 0 || state > 2) {
        httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Tally number must be 1-41; state must be 0-2");
        return ESP_FAIL;
    }

    if (enabled != tally_debug_is_enabled()) tally_debug_set_enabled(enabled);
    tally_debug_set_test(static_cast<uint8_t>(number), static_cast<uint8_t>(state));
    httpd_resp_set_status(request, "303 See Other");
    httpd_resp_set_hdr(request, "Location", "/");
    return httpd_resp_send(request, nullptr, 0);
}

esp_err_t save_handler(httpd_req_t* request) {
    if (request->method != HTTP_POST || request->content_len <= 0 || request->content_len > 2048) {
        httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Invalid settings request");
        return ESP_FAIL;
    }
    std::string body(static_cast<size_t>(request->content_len), '\0');
    size_t received = 0;
    while (received < body.size()) {
        const int length = httpd_req_recv(request, &body[received], body.size() - received);
        if (length <= 0) {
            httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "Could not read settings");
            return ESP_FAIL;
        }
        received += static_cast<size_t>(length);
    }

    const TallySettings previous = tally_settings;
    TallySettings updated = previous;
    if (!parse_form(body.c_str(), updated)) {
        httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Invalid form values");
        return ESP_FAIL;
    }
    tally_settings = updated;
    const esp_err_t result = tally_settings_save();
    if (result != ESP_OK) {
        httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "Could not save settings");
        return result;
    }

    if (requires_restart(previous, updated)) {
        httpd_resp_set_type(request, "text/html; charset=utf-8");
        httpd_resp_sendstr(request, "<!doctype html><html><body><p>Settings saved. Restarting to apply hardware or network changes...</p></body></html>");
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
        return ESP_OK;
    }

    tally_refresh_outputs();
    httpd_resp_set_status(request, "303 See Other");
    httpd_resp_set_hdr(request, "Location", "/");
    return httpd_resp_send(request, nullptr, 0);
}

}  // namespace

void tally_settings_init() {
    memset(&tally_settings, 0, sizeof(tally_settings));
    strlcpy(tally_settings.device_name, "Tally Light", sizeof(tally_settings.device_name));
    strlcpy(tally_settings.wifi_ssid, CONFIG_TALLY_WIFI_SSID, sizeof(tally_settings.wifi_ssid));
    strlcpy(tally_settings.wifi_password, CONFIG_TALLY_WIFI_PASSWORD, sizeof(tally_settings.wifi_password));
    strlcpy(tally_settings.switcher_ip, CONFIG_TALLY_SWITCHER_IP, sizeof(tally_settings.switcher_ip));
    tally_settings.tally_number = CONFIG_TALLY_SOURCE_NUMBER;
    tally_settings.led1_mode = CONFIG_TALLY_LED1_MODE;
    tally_settings.led2_mode = CONFIG_TALLY_LED2_MODE;
    tally_settings.reserved_legacy_led_brightness = 255;
    tally_settings.neopixel_status = 2;
    tally_settings.neopixel_brightness = 255;
    tally_settings.neopixel_gpio = CONFIG_TALLY_NEOPIXEL_GPIO;
    tally_settings.battery_monitor_enabled = false;
    tally_settings.battery_adc_gpio = 3;
    tally_settings.battery_low_mv = 3500;
    tally_settings.battery_scale_milli = 2000;
    tally_settings.debug_mode_enabled = false;
    tally_settings.neopixel_color_order = 0;

    nvs_handle_t handle;
    if (nvs_open("tally", NVS_READONLY, &handle) != ESP_OK) return;
    size_t size = 0;
    esp_err_t result = nvs_get_blob(handle, "settings", nullptr, &size);
    if (result == ESP_OK && size == sizeof(tally_settings)) {
        result = nvs_get_blob(handle, "settings", &tally_settings, &size);
    } else if (result == ESP_OK && size == sizeof(PreviousTallySettings)) {
        PreviousTallySettings previous{};
        result = nvs_get_blob(handle, "settings", &previous, &size);
        if (result == ESP_OK) {
            memcpy(&tally_settings, &previous, sizeof(previous));
            tally_settings.debug_mode_enabled = false;
            tally_settings.neopixel_color_order = 0;
        }
    } else if (result == ESP_OK && size == sizeof(LegacyTallySettings)) {
        LegacyTallySettings legacy{};
        result = nvs_get_blob(handle, "settings", &legacy, &size);
        if (result == ESP_OK) {
            memcpy(&tally_settings, &legacy, sizeof(legacy));
            tally_settings.battery_monitor_enabled = false;
            tally_settings.battery_adc_gpio = 3;
            tally_settings.battery_low_mv = 3500;
            tally_settings.battery_scale_milli = 2000;
            tally_settings.debug_mode_enabled = false;
            tally_settings.neopixel_color_order = 0;
        }
    }
    if (result != ESP_OK || (size != sizeof(tally_settings) && size != sizeof(PreviousTallySettings) && size != sizeof(LegacyTallySettings))) {
        strlcpy(tally_settings.device_name, "Tally Light", sizeof(tally_settings.device_name));
        strlcpy(tally_settings.wifi_ssid, CONFIG_TALLY_WIFI_SSID, sizeof(tally_settings.wifi_ssid));
        strlcpy(tally_settings.wifi_password, CONFIG_TALLY_WIFI_PASSWORD, sizeof(tally_settings.wifi_password));
        strlcpy(tally_settings.switcher_ip, CONFIG_TALLY_SWITCHER_IP, sizeof(tally_settings.switcher_ip));
        tally_settings.tally_number = CONFIG_TALLY_SOURCE_NUMBER;
        tally_settings.led1_mode = CONFIG_TALLY_LED1_MODE;
        tally_settings.led2_mode = CONFIG_TALLY_LED2_MODE;
        tally_settings.reserved_legacy_led_brightness = 255;
        tally_settings.neopixel_status = 2;
        tally_settings.neopixel_brightness = 255;
        tally_settings.neopixel_gpio = CONFIG_TALLY_NEOPIXEL_GPIO;
        tally_settings.battery_monitor_enabled = false;
        tally_settings.battery_adc_gpio = 3;
        tally_settings.battery_low_mv = 3500;
        tally_settings.battery_scale_milli = 2000;
        tally_settings.debug_mode_enabled = false;
        tally_settings.neopixel_color_order = 0;
    }
    nvs_close(handle);

    if (size == sizeof(LegacyTallySettings) || size == sizeof(PreviousTallySettings)) tally_settings_save();
}

esp_err_t tally_settings_save() {
    nvs_handle_t handle;
    esp_err_t result = nvs_open("tally", NVS_READWRITE, &handle);
    if (result != ESP_OK) return result;
    result = nvs_set_blob(handle, "settings", &tally_settings, sizeof(tally_settings));
    if (result == ESP_OK) result = nvs_commit(handle);
    nvs_close(handle);
    return result;
}

void tally_web_server_start() {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 5;
    httpd_handle_t server = nullptr;
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(kTag, "Could not start HTTP server");
        return;
    }

    const httpd_uri_t root = {.uri = "/", .method = HTTP_GET, .handler = root_handler, .user_ctx = nullptr};
    const httpd_uri_t save = {.uri = "/save", .method = HTTP_POST, .handler = save_handler, .user_ctx = nullptr};
    const httpd_uri_t debug = {.uri = "/debug", .method = HTTP_POST, .handler = debug_handler, .user_ctx = nullptr};
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &root));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &save));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &debug));
    ESP_LOGI(kTag, "Setup web server started");
}