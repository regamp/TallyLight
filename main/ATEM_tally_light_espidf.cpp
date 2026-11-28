#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <atomic>
#include <cstdio>
#include <cstdarg>
#include <new>
#include <unistd.h>

#include "apps/ping/ping_sock.h"
#include "driver/rmt.h"
#include "esp_attr.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/etharp.h"
#include "lwip/netif.h"
#include "lwip/sockets.h"
#include "lwip/tcpip.h"
#include "esp_netif_net_stack.h"
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
constexpr bool kAckDuringInit = true;
constexpr bool kReuseLocalPort = true;
constexpr int kWifiConnected = BIT0;
constexpr uint32_t kInitRequestRetryMs = 1000;
constexpr uint32_t kInitRequestCheckMs = 200;
constexpr uint32_t kOutOfOrderGapTimeoutMs = 500;
constexpr uint32_t kHealthReportIntervalMs = 30000;
constexpr uint32_t kAtemSilentRestartMs = 10 * 60 * 1000;
constexpr uint32_t kRoamScanIntervalMs = 60000;
constexpr int kRoamRssiThreshold = -72;
constexpr int kRoamImprovementDb = 8;
constexpr uint32_t kDebugAutoDisableMs = 10 * 60 * 1000;
constexpr size_t kMaxNeopixelCount = 64;
constexpr size_t kDiagnosticEventCount = 32;
constexpr uint32_t kSoftProbeMs = 1500;
constexpr uint32_t kSilentTimeoutEstablishedMs = 5000;
constexpr uint32_t kSilentTimeoutHandshakeMs = 5000;
constexpr uint32_t kFreshPortWindowMs = 10 * 60 * 1000;
constexpr uint32_t kFreshPortMinIntervalMs = 15000;
constexpr size_t kFreshPortLimit = 20;
constexpr uint32_t kWifiBounceMinIntervalMs = 2 * 60 * 1000;
constexpr uint32_t kHoldTallyOnReconnectMs = 15000;
constexpr size_t kPacketHeaderRingSize = 128;
constexpr size_t kAckDeduplicationRingSize = 32;

struct RestartRecord {
    uint32_t magic;
    char reason[32];
};

struct DiagnosticEvent {
    uint32_t time_ms;
    char type[12];
    char details[96];
};

struct PacketHeaderRecord {
    uint32_t time_ms;
    char direction;
    uint8_t flags;
    uint16_t packet_id;
    uint16_t ack_id;
    uint16_t session_id;
    uint16_t length;
    char command[5];
};

struct SessionRecord {
    uint32_t start_ms;
    uint32_t lifetime_s;
    uint32_t reconnect_delay_ms;
    uint16_t last_rx_id;
    bool probe_revived;
    char cause[32];
};

struct PingContext {
    uint8_t replies = 0;
    uint8_t lost = 0;
    uint32_t total_rtt_ms = 0;
    char cause[32]{};
};

struct AckRecord {
    uint16_t packet_id = 0;
    uint32_t time_ms = 0;
};

RTC_NOINIT_ATTR RestartRecord restart_record;

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
uint32_t last_rx_ms = 0;
uint32_t last_valid_atem_packet_ms = 0;
uint32_t atem_silent_wifi_ms = 0;
uint32_t last_silence_check_ms = 0;
uint32_t last_hello_ms = 0;
uint32_t handshake_started_ms = 0;
uint32_t handshake_backoff_until_ms = 0;
uint32_t last_rx_packet_id = 0;
uint32_t last_ack_duplicate_window_ms = 0;
uint32_t last_roam_scan_ms = 0;
uint32_t hello_tx_count = 0;
uint32_t hello_rx_count = 0;
uint32_t hello_close_rx_count = 0;
uint32_t hello_tx_since_good = 0;
uint32_t hello_rx_since_good = 0;
uint32_t ack_tx_count = 0;
uint32_t ack_tx_recent = 0;
uint32_t duplicate_recent = 0;
uint32_t consecutive_handshake_failures = 0;
uint16_t last_rx_flags = 0;
uint16_t current_local_port = 0;
uint16_t last_hello_payload_length = 0;
std::array<uint8_t, 8> last_hello_payload{};
std::array<DiagnosticEvent, kDiagnosticEventCount> diagnostic_events{};
size_t next_diagnostic_event = 0;
size_t diagnostic_event_count = 0;
portMUX_TYPE diagnostic_events_mux = portMUX_INITIALIZER_UNLOCKED;
std::array<PacketHeaderRecord, kPacketHeaderRingSize> packet_header_ring{};
size_t next_packet_header = 0;
size_t packet_header_count = 0;
portMUX_TYPE packet_header_mux = portMUX_INITIALIZER_UNLOCKED;
std::array<SessionRecord, 16> session_history{};
size_t next_session_record = 0;
size_t session_record_count = 0;
portMUX_TYPE session_history_mux = portMUX_INITIALIZER_UNLOCKED;
std::array<uint32_t, kFreshPortLimit> fresh_port_timestamps{};
size_t fresh_port_timestamp_count = 0;
bool fresh_port_pending = false;
bool reused_port_after_drop = false;
std::atomic<uint32_t> fresh_port_last_granted_ms{0};
std::atomic<uint32_t> fresh_port_denied_count{0};
bool has_fresh_port_grant = false;
uint32_t last_wifi_bounce_ms = 0;
std::atomic<uint32_t> wifi_bounce_count{0};
std::atomic<uint32_t> soft_probe_count{0};
std::atomic<uint32_t> soft_probe_revived_count{0};
bool soft_probe_sent_for_silence = false;
bool soft_probe_awaiting_reply = false;
bool session_probe_revived = false;
uint32_t session_started_ms = 0;
uint32_t session_ended_ms = 0;
size_t pending_session_record = 0;
bool pending_session_delay = false;
uint32_t last_session_lifetime_s = 0;
uint32_t hold_tally_until_ms = 0;
bool hold_tally_active = false;
std::array<AckRecord, kAckDeduplicationRingSize> recent_ack_ids{};
size_t next_recent_ack_id = 0;
uint8_t consecutive_init_acks = 0;
std::array<uint32_t, 64> duplicate_timestamps{};
size_t next_duplicate_timestamp = 0;
size_t duplicate_timestamp_count = 0;
std::atomic<bool> duplicate_storm_pending{false};
std::atomic<uint32_t> ack_retry_ok{0};
std::atomic<uint32_t> ack_dropped{0};
portMUX_TYPE ping_result_mux = portMUX_INITIALIZER_UNLOCKED;
uint32_t last_ping_ok = 0;
uint32_t last_ping_lost = 0;
uint32_t last_ping_avg_rtt_ms = 0;
uint32_t last_ping_time_ms = 0;
bool last_arp_entry_found = false;
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
std::atomic<bool> wifi_session_reset{false};
std::atomic<bool> wifi_had_ip{false};
std::atomic<bool> wifi_bounce_pending{false};
std::atomic<bool> gratuitous_arp_pending{false};
std::atomic<bool> restart_pending{false};
std::atomic<bool> roam_scan_pending{false};
std::atomic<bool> roam_reconnect_pending{false};
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
uint32_t wifi_disconnect_reason = 0;
int roam_current_rssi = -127;
std::array<uint8_t, 6> roam_current_bssid{};

