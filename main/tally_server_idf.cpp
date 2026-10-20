#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "tally_server_idf.hpp"

namespace {

constexpr char kTag[] = "tally_relay";
constexpr uint16_t kPort = 9910;
constexpr size_t kMaxClients = 5;
constexpr size_t kMaxFlags = 41;
constexpr uint8_t kAck = 0x80;
constexpr uint8_t kResendRequest = 0x40;
constexpr uint8_t kResent = 0x20;
constexpr uint8_t kHello = 0x10;
constexpr uint8_t kAckRequest = 0x08;

struct Client {
    sockaddr_in address{};
    bool connected = false;
    bool initialized = false;
    uint16_t local_packet_id = 0;
    uint16_t session_id = 0;
    uint32_t last_received = 0;
    uint32_t last_sent = 0;
    uint16_t last_acked_id = 0;
};

std::array<Client, kMaxClients> clients{};
std::array<uint8_t, kMaxFlags> tally_flags{};
uint8_t tally_sources = 0;
bool tally_changed = true;
int server_socket = -1;
SemaphoreHandle_t tally_mutex;
bool relay_started = false;

uint32_t millis_now() {
    return static_cast<uint32_t>(esp_timer_get_time() / 1000);
}

uint16_t read_be16(const uint8_t* data) {
    return static_cast<uint16_t>((static_cast<uint16_t>(data[0]) << 8) | data[1]);
}

void write_be16(uint8_t* data, uint16_t value) {
    data[0] = static_cast<uint8_t>(value >> 8);
    data[1] = static_cast<uint8_t>(value);
}

bool same_address(const sockaddr_in& left, const sockaddr_in& right) {
    return left.sin_addr.s_addr == right.sin_addr.s_addr && left.sin_port == right.sin_port;
}

Client* find_client(const sockaddr_in& address) {
    Client* empty_slot = nullptr;
    for (Client& client : clients) {
        if (client.connected && same_address(client.address, address)) return &client;
        if (!client.connected && !empty_slot) empty_slot = &client;
    }
    if (empty_slot) {
        *empty_slot = Client{};
        empty_slot->address = address;
    }
    return empty_slot;
}

void send_packet(Client& client, uint8_t flags, const uint8_t* payload, size_t payload_length,
                 uint16_t ack_id = 0, bool resend = false, uint16_t resend_id = 0) {
    uint8_t packet[64]{};
    const size_t packet_length = 12 + payload_length;
    packet[0] = static_cast<uint8_t>(flags | ((packet_length >> 8) & 0x07));
    packet[1] = static_cast<uint8_t>(packet_length);
    write_be16(packet + 2, client.session_id);
    write_be16(packet + 4, ack_id);

    if (resend) {
        write_be16(packet + 10, resend_id);
    } else if ((flags & kAckRequest) && !(flags & (kResent | kResendRequest | kHello))) {
        client.local_packet_id++;
        write_be16(packet + 10, client.local_packet_id);
    }
    if (payload_length) memcpy(packet + 12, payload, payload_length);

    const int sent = sendto(server_socket, packet, packet_length, 0,
                            reinterpret_cast<const sockaddr*>(&client.address), sizeof(client.address));
    if (sent == static_cast<int>(packet_length)) client.last_sent = millis_now();
}

void send_tally(Client& client, uint8_t flags = kAckRequest, uint16_t resend_id = 0,
                bool resend = false) {
    std::array<uint8_t, 51> payload{};
    uint8_t source_count;
    std::array<uint8_t, kMaxFlags> current_flags{};
    xSemaphoreTake(tally_mutex, portMAX_DELAY);
    source_count = tally_sources;
    current_flags = tally_flags;
    xSemaphoreGive(tally_mutex);

    const uint16_t command_length = static_cast<uint16_t>(10 + source_count);
    write_be16(payload.data(), command_length);
    memcpy(payload.data() + 4, "TlIn", 4);
    write_be16(payload.data() + 8, source_count);
    memcpy(payload.data() + 10, current_flags.data(), source_count);
    send_packet(client, flags, payload.data(), command_length, 0, resend, resend_id);
}

void send_empty_ack(Client& client, uint16_t remote_id) {
    send_packet(client, kAck, nullptr, 0, remote_id);
}

void reset_client(Client& client) {
    client = Client{};
}

void handle_packet(const uint8_t* packet, size_t packet_size, const sockaddr_in& remote) {
    if (packet_size < 12) return;
    const size_t declared_size = ((packet[0] & 0x07) << 8) | packet[1];
    if (declared_size != packet_size) return;

    const uint8_t flags = packet[0] & 0xF8;
    const uint16_t session_id = read_be16(packet + 2);
    const uint16_t remote_packet_id = read_be16(packet + 10);
    Client* client = find_client(remote);

    if (!client) {
        if (flags & kHello) {
            Client rejected{};
            rejected.address = remote;
            rejected.session_id = session_id;
            uint8_t response[8]{};
            response[0] = 3;
            send_packet(rejected, kHello, response, sizeof(response));
        }
        return;
    }

    client->session_id = session_id;
    client->last_received = millis_now();

    if (client->initialized) {
        if (flags & kAck) client->last_acked_id = read_be16(packet + 4);
        if (flags & kAckRequest) send_empty_ack(*client, remote_packet_id);
        if (flags & kResendRequest) {
            const uint16_t wanted = static_cast<uint16_t>(read_be16(packet + 6) + 1);
            send_tally(*client, kResent | kAck | kAckRequest, wanted, true);
        }
        return;
    }

    if (client->connected) {
        if (flags & kAck) {
            send_tally(*client);
            send_packet(*client, kAckRequest, nullptr, 0);
            client->initialized = true;
        }
        return;
    }

    if (flags & kHello) {
        uint8_t response[8]{};
        response[0] = 2;
        send_packet(*client, kHello, response, sizeof(response));
        client->connected = true;
    } else {
        reset_client(*client);
    }
}

void service_clients() {
    const uint32_t now = millis_now();
    bool changed;
    xSemaphoreTake(tally_mutex, portMAX_DELAY);
    changed = tally_changed;
    tally_changed = false;
    xSemaphoreGive(tally_mutex);

    if (changed) {
        for (Client& client : clients) {
            if (client.initialized) send_tally(client);
        }
    }

    for (Client& client : clients) {
        if (!client.connected) continue;
        if (now - client.last_received >= 5000) {
            reset_client(client);
        } else if (client.initialized) {
            if (client.last_acked_id < client.local_packet_id && now - client.last_sent >= 250) {
                send_tally(client);
            } else if (now - client.last_received >= 1500 && now - client.last_sent >= 1500) {
                send_packet(client, kAckRequest, nullptr, 0);
            }
        } else if (now - client.last_sent >= 1500) {
            uint8_t response[8]{};
            response[0] = 2;
            send_packet(client, kHello, response, sizeof(response));
        }
    }
}

void tally_server_task(void*) {
    server_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (server_socket < 0) {
        ESP_LOGE(kTag, "Could not create UDP socket: errno %d", errno);
        vTaskDelete(nullptr);
        return;
    }

    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_port = htons(kPort);
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(server_socket, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0) {
        ESP_LOGE(kTag, "Could not bind UDP port %u: errno %d", kPort, errno);
        close(server_socket);
        server_socket = -1;
        vTaskDelete(nullptr);
        return;
    }

    timeval timeout{};
    timeout.tv_usec = 100000;
    setsockopt(server_socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    ESP_LOGI(kTag, "Tally relay listening on UDP %u", kPort);
    uint8_t packet[1500];
    while (true) {
        sockaddr_in remote{};
        socklen_t address_length = sizeof(remote);
        const int received = recvfrom(server_socket, packet, sizeof(packet), 0,
                                      reinterpret_cast<sockaddr*>(&remote), &address_length);
        if (received > 0) handle_packet(packet, static_cast<size_t>(received), remote);
        service_clients();
    }
}

}  // namespace

void tally_server_start() {
    if (relay_started) return;
    if (!tally_mutex) tally_mutex = xSemaphoreCreateMutex();
    if (!tally_mutex) {
        ESP_LOGE(kTag, "Could not create tally mutex");
        return;
    }
    if (xTaskCreate(tally_server_task, "tally_server", 4096, nullptr, 5, nullptr) == pdPASS) {
        relay_started = true;
    } else {
        ESP_LOGE(kTag, "Could not start tally relay task");
    }
}

void tally_server_set_sources(uint8_t count) {
    xSemaphoreTake(tally_mutex, portMAX_DELAY);
    const uint8_t safe_count = std::min<uint8_t>(count, kMaxFlags);
    if (tally_sources != safe_count) {
        tally_sources = safe_count;
        tally_changed = true;
    }
    xSemaphoreGive(tally_mutex);
}

void tally_server_set_flag(uint8_t index, uint8_t flags) {
    if (index >= kMaxFlags) return;
    xSemaphoreTake(tally_mutex, portMAX_DELAY);
    if (tally_flags[index] != flags) {
        tally_flags[index] = flags;
        tally_changed = true;
    }
    xSemaphoreGive(tally_mutex);
}

void tally_server_reset_flags() {
    if (!tally_mutex) return;
    xSemaphoreTake(tally_mutex, portMAX_DELAY);
    tally_flags.fill(0);
    tally_changed = true;
    xSemaphoreGive(tally_mutex);
}

void tally_server_set_all_flags(uint8_t count, const uint8_t* flags) {
    if (!tally_mutex || (!flags && count)) return;
    xSemaphoreTake(tally_mutex, portMAX_DELAY);
    tally_sources = std::min<uint8_t>(count, kMaxFlags);
    tally_flags.fill(0);
    if (tally_sources) memcpy(tally_flags.data(), flags, tally_sources);
    tally_changed = true;
    xSemaphoreGive(tally_mutex);
}