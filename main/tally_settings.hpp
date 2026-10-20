#pragma once

#include <cstdint>
#include "esp_err.h"

struct TallySettings {
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
    bool debug_mode_enabled;
    uint8_t neopixel_color_order;
};

extern TallySettings tally_settings;

void tally_settings_init();
esp_err_t tally_settings_save();
void tally_web_server_start();
bool tally_battery_monitor_init();
uint16_t tally_battery_update();
uint16_t tally_battery_voltage_mv();
bool tally_battery_is_low();
const char* tally_atem_connection_status();
void tally_debug_set_enabled(bool enabled);
void tally_debug_set_test(uint8_t tally_number, uint8_t state);
bool tally_debug_is_enabled();
uint8_t tally_debug_tally_number();
uint8_t tally_debug_state();