uint16_t read_be16(const uint8_t* data) {
    return static_cast<uint16_t>((static_cast<uint16_t>(data[0]) << 8) | data[1]);
}

uint32_t millis_now();
void reset_atem_session(bool close_socket, bool hold_tally = false);
void handle_handshake_failure(const char* cause, bool close_socket_immediately = false);

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

void record_diagnostic_event(const char* type, const char* details) {
    const uint32_t now = millis_now();
    portENTER_CRITICAL(&diagnostic_events_mux);
    DiagnosticEvent& event = diagnostic_events[next_diagnostic_event];
    event.time_ms = now;
    strlcpy(event.type, type, sizeof(event.type));
    strlcpy(event.details, details, sizeof(event.details));
    next_diagnostic_event = (next_diagnostic_event + 1) % diagnostic_events.size();
    diagnostic_event_count =
        std::min(diagnostic_event_count + 1, diagnostic_events.size());
    portEXIT_CRITICAL(&diagnostic_events_mux);
}

void record_packet_header(char direction, const uint8_t* packet, size_t packet_size) {
    PacketHeaderRecord record{};
    record.time_ms = millis_now();
    record.direction = direction;
    record.length = static_cast<uint16_t>(std::min<size_t>(packet_size, UINT16_MAX));
    if (packet && packet_size >= 12) {
        record.flags = packet[0] >> 3;
        record.session_id = read_be16(packet + 2);
        record.ack_id = read_be16(packet + 4);
        record.packet_id = read_be16(packet + 10);
        if (packet_size >= 20) {
            memcpy(record.command, packet + 16, 4);
            record.command[4] = '\0';
        }
    }

    portENTER_CRITICAL(&packet_header_mux);
    packet_header_ring[next_packet_header] = record;
    next_packet_header = (next_packet_header + 1) % packet_header_ring.size();
    packet_header_count = std::min(packet_header_count + 1, packet_header_ring.size());
    portEXIT_CRITICAL(&packet_header_mux);
}

void record_session_end(const char* cause) {
    if (!session_started_ms) return;
    const uint32_t now = millis_now();
    SessionRecord record{};
    record.start_ms = session_started_ms;
    record.lifetime_s = (now - session_started_ms) / 1000;
    record.reconnect_delay_ms = UINT32_MAX;
    record.last_rx_id = static_cast<uint16_t>(last_rx_packet_id);
    record.probe_revived = session_probe_revived;
    strlcpy(record.cause, cause, sizeof(record.cause));

    portENTER_CRITICAL(&session_history_mux);
    const size_t slot = next_session_record;
    session_history[slot] = record;
    next_session_record = (next_session_record + 1) % session_history.size();
    session_record_count = std::min(session_record_count + 1, session_history.size());
    portEXIT_CRITICAL(&session_history_mux);

    pending_session_record = slot;
    pending_session_delay = true;
    session_ended_ms = now;
    last_session_lifetime_s = record.lifetime_s;
    session_started_ms = 0;
}

void record_session_start(uint32_t now, uint16_t packet_id) {
    if (pending_session_delay) {
        const uint32_t delay = now - session_ended_ms;
        portENTER_CRITICAL(&session_history_mux);
        session_history[pending_session_record].reconnect_delay_ms = delay;
        portEXIT_CRITICAL(&session_history_mux);
        pending_session_delay = false;
    }
    session_started_ms = now;
    last_rx_packet_id = packet_id;
    session_probe_revived = false;
    soft_probe_sent_for_silence = false;
    soft_probe_awaiting_reply = false;
}

void dump_packet_header_ring(size_t count) {
    std::array<PacketHeaderRecord, 24> snapshot{};
    size_t copied = 0;
    portENTER_CRITICAL(&packet_header_mux);
    copied = std::min({count, packet_header_count, snapshot.size()});
    const size_t first = (next_packet_header + packet_header_ring.size() -
                          copied) % packet_header_ring.size();
    for (size_t index = 0; index < copied; ++index)
        snapshot[index] = packet_header_ring[(first + index) % packet_header_ring.size()];
    portEXIT_CRITICAL(&packet_header_mux);

    for (size_t index = 0; index < copied; ++index) {
        const PacketHeaderRecord& packet = snapshot[index];
        ESP_LOGW(kTag, "Packet %c t=%u flags=0x%02x id=%u ack=%u session=0x%04x "
                      "len=%u cmd=%.4s",
                 packet.direction, static_cast<unsigned>(packet.time_ms),
                 packet.flags, static_cast<unsigned>(packet.packet_id),
                 static_cast<unsigned>(packet.ack_id),
                 static_cast<unsigned>(packet.session_id),
                 static_cast<unsigned>(packet.length), packet.command);
    }
}

struct ArpLookup {
    struct netif* netif;
    ip4_addr_t target;
    SemaphoreHandle_t completion;
    bool found;
};

void arp_lookup_callback(void* argument) {
    auto* lookup = static_cast<ArpLookup*>(argument);
    struct eth_addr* hardware_address = nullptr;
    const ip4_addr_t* resolved_address = nullptr;
    lookup->found = etharp_find_addr(lookup->netif, &lookup->target,
                                     &hardware_address, &resolved_address) >= 0;
    xSemaphoreGive(lookup->completion);
}

void log_arp_entry() {
    static SemaphoreHandle_t completion = nullptr;
    if (!completion) completion = xSemaphoreCreateBinary();
    bool found = false;
    bool lookup_completed = false;
    auto* netif = station_netif
                      ? static_cast<struct netif*>(esp_netif_get_netif_impl(station_netif))
                      : nullptr;
    if (!completion) {
        ESP_LOGW(kTag, "Could not create ARP lookup semaphore");
    } else if (!netif) {
        ESP_LOGW(kTag, "Could not access station netif for ARP lookup");
    } else {
        xSemaphoreTake(completion, 0);
        ArpLookup lookup{};
        lookup.netif = netif;
        lookup.target.addr = switcher_address.sin_addr.s_addr;
        lookup.completion = completion;
        const err_t result = tcpip_callback(arp_lookup_callback, &lookup);
        if (result == ERR_OK) {
            if (xSemaphoreTake(completion, portMAX_DELAY) == pdTRUE) {
                found = lookup.found;
                lookup_completed = true;
            }
        } else {
            ESP_LOGW(kTag, "Could not schedule ARP lookup: %d", result);
        }
    }
    portENTER_CRITICAL(&ping_result_mux);
    last_arp_entry_found = found;
    portEXIT_CRITICAL(&ping_result_mux);
    ESP_LOGI(kTag, "ATEM ARP entry: %s",
             lookup_completed ? (found ? "present" : "missing") : "unavailable");
    record_diagnostic_event("arp", lookup_completed
                                       ? (found ? "entry present" : "entry missing")
                                       : "lookup unavailable");
}

