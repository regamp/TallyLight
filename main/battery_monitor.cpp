#include <algorithm>
#include <atomic>
#include <cstdint>

#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "tally_settings.hpp"

namespace {

constexpr char kTag[] = "battery";
constexpr int kSampleCount = 16;
constexpr int kAdcFullScaleMv = 2500;

adc_oneshot_unit_handle_t adc_unit_handle = nullptr;
adc_cali_handle_t calibration_handle = nullptr;
adc_channel_t battery_channel;
bool monitor_ready = false;
std::atomic<uint16_t> battery_voltage_mv{0};
std::atomic<bool> battery_low{false};
int filtered_voltage_mv = 0;
bool filter_initialized = false;
uint8_t low_reading_count = 0;
uint8_t normal_reading_count = 0;

}  // namespace

bool tally_battery_monitor_init() {
    if (!tally_settings.battery_monitor_enabled) {
        ESP_LOGI(kTag, "Battery monitoring disabled");
        return false;
    }

    adc_unit_t unit_id;
#if CONFIG_IDF_TARGET_ESP32C3
    if (tally_settings.battery_adc_gpio > 4) {
        ESP_LOGE(kTag, "ESP32-C3 battery ADC must use ADC1 GPIO0-GPIO4");
        return false;
    }
#endif
    if (adc_oneshot_io_to_channel(tally_settings.battery_adc_gpio, &unit_id, &battery_channel) != ESP_OK ||
        unit_id != ADC_UNIT_1) {
        ESP_LOGE(kTag, "GPIO %u is not an ADC input; battery monitoring disabled",
                 tally_settings.battery_adc_gpio);
        return false;
    }

    adc_oneshot_unit_init_cfg_t unit_config{};
    unit_config.unit_id = unit_id;
    unit_config.ulp_mode = ADC_ULP_MODE_DISABLE;
    if (adc_oneshot_new_unit(&unit_config, &adc_unit_handle) != ESP_OK) {
        ESP_LOGE(kTag, "Could not initialize ADC unit");
        return false;
    }

    adc_oneshot_chan_cfg_t channel_config{};
    channel_config.atten = ADC_ATTEN_DB_11;
    channel_config.bitwidth = ADC_BITWIDTH_DEFAULT;
    if (adc_oneshot_config_channel(adc_unit_handle, battery_channel, &channel_config) != ESP_OK) {
        ESP_LOGE(kTag, "Could not configure ADC for GPIO %u", tally_settings.battery_adc_gpio);
        return false;
    }

    adc_cali_scheme_ver_t schemes{};
    if (adc_cali_check_scheme(&schemes) == ESP_OK) {
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
        if (schemes & ADC_CALI_SCHEME_VER_CURVE_FITTING) {
            adc_cali_curve_fitting_config_t config{};
            config.unit_id = unit_id;
            config.atten = ADC_ATTEN_DB_11;
            config.bitwidth = ADC_BITWIDTH_DEFAULT;
            adc_cali_create_scheme_curve_fitting(&config, &calibration_handle);
        }
#endif
#if ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
        if (!calibration_handle && (schemes & ADC_CALI_SCHEME_VER_LINE_FITTING)) {
            adc_cali_line_fitting_config_t config{};
            config.unit_id = unit_id;
            config.atten = ADC_ATTEN_DB_11;
            config.bitwidth = ADC_BITWIDTH_DEFAULT;
            adc_cali_create_scheme_line_fitting(&config, &calibration_handle);
        }
#endif
    }

    monitor_ready = true;
    low_reading_count = 0;
    normal_reading_count = 0;
    filter_initialized = false;
    ESP_LOGI(kTag, "LiPo monitoring on GPIO %u; low threshold %u mV, divider scale %.3f",
             tally_settings.battery_adc_gpio, tally_settings.battery_low_mv,
             tally_settings.battery_scale_milli / 1000.0f);
    return true;
}

uint16_t tally_battery_update() {
    if (!monitor_ready) return battery_voltage_mv;

    int raw_sum = 0;
    for (int sample = 0; sample < kSampleCount; ++sample) {
        int raw = 0;
        if (adc_oneshot_read(adc_unit_handle, battery_channel, &raw) != ESP_OK) return battery_voltage_mv;
        raw_sum += raw;
    }

    const int raw_average = raw_sum / kSampleCount;
    int pin_voltage_mv = 0;
    if (!calibration_handle || adc_cali_raw_to_voltage(calibration_handle, raw_average, &pin_voltage_mv) != ESP_OK) {
        pin_voltage_mv = raw_average * kAdcFullScaleMv / 4095;
    }
    const uint32_t scaled_mv =
        static_cast<uint32_t>(pin_voltage_mv) * tally_settings.battery_scale_milli / 1000;
    const int measured_mv = static_cast<int>(std::min<uint32_t>(scaled_mv, UINT16_MAX));
    if (!filter_initialized) {
        filtered_voltage_mv = measured_mv;
        filter_initialized = true;
    } else {
        filtered_voltage_mv = (filtered_voltage_mv * 3 + measured_mv + 2) / 4;
    }
    battery_voltage_mv = static_cast<uint16_t>(filtered_voltage_mv);

    if (!battery_low) {
        normal_reading_count = 0;
        if (battery_voltage_mv <= tally_settings.battery_low_mv) {
            if (low_reading_count < 3) ++low_reading_count;
            if (low_reading_count >= 3) {
                battery_low = true;
                low_reading_count = 0;
            }
        } else {
            low_reading_count = 0;
        }
    } else {
        low_reading_count = 0;
        if (battery_voltage_mv > tally_settings.battery_low_mv + 100) {
            if (normal_reading_count < 3) ++normal_reading_count;
            if (normal_reading_count >= 3) {
                battery_low = false;
                normal_reading_count = 0;
            }
        } else {
            normal_reading_count = 0;
        }
    }
    ESP_LOGI(kTag, "LiPo: %u.%03u V%s", battery_voltage_mv / 1000,
             battery_voltage_mv % 1000, battery_low ? " (low)" : "");
    return battery_voltage_mv;
}

uint16_t tally_battery_voltage_mv() {
    return battery_voltage_mv;
}

bool tally_battery_is_low() {
    return battery_low;
}