void ping_success(esp_ping_handle_t handle, void* argument) {
    auto* context = static_cast<PingContext*>(argument);
    uint32_t elapsed_ms = 0;
    if (esp_ping_get_profile(handle, ESP_PING_PROF_TIMEGAP, &elapsed_ms,
                             sizeof(elapsed_ms)) == ESP_OK) {
        ++context->replies;
        context->total_rtt_ms += elapsed_ms;
    }
}

void ping_timeout(esp_ping_handle_t, void* argument) {
    auto* context = static_cast<PingContext*>(argument);
    ++context->lost;
}

void ping_end(esp_ping_handle_t handle, void* argument) {
    auto* context = static_cast<PingContext*>(argument);
    const uint32_t average = context->replies
                                 ? context->total_rtt_ms / context->replies
                                 : 0;
    portENTER_CRITICAL(&ping_result_mux);
    last_ping_ok = context->replies;
    last_ping_lost = context->lost;
    last_ping_avg_rtt_ms = average;
    last_ping_time_ms = millis_now();
    portEXIT_CRITICAL(&ping_result_mux);
    ESP_LOGI(kTag, "ATEM ping complete (%s): ok=%u lost=%u avg_rtt_ms=%u",
             context->cause,
             static_cast<unsigned>(context->replies),
             static_cast<unsigned>(context->lost), static_cast<unsigned>(average));
    char details[48]{};
    snprintf(details, sizeof(details), "%.12s ok=%u lost=%u rtt=%u ms",
             context->cause,
             static_cast<unsigned>(context->replies),
             static_cast<unsigned>(context->lost), static_cast<unsigned>(average));
    record_diagnostic_event("ping", details);
    esp_ping_delete_session(handle);
    delete context;
}

void start_connectivity_diagnostic_ping(const char* cause) {
    log_arp_entry();
    portENTER_CRITICAL(&ping_result_mux);
    last_ping_ok = 0;
    last_ping_lost = 0;
    last_ping_avg_rtt_ms = 0;
    last_ping_time_ms = 0;
    portEXIT_CRITICAL(&ping_result_mux);
    record_diagnostic_event("ping_start", cause);
    auto* context = new (std::nothrow) PingContext{};
    if (!context) {
        ESP_LOGW(kTag, "Could not allocate ATEM ping context");
        record_diagnostic_event("ping", "allocation failed");
        return;
    }
    strlcpy(context->cause, cause, sizeof(context->cause));
    esp_ping_config_t config = ESP_PING_DEFAULT_CONFIG();
    config.count = 3;
    config.interval_ms = 300;
    config.timeout_ms = 300;
    config.target_addr.type = IPADDR_TYPE_V4;
    ip_addr_set_ip4_u32(&config.target_addr, switcher_address.sin_addr.s_addr);

    esp_ping_callbacks_t callbacks{};
    callbacks.cb_args = context;
    callbacks.on_ping_success = ping_success;
    callbacks.on_ping_timeout = ping_timeout;
    callbacks.on_ping_end = ping_end;
    esp_ping_handle_t handle = nullptr;
    esp_err_t result = esp_ping_new_session(&config, &callbacks, &handle);
    if (result == ESP_OK) result = esp_ping_start(handle);
    if (result != ESP_OK) {
        ESP_LOGW(kTag, "Could not start ATEM ping (%s): %s", cause,
                 esp_err_to_name(result));
        record_diagnostic_event("ping", "start failed");
        if (handle) esp_ping_delete_session(handle);
        delete context;
    }
}

void refresh_recent_counters(uint32_t now) {
    if (!last_ack_duplicate_window_ms ||
        now - last_ack_duplicate_window_ms >= 10000) {
        last_ack_duplicate_window_ms = now;
        ack_tx_recent = 0;
    }
    duplicate_recent = 0;
    for (size_t index = 0; index < duplicate_timestamp_count; ++index) {
        const uint32_t timestamp =
            duplicate_timestamps[(next_duplicate_timestamp +
                                  duplicate_timestamps.size() - 1 - index) %
                                 duplicate_timestamps.size()];
        if (now - timestamp <= 10000) ++duplicate_recent;
    }
}

void record_ack_sent() {
    ++ack_tx_count;
    refresh_recent_counters(millis_now());
    ++ack_tx_recent;
}

void record_duplicate_received() {
    const uint32_t now = millis_now();
    duplicate_timestamps[next_duplicate_timestamp] = now;
    next_duplicate_timestamp =
        (next_duplicate_timestamp + 1) % duplicate_timestamps.size();
    duplicate_timestamp_count =
        std::min(duplicate_timestamp_count + 1, duplicate_timestamps.size());
    refresh_recent_counters(now);
    if (duplicate_recent > 50) duplicate_storm_pending.store(true);
}

void log_reconnect_diagnostic(const char* cause) {
    wifi_ap_record_t access_point{};
    char bssid[18] = "none";
    int channel = 0;
    int rssi = -127;
    if ((xEventGroupGetBits(wifi_events) & kWifiConnected) &&
        esp_wifi_sta_get_ap_info(&access_point) == ESP_OK) {
        snprintf(bssid, sizeof(bssid), "%02x:%02x:%02x:%02x:%02x:%02x",
                 access_point.bssid[0], access_point.bssid[1], access_point.bssid[2],
                 access_point.bssid[3], access_point.bssid[4], access_point.bssid[5]);
        channel = access_point.primary;
        rssi = access_point.rssi;
    }
    const uint32_t now = millis_now();
    refresh_recent_counters(now);
    const uint32_t session_lifetime_s =
        session_started_ms ? (now - session_started_ms) / 1000 : last_session_lifetime_s;
    ESP_LOGW(kTag,
             "Reconnect: cause=%s session_lifetime_s=%u ms_since_last_rx=%u "
             "last_rx_id=%u last_rx_flags=0x%02x "
             "ack_10s=%u dup_10s=%u hello_tx_since_good=%u hello_rx_since_good=%u "
             "hello_payload=%02x%02x%02x%02x%02x%02x%02x%02x "
             "BSSID=%s channel=%d RSSI=%d",
             cause, static_cast<unsigned>(session_lifetime_s),
             static_cast<unsigned>(last_rx_ms ? now - last_rx_ms : now),
             static_cast<unsigned>(last_rx_packet_id), static_cast<unsigned>(last_rx_flags),
             static_cast<unsigned>(ack_tx_recent), static_cast<unsigned>(duplicate_recent),
             static_cast<unsigned>(hello_tx_since_good),
             static_cast<unsigned>(hello_rx_since_good),
             last_hello_payload[0], last_hello_payload[1], last_hello_payload[2],
             last_hello_payload[3], last_hello_payload[4], last_hello_payload[5],
             last_hello_payload[6], last_hello_payload[7], bssid, channel, rssi);
    record_diagnostic_event("reconnect", cause);
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

void handle_roam_scan_complete() {
    roam_scan_pending.store(false);
    uint16_t access_point_count = 0;
    esp_err_t result = esp_wifi_scan_get_ap_num(&access_point_count);
    if (result != ESP_OK) {
        ESP_LOGW(kTag, "Could not read roaming scan count: %s", esp_err_to_name(result));
        return;
    }

    std::array<wifi_ap_record_t, 32> access_points{};
    uint16_t records_to_read =
        std::min<uint16_t>(access_point_count, access_points.size());
    if (records_to_read) {
        result = esp_wifi_scan_get_ap_records(&records_to_read, access_points.data());
        if (result != ESP_OK) {
            ESP_LOGW(kTag, "Could not read roaming scan results: %s", esp_err_to_name(result));
            return;
        }
    }

    for (uint16_t index = 0; index < records_to_read; ++index) {
        const wifi_ap_record_t& candidate = access_points[index];
        if (strncmp(reinterpret_cast<const char*>(candidate.ssid),
                    tally_settings.wifi_ssid, sizeof(candidate.ssid)) == 0 &&
            memcmp(candidate.bssid, roam_current_bssid.data(),
                   roam_current_bssid.size()) != 0 &&
            candidate.rssi >= roam_current_rssi + kRoamImprovementDb) {
            log_reconnect_diagnostic("Wi-Fi roam to better same-SSID BSSID");
            roam_reconnect_pending.store(true);
            const esp_err_t disconnect_result = esp_wifi_disconnect();
            if (disconnect_result != ESP_OK) {
                roam_reconnect_pending.store(false);
                ESP_LOGW(kTag, "Could not disconnect for Wi-Fi roaming: %s",
                         esp_err_to_name(disconnect_result));
            }
            return;
        }
    }
}

void wifi_event_handler(void*, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        if (tally_settings.wifi_ssid[0] != '\0' && !restart_pending.load()) {
            const esp_err_t result = esp_wifi_connect();
            if (result != ESP_OK) {
                ESP_LOGE(kTag, "Could not connect to Wi-Fi: %s", esp_err_to_name(result));
            }
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_START) {
        debug_tally_pending.store(true);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_BSS_RSSI_LOW) {
        const esp_err_t threshold_result = esp_wifi_set_rssi_threshold(kRoamRssiThreshold);
        if (threshold_result != ESP_OK) {
            ESP_LOGW(kTag, "Could not re-arm Wi-Fi roaming RSSI threshold: %s",
                     esp_err_to_name(threshold_result));
        }
        const uint32_t now = millis_now();
        if (!roam_scan_pending.load() &&
            (!last_roam_scan_ms || now - last_roam_scan_ms >= kRoamScanIntervalMs)) {
            wifi_ap_record_t access_point{};
            if (esp_wifi_sta_get_ap_info(&access_point) == ESP_OK) {
                roam_current_rssi = access_point.rssi;
                memcpy(roam_current_bssid.data(), access_point.bssid,
                       roam_current_bssid.size());
                wifi_scan_config_t scan_config{};
                scan_config.scan_type = WIFI_SCAN_TYPE_ACTIVE;
                scan_config.scan_time.active.min = 100;
                scan_config.scan_time.active.max = 300;
                const esp_err_t result = esp_wifi_scan_start(&scan_config, false);
                if (result == ESP_OK) {
                    last_roam_scan_ms = now;
                    roam_scan_pending.store(true);
                    record_diagnostic_event("roam_scan", "RSSI threshold reached");
                } else {
                    ESP_LOGW(kTag, "Could not start Wi-Fi roaming scan: %s",
                             esp_err_to_name(result));
                }
            }
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_SCAN_DONE) {
        if (roam_scan_pending.load()) handle_roam_scan_complete();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(wifi_events, kWifiConnected);
        const bool intentional_bounce = wifi_bounce_pending.load();
        const bool intentional_roam = roam_reconnect_pending.exchange(false);
        const bool had_ip = wifi_had_ip.exchange(false);
        const auto* event = static_cast<wifi_event_sta_disconnected_t*>(event_data);
        wifi_disconnect_reason = event ? event->reason : 0;
        if (had_ip) {
            wifi_session_reset.store(true);
            if (!intentional_bounce && !intentional_roam)
                wifi_lost.store(true);
        }
        if (intentional_bounce) gratuitous_arp_pending.store(true);
        if (tally_settings.wifi_ssid[0] != '\0' && !restart_pending.load()) {
            const esp_err_t result = esp_wifi_connect();
            if (result != ESP_OK && !restart_pending.load()) {
                ESP_LOGE(kTag, "Could not reconnect to Wi-Fi: %s", esp_err_to_name(result));
            }
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(wifi_events, kWifiConnected);
        wifi_had_ip.store(true);
        if (wifi_bounce_pending.load()) gratuitous_arp_pending.store(true);
        debug_tally_pending.store(true);
        if (gratuitous_arp_pending.exchange(false) && station_netif) {
            struct netif* netif =
                static_cast<struct netif*>(esp_netif_get_netif_impl(station_netif));
            if (netif) etharp_gratuitous(netif);
            wifi_bounce_pending.store(false);
        }
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
    const esp_err_t rssi_result = esp_wifi_set_rssi_threshold(kRoamRssiThreshold);
    if (rssi_result != ESP_OK) {
        ESP_LOGW(kTag, "Could not set Wi-Fi roaming RSSI threshold: %s",
                 esp_err_to_name(rssi_result));
    }
    if (!tally_settings.wifi_ssid[0]) soft_ap_active = true;
}

void record_atem_send_failure(const char* operation, int error);

bool suppress_duplicate_ack(uint16_t packet_id) {
    const uint32_t now = millis_now();
    for (const AckRecord& record : recent_ack_ids) {
        if (record.time_ms && record.packet_id == packet_id &&
            now - record.time_ms < 20) {
            return true;
        }
    }
    recent_ack_ids[next_recent_ack_id] = {packet_id, now};
    next_recent_ack_id = (next_recent_ack_id + 1) % recent_ack_ids.size();
    return false;
}

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

    const bool is_ack = flags == 0x10;
    for (uint8_t attempt = 0; attempt < (is_ack ? 4 : 1); ++attempt) {
        const int sent = sendto(atem_socket, packet, packet_size, 0,
                                reinterpret_cast<const sockaddr*>(&switcher_address),
                                sizeof(switcher_address));
        if (sent == static_cast<int>(packet_size)) {
            record_atem_packet("TX");
            record_packet_header('T', packet, packet_size);
            if (is_ack) {
                record_ack_sent();
                if (attempt) ++ack_retry_ok;
                if (!atem_initialized && ++consecutive_init_acks >= 8) {
                    consecutive_init_acks = 0;
                    vTaskDelay(1);
                }
            } else {
                consecutive_init_acks = 0;
            }
            return true;
        }

        const int send_error = sent < 0 ? errno : 0;
        if (is_ack && sent < 0 && send_error == ENOMEM && attempt < 3) {
            vTaskDelay(1);
            continue;
        }
        if (is_ack) ++ack_dropped;
        record_atem_send_failure(sent < 0 ? "ATEM UDP send" : "Short ATEM UDP send",
                                 send_error);
        return false;
    }
    return false;
}

void send_hello() {
    uint8_t payload[8]{};
    payload[0] = 0x01;
    const bool sent = send_atem_packet(0x02, 0, payload, sizeof(payload), 0x3A);
    last_hello_ms = millis_now();
    if (sent) {
        ++hello_tx_count;
        ++hello_tx_since_good;
        if (!handshake_started_ms) {
            handshake_started_ms = last_hello_ms;
            record_diagnostic_event("hello_tx", "new handshake round");
        }
    }
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
    if (kAckDuringInit || init_payload_seen) {
        if (!suppress_duplicate_ack(remote_packet_id))
            send_atem_packet(0x10, remote_packet_id, nullptr, 0);
    }

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
        record_duplicate_received();
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
        record_duplicate_received();
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
    (void)source;
    last_rx_ms = millis_now();
    record_packet_header('R', packet, packet_size);
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

    const uint8_t flags = packet[0] >> 3;
    const uint16_t remote_packet_id = read_be16(packet + 10);
    last_rx_packet_id = remote_packet_id;
    last_rx_flags = flags;

    if (flags & 0x02) {
        const bool was_connected = atem_connected.load();
        const size_t payload_size = packet_size - 12;
        const uint8_t payload_status = payload_size ? packet[12] : 0xFF;
        last_hello_payload_length =
            static_cast<uint16_t>(std::min(payload_size, last_hello_payload.size()));
        last_hello_payload.fill(0);
        if (last_hello_payload_length)
            memcpy(last_hello_payload.data(), packet + 12, last_hello_payload_length);
        ++hello_rx_count;
        ++hello_rx_since_good;
        if (was_connected) ++hello_close_rx_count;
        char hello_event[64]{};
        snprintf(hello_event, sizeof(hello_event), "payload=%02x%02x%02x%02x",
                 last_hello_payload[0], last_hello_payload[1],
                 last_hello_payload[2], last_hello_payload[3]);
        record_diagnostic_event("hello_rx", hello_event);

        if (!was_connected) {
            ESP_LOGI(kTag,
                     "ATEM hello RX payload=%02x %02x %02x %02x %02x %02x %02x %02x "
                     "length=%u connected=0 initialized=%u",
                     last_hello_payload[0], last_hello_payload[1],
                     last_hello_payload[2], last_hello_payload[3],
                     last_hello_payload[4], last_hello_payload[5],
                     last_hello_payload[6], last_hello_payload[7],
                     static_cast<unsigned>(payload_size),
                     static_cast<unsigned>(atem_initialized));
        }
        if (was_connected && !atem_initialized && payload_status == 2) {
            ESP_LOGW(kTag,
                     "ATEM hello RX payload=%02x %02x %02x %02x %02x %02x %02x %02x "
                     "length=%u connected=1 initialized=0; repeating hello ACK",
                     last_hello_payload[0], last_hello_payload[1],
                     last_hello_payload[2], last_hello_payload[3],
                     last_hello_payload[4], last_hello_payload[5],
                     last_hello_payload[6], last_hello_payload[7],
                     static_cast<unsigned>(payload_size));
            send_atem_packet(0x10, 0, nullptr, 0, 0x03);
            record_atem_packet("RX");
            return;
        }
        if (was_connected) {
            log_reconnect_diagnostic("hello flag received while connected");
            record_session_end("hello while connected");
            record_atem_packet("RX");
            reset_atem_session(false, true);
            set_state(2);
            return;
        }
        if (payload_status != 2) {
            if (payload_status == 3)
                ESP_LOGE(kTag, "ATEM rejected the connection (status=3; no free client slot)");
            handle_handshake_failure(payload_status == 3
                                         ? "ATEM reports no free client slot"
                                         : "invalid hello response");
            record_atem_packet("RX");
            return;
        }

        session_id = read_be16(packet + 2);
        atem_connected = true;
        clear_out_of_order_packets();
        last_remote_packet_id = remote_packet_id;
        remote_sequence_initialized = true;
        last_valid_atem_packet_ms = millis_now();
        last_atem_packet_ms = last_valid_atem_packet_ms;
        record_session_start(last_valid_atem_packet_ms, remote_packet_id);
        atem_silent_wifi_ms = 0;
        last_silence_check_ms = last_valid_atem_packet_ms;
        handshake_started_ms = 0;
        hold_tally_active = false;
        hold_tally_until_ms = 0;
        send_atem_packet(0x10, 0, nullptr, 0, 0x03);
        set_state(3);
        ESP_LOGI(kTag, "ATEM switcher connected");
        record_atem_packet("RX");
        return;
    }

    if (!atem_connected) {
        record_atem_packet("RX");
        return;
    }

    session_id = read_be16(packet + 2);
    if (soft_probe_awaiting_reply) {
        soft_probe_awaiting_reply = false;
        session_probe_revived = true;
        ++soft_probe_revived_count;
        ESP_LOGI(kTag, "soft_probe revived ATEM session with packet %u",
                 static_cast<unsigned>(remote_packet_id));
        record_diagnostic_event("soft_probe", "probe_revived");
    }
    soft_probe_sent_for_silence = false;
    last_atem_packet_ms = millis_now();
    last_valid_atem_packet_ms = last_atem_packet_ms;
    atem_silent_wifi_ms = 0;
    last_silence_check_ms = last_valid_atem_packet_ms;
    if (consecutive_handshake_failures) {
        consecutive_handshake_failures = 0;
        record_diagnostic_event("session_good", "valid post-handshake packet");
    }
    hello_tx_since_good = 0;
    hello_rx_since_good = 0;

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
            record_packet_header('T', response, sizeof(response));
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
    current_local_port = ntohs(bound_address.sin_port);
    if (fresh_port_pending) {
        fresh_port_pending = false;
        reused_port_after_drop = false;
    }
    last_atem_packet_ms = millis_now();
    return true;
}

bool request_fresh_local_port(const char* cause) {
    const uint32_t now = millis_now();
    size_t kept = 0;
    for (size_t index = 0; index < fresh_port_timestamp_count; ++index) {
        if (now - fresh_port_timestamps[index] < kFreshPortWindowMs)
            fresh_port_timestamps[kept++] = fresh_port_timestamps[index];
    }
    fresh_port_timestamp_count = kept;

    uint32_t wait_ms = 0;
    if (has_fresh_port_grant) {
        const uint32_t elapsed = now - fresh_port_last_granted_ms.load();
        if (elapsed < kFreshPortMinIntervalMs)
            wait_ms = kFreshPortMinIntervalMs - elapsed;
    }
    if (fresh_port_timestamp_count >= kFreshPortLimit) {
        const uint32_t cap_wait_ms =
            fresh_port_timestamps[0] + kFreshPortWindowMs - now;
        wait_ms = std::max(wait_ms, cap_wait_ms);
    }
    if (wait_ms) {
        ++fresh_port_denied_count;
        handshake_backoff_until_ms = now + wait_ms;
        ESP_LOGW(kTag, "Keeping UDP port %u for %s; fresh port denied for %u ms",
                 static_cast<unsigned>(current_local_port), cause,
                 static_cast<unsigned>(wait_ms));
        record_diagnostic_event("port_choice", "reuse; fresh-port rate limit");
        reused_port_after_drop = true;
        return false;
    }

    ESP_LOGI(kTag, "Opening a fresh UDP port for %s (%u of %u in 10 minutes)",
             cause, static_cast<unsigned>(fresh_port_timestamp_count + 1),
             static_cast<unsigned>(kFreshPortLimit));
    record_diagnostic_event("port_choice", "new local UDP port");
    fresh_port_timestamps[fresh_port_timestamp_count++] = now;
    fresh_port_last_granted_ms.store(now);
    has_fresh_port_grant = true;
    fresh_port_pending = true;
    reused_port_after_drop = false;
    return true;
}

void reset_atem_session(bool close_socket, bool hold_tally) {
    const bool preserve_tally = hold_tally || hold_tally_active;
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
    soft_probe_sent_for_silence = false;
    soft_probe_awaiting_reply = false;
    session_probe_revived = false;
    recent_ack_ids.fill({});
    next_recent_ack_id = 0;
    consecutive_init_acks = 0;
    duplicate_timestamps.fill(0);
    next_duplicate_timestamp = 0;
    duplicate_timestamp_count = 0;
    duplicate_recent = 0;
    duplicate_storm_pending.store(false);
    missed_init_packets.fill(0xFF);
    clear_out_of_order_packets();
    if (preserve_tally) {
        if (!hold_tally_active)
            hold_tally_until_ms = millis_now() + kHoldTallyOnReconnectMs;
        hold_tally_active = true;
    } else {
        hold_tally_active = false;
        hold_tally_until_ms = 0;
        portENTER_CRITICAL(&tally_flags_mux);
        tally_flags.fill(0);
        portEXIT_CRITICAL(&tally_flags_mux);
        tally_server_reset_flags();
        stream_is_on_air = false;
    }
    last_hello_ms = 0;
    handshake_started_ms = 0;
    last_atem_packet_ms = millis_now();
    output_refresh_pending.store(true);
    if (test_server_mode.load()) debug_tally_pending.store(true);
    if (close_socket && atem_socket >= 0) {
        close(atem_socket);
        atem_socket = -1;
        current_local_port = 0;
    }
}

void handle_handshake_failure(const char* cause, bool close_socket_immediately) {
    start_connectivity_diagnostic_ping(cause);
    log_reconnect_diagnostic(cause);
    ++consecutive_handshake_failures;
    ++timeout_reconnect_count;
    char details[96]{};
    snprintf(details, sizeof(details), "%s; consecutive=%u", cause,
             static_cast<unsigned>(consecutive_handshake_failures));
    record_diagnostic_event("handshake_fail", details);

    const uint32_t failures = consecutive_handshake_failures;
    const uint32_t now = millis_now();
    const bool bounce_due = failures == 3 ||
                            (failures > 3 && failures % 6 == 0);
    const bool bounce_interval_elapsed =
        !last_wifi_bounce_ms ||
        now - last_wifi_bounce_ms >= kWifiBounceMinIntervalMs;
    const bool wifi_bounce =
        bounce_due && (failures == 3 || bounce_interval_elapsed);

    if (wifi_bounce) {
        fresh_port_pending = false;
        reused_port_after_drop = false;
        ESP_LOGI(kTag, "Reconnect ladder: failure=%u action=Wi-Fi bounce and recreate socket",
                 static_cast<unsigned>(failures));
        record_session_end(cause);
        reset_atem_session(true, hold_tally_active);
        last_wifi_bounce_ms = now;
        ++wifi_bounce_count;
        wifi_bounce_pending.store(true);
        gratuitous_arp_pending.store(true);
        char bounce_details[48]{};
        snprintf(bounce_details, sizeof(bounce_details), "failure=%u",
                 static_cast<unsigned>(failures));
        record_diagnostic_event("wifi_bounce", bounce_details);
        const esp_err_t result = esp_wifi_disconnect();
        if (result != ESP_OK) {
            wifi_bounce_pending.store(false);
            ESP_LOGE(kTag, "Could not bounce Wi-Fi after handshake failures: %s",
                     esp_err_to_name(result));
        }
        return;
    }

    const bool prefer_fresh = close_socket_immediately || !kReuseLocalPort ||
                              (reused_port_after_drop && strstr(cause, "silent"));
    const bool fresh_port = prefer_fresh && request_fresh_local_port(cause);
    const bool fresh_port_denied = prefer_fresh && !fresh_port;
    const char* action = fresh_port ? "open fresh UDP port" :
                         fresh_port_denied ? "reuse UDP port until rate limit expires" :
                         failures >= 4 ? "exponential hello backoff" :
                         kReuseLocalPort ? "reuse UDP socket for next session" :
                                           "recreate UDP socket for next session";
    ESP_LOGI(kTag, "Reconnect ladder: failure=%u action=%s",
             static_cast<unsigned>(failures), action);
    record_session_end(cause);
    reset_atem_session(fresh_port, hold_tally_active);
    if (fresh_port_denied) {
        ESP_LOGI(kTag, "Next hello round in %u ms",
                 static_cast<unsigned>(handshake_backoff_until_ms - millis_now()));
    } else if (failures >= 4) {
        const uint32_t exponent = std::min<uint32_t>(failures > 4 ? failures - 4 : 0, 3);
        const uint32_t delay_ms = std::min<uint32_t>(5000U << exponent, 30000U);
        const int32_t jitter_ms = static_cast<int32_t>(esp_random() % 501U) - 250;
        const uint32_t actual_delay_ms = static_cast<uint32_t>(
            std::clamp<int32_t>(static_cast<int32_t>(delay_ms) + jitter_ms, 1000, 30000));
        handshake_backoff_until_ms = millis_now() + actual_delay_ms;
        ESP_LOGI(kTag, "Next hello round in %u ms",
                 static_cast<unsigned>(actual_delay_ms));
    }
}

void handle_silent_session_timeout() {
    const uint32_t now = millis_now();
    ++timeout_reconnect_count;
    log_reconnect_diagnostic("ATEM session silent");
    dump_packet_header_ring(24);
    record_diagnostic_event("timeout", "ATEM session silent");
    start_connectivity_diagnostic_ping("silent session timeout");
    record_session_end("silent timeout");

    const bool fresh_port = request_fresh_local_port("silent session timeout");
    if (!fresh_port) {
        ESP_LOGW(kTag, "Reusing UDP port after fresh-port rate-limit wait of %u ms",
                 static_cast<unsigned>(handshake_backoff_until_ms - now));
    }

    reset_atem_session(fresh_port, true);
    set_state(2);
}

void clear_held_tally_if_expired(uint32_t now) {
    if (!hold_tally_active ||
        static_cast<int32_t>(now - hold_tally_until_ms) < 0) {
        return;
    }
    hold_tally_active = false;
    hold_tally_until_ms = 0;
    portENTER_CRITICAL(&tally_flags_mux);
    tally_flags.fill(0);
    portEXIT_CRITICAL(&tally_flags_mux);
    tally_server_reset_flags();
    stream_is_on_air = false;
    if (test_server_mode.load()) debug_tally_pending.store(true);
    const bool wifi_connected =
        wifi_events && (xEventGroupGetBits(wifi_events) & kWifiConnected);
    set_state(wifi_connected ? 2 : 1);
    output_refresh_pending.store(true);
    record_diagnostic_event("tally_hold", "expired; flags cleared");
}

void handle_duplicate_storm() {
    if (!duplicate_storm_pending.exchange(false)) return;
    const uint32_t now = millis_now();
    log_reconnect_diagnostic("dup_storm");
    record_diagnostic_event("dup_storm", "more than 50 duplicates in 10 seconds");
    start_connectivity_diagnostic_ping("dup_storm");
    record_session_end("dup_storm");
    const bool fresh_port = request_fresh_local_port("dup_storm");
    reset_atem_session(fresh_port, true);
    set_state(2);
    if (!fresh_port) {
        ESP_LOGW(kTag, "dup_storm reusing UDP port after rate-limit wait of %u ms",
                 static_cast<unsigned>(handshake_backoff_until_ms - now));
    }
    duplicate_timestamps.fill(0);
    duplicate_timestamp_count = 0;
    duplicate_recent = 0;
}

void report_atem_health() {
    wifi_ap_record_t access_point{};
    char bssid[18] = "none";
    int channel = 0;
    const int rssi = (xEventGroupGetBits(wifi_events) & kWifiConnected) &&
                     esp_wifi_sta_get_ap_info(&access_point) == ESP_OK
                         ? access_point.rssi
                         : -127;
    if (rssi != -127) {
        snprintf(bssid, sizeof(bssid), "%02x:%02x:%02x:%02x:%02x:%02x",
                 access_point.bssid[0], access_point.bssid[1], access_point.bssid[2],
                 access_point.bssid[3], access_point.bssid[4], access_point.bssid[5]);
        channel = access_point.primary;
    }
    refresh_recent_counters(millis_now());
    ESP_LOGI(kTag,
             "Health: heap=%u min_heap=%u largest_block=%u RSSI=%d dBm send_failures=%u ENOMEM=%u "
             "reconnects(timeout/wifi/manual)=%u/%u/%u ATEM_stack_highwater=%u "
             "ATEM_packets(rx/tx/duplicate/reordered)=%u/%u/%u/%u skipped_gaps=%u "
             "hello_tx=%u hello_rx=%u hello_close_rx=%u dup_last_10s=%u ack_tx=%u "
             "ack_retry_ok=%u ack_dropped=%u local_port=%u wifi_bssid=%s wifi_channel=%d "
             "consecutive_handshake_failures=%u soft_probes=%u soft_probe_revived=%u",
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
             static_cast<unsigned>(atem_skipped_gap_count),
             static_cast<unsigned>(hello_tx_count),
             static_cast<unsigned>(hello_rx_count),
             static_cast<unsigned>(hello_close_rx_count),
             static_cast<unsigned>(duplicate_recent),
             static_cast<unsigned>(ack_tx_count),
             static_cast<unsigned>(ack_retry_ok.load()),
             static_cast<unsigned>(ack_dropped.load()),
             static_cast<unsigned>(current_local_port), bssid, channel,
             static_cast<unsigned>(consecutive_handshake_failures),
             static_cast<unsigned>(soft_probe_count.load()),
             static_cast<unsigned>(soft_probe_revived_count.load()));
}

void run_atem_client() {
    static uint8_t packet[kAtemMaxPacketSize];
    if (!atem_connected &&
        static_cast<int32_t>(millis_now() - handshake_backoff_until_ms) >= 0 &&
        (last_hello_ms == 0 ||
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
    if (!last_valid_atem_packet_ms) last_valid_atem_packet_ms = millis_now();
    set_state(1);
    uint32_t last_health_report_ms = millis_now();
    uint32_t last_init_request_check_ms = millis_now();

    while (true) {
        const bool real_wifi_loss = wifi_lost.exchange(false);
        const bool wifi_session_needs_reset = wifi_session_reset.exchange(false);
        if (real_wifi_loss) {
            ++wifi_reconnect_count;
            char cause[64]{};
            snprintf(cause, sizeof(cause), "Wi-Fi lost after IP acquisition (reason %u)",
                     static_cast<unsigned>(wifi_disconnect_reason));
            log_reconnect_diagnostic(cause);
        }
        if (wifi_session_needs_reset) {
            record_session_end("Wi-Fi loss");
            reset_atem_session(false, true);
            set_state(2);
        }
        if (manual_reset_requested.exchange(false)) {
            ++manual_reconnect_count;
            log_reconnect_diagnostic("manual reset");
            record_session_end("manual reset");
            reset_atem_session(false, true);
            set_state(2);
        }

        const EventBits_t bits = xEventGroupGetBits(wifi_events);
        const uint32_t loop_now = millis_now();
        if (!last_silence_check_ms) last_silence_check_ms = loop_now;
        else if (bits & kWifiConnected)
            atem_silent_wifi_ms += loop_now - last_silence_check_ms;
        last_silence_check_ms = loop_now;
        if (bits & kWifiConnected) {
            if (atem_socket < 0 && !open_atem_socket()) {
                vTaskDelay(pdMS_TO_TICKS(1000));
            } else {
                if (!atem_connected) set_state(2);
                run_atem_client();
                handle_duplicate_storm();
                const uint32_t now = millis_now();
                if (init_payload_seen && !atem_initialized &&
                    now - last_init_request_check_ms >= kInitRequestCheckMs) {
                    request_missing_initialization_packet();
                    last_init_request_check_ms = now;
                }
                recover_timed_out_out_of_order_gap();
                if (atem_connected && !soft_probe_sent_for_silence &&
                    last_valid_atem_packet_ms &&
                    now - last_valid_atem_packet_ms >= kSoftProbeMs) {
                    soft_probe_sent_for_silence = true;
                    ++soft_probe_count;
                    soft_probe_awaiting_reply =
                        send_atem_packet(0x10, last_remote_packet_id, nullptr, 0);
                    ESP_LOGW(kTag, "soft_probe id=%u sent=%u",
                             static_cast<unsigned>(last_remote_packet_id),
                             static_cast<unsigned>(soft_probe_awaiting_reply));
                    record_diagnostic_event("soft_probe",
                                            soft_probe_awaiting_reply
                                                ? "sent"
                                                : "send failed");
                }
                if (atem_connected && last_valid_atem_packet_ms &&
                    now - last_valid_atem_packet_ms >= kSilentTimeoutEstablishedMs) {
                    handle_silent_session_timeout();
                } else if (!atem_connected && handshake_started_ms) {
                    const uint32_t handshake_timeout =
                        reused_port_after_drop ? 3000 : kSilentTimeoutHandshakeMs;
                    if (now - handshake_started_ms >= handshake_timeout) {
                        handle_handshake_failure(
                            reused_port_after_drop
                                ? "reused-port handshake silent for 3000 ms"
                                : "hello sent; no valid reply within 5000 ms",
                            reused_port_after_drop);
                    }
                }

            }
            if (atem_silent_wifi_ms >= kAtemSilentRestartMs &&
                !restart_pending.exchange(true)) {
                log_reconnect_diagnostic("ATEM silent for 10 minutes");
                record_diagnostic_event("restart", "atem_silent_10min");
                restart_record.magic = 0x4154454D;
                strlcpy(restart_record.reason, "atem_silent_10min",
                        sizeof(restart_record.reason));
                ESP_LOGE(kTag, "Restarting after 10 minutes without a valid ATEM packet");
                esp_restart();
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(50));
        }

        const uint32_t now = millis_now();
        clear_held_tally_if_expired(now);
        refresh_recent_counters(now);
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

size_t tally_atem_diag_snapshot(char* output, size_t capacity) {
    if (!output || capacity == 0) return 0;
    static std::array<DiagnosticEvent, kDiagnosticEventCount> events_copy{};
    static std::array<PacketHeaderRecord, kPacketHeaderRingSize> packets_copy{};
    static std::array<SessionRecord, 16> sessions_copy{};
    size_t copied_event_count = 0;
    size_t copied_packet_count = 0;
    size_t copied_session_count = 0;
    portENTER_CRITICAL(&diagnostic_events_mux);
    copied_event_count = diagnostic_event_count;
    const size_t first = (next_diagnostic_event + diagnostic_events.size() -
                          diagnostic_event_count) % diagnostic_events.size();
    for (size_t index = 0; index < copied_event_count; ++index)
        events_copy[index] = diagnostic_events[(first + index) % diagnostic_events.size()];
    portEXIT_CRITICAL(&diagnostic_events_mux);

    portENTER_CRITICAL(&packet_header_mux);
    copied_packet_count = packet_header_count;
    const size_t packet_first =
        (next_packet_header + packet_header_ring.size() - packet_header_count) %
        packet_header_ring.size();
    for (size_t index = 0; index < copied_packet_count; ++index)
        packets_copy[index] =
            packet_header_ring[(packet_first + index) % packet_header_ring.size()];
    portEXIT_CRITICAL(&packet_header_mux);

    portENTER_CRITICAL(&session_history_mux);
    copied_session_count = session_record_count;
    const size_t session_first =
        (next_session_record + session_history.size() - session_record_count) %
        session_history.size();
    for (size_t index = 0; index < copied_session_count; ++index)
        sessions_copy[index] =
            session_history[(session_first + index) % session_history.size()];
    portEXIT_CRITICAL(&session_history_mux);

    uint32_t ping_ok;
    uint32_t ping_lost;
    uint32_t ping_rtt;
    uint32_t ping_time;
    bool arp_found;
    portENTER_CRITICAL(&ping_result_mux);
    ping_ok = last_ping_ok;
    ping_lost = last_ping_lost;
    ping_rtt = last_ping_avg_rtt_ms;
    ping_time = last_ping_time_ms;
    arp_found = last_arp_entry_found;
    portEXIT_CRITICAL(&ping_result_mux);

    const char* restart_reason =
        restart_record.magic == 0x4154454D ? restart_record.reason : "none";
    size_t used = 0;
    auto append = [&](const char* format, ...) {
        if (used >= capacity - 1) return;
        va_list arguments;
        va_start(arguments, format);
        const int written = vsnprintf(output + used, capacity - used, format, arguments);
        va_end(arguments);
        if (written > 0) used = std::min(used + static_cast<size_t>(written), capacity - 1);
    };

    append("Last restart reason: %s\n"
           "ATEM: connected=%u initialized=%u local_port=%u handshake_failures=%u "
           "hello_tx=%u hello_rx=%u hello_close_rx=%u ack_tx=%u ack_retry_ok=%u "
           "ack_dropped=%u dup_last_10s=%u\n"
           "Reconnect controls: fresh_port_last_granted_ms=%u fresh_port_denied_count=%u "
           "soft_probe_count=%u soft_probe_revived_count=%u wifi_bounce_count=%u\n"
           "Ping: ok=%u lost=%u avg_rtt_ms=%u completed_ms=%u ARP=%s\n",
           restart_reason, static_cast<unsigned>(atem_connected.load()),
           static_cast<unsigned>(atem_initialized),
           static_cast<unsigned>(current_local_port),
           static_cast<unsigned>(consecutive_handshake_failures),
           static_cast<unsigned>(hello_tx_count),
           static_cast<unsigned>(hello_rx_count),
           static_cast<unsigned>(hello_close_rx_count),
           static_cast<unsigned>(ack_tx_count),
           static_cast<unsigned>(ack_retry_ok.load()),
           static_cast<unsigned>(ack_dropped.load()),
           static_cast<unsigned>(duplicate_recent),
           static_cast<unsigned>(fresh_port_last_granted_ms.load()),
           static_cast<unsigned>(fresh_port_denied_count.load()),
           static_cast<unsigned>(soft_probe_count.load()),
           static_cast<unsigned>(soft_probe_revived_count.load()),
           static_cast<unsigned>(wifi_bounce_count.load()),
           static_cast<unsigned>(ping_ok), static_cast<unsigned>(ping_lost),
           static_cast<unsigned>(ping_rtt), static_cast<unsigned>(ping_time),
           arp_found ? "present" : "missing");

    append("Sessions (oldest first):\n");
    for (size_t index = 0; index < copied_session_count; ++index) {
        const SessionRecord& session = sessions_copy[index];
        if (session.reconnect_delay_ms == UINT32_MAX) {
            append("%u lifetime_s=%u last_rx_id=%u probe_revived=%u "
                   "cause=%s reconnect_delay=pending\n",
                   static_cast<unsigned>(session.start_ms),
                   static_cast<unsigned>(session.lifetime_s),
                   static_cast<unsigned>(session.last_rx_id),
                   static_cast<unsigned>(session.probe_revived), session.cause);
        } else {
            append("%u lifetime_s=%u last_rx_id=%u probe_revived=%u "
                   "cause=%s reconnect_delay_ms=%u\n",
                   static_cast<unsigned>(session.start_ms),
                   static_cast<unsigned>(session.lifetime_s),
                   static_cast<unsigned>(session.last_rx_id),
                   static_cast<unsigned>(session.probe_revived), session.cause,
                   static_cast<unsigned>(session.reconnect_delay_ms));
        }
    }

    append("Packet headers (oldest first):\n");
    for (size_t index = 0; index < copied_packet_count; ++index) {
        const PacketHeaderRecord& packet = packets_copy[index];
        append("%u %c flags=0x%02x id=%u ack=%u session=0x%04x len=%u cmd=%.4s\n",
               static_cast<unsigned>(packet.time_ms), packet.direction,
               packet.flags, static_cast<unsigned>(packet.packet_id),
               static_cast<unsigned>(packet.ack_id),
               static_cast<unsigned>(packet.session_id),
               static_cast<unsigned>(packet.length), packet.command);
    }

    append("Events (oldest first):\n");
    for (size_t index = 0; index < copied_event_count && used < capacity - 1; ++index) {
        const DiagnosticEvent& event = events_copy[index];
        append("%u %s %s\n", static_cast<unsigned>(event.time_ms),
               event.type, event.details);
    }
    output[used] = '\0';
    return used;
}

void tally_mark_restart_pending() {
    restart_pending.store(true);
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
    if (restart_record.magic == 0x4154454D) {
        ESP_LOGW(kTag, "Last restart reason: %s", restart_record.reason);
        record_diagnostic_event("last_restart", restart_record.reason);
    }
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