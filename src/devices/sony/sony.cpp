#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <vector>

#include <bluetooth/bluetooth.h>
#include <bluetooth/rfcomm.h>
#include <bluetooth/sdp.h>
#include <bluetooth/sdp_lib.h>
#include <gio/gio.h>

#include "../../common.hpp"
#include "../../fd.hpp"
#include "sony.hpp"

namespace {

struct GErrorHolder {
    ~GErrorHolder() {
        if (error) g_error_free(error);
    }

    GError* error = nullptr;
};

constexpr std::string_view kSonyMdrRfcommUuid = "956C7B26-D49A-4BA8-B03F-B17D393CB6E2";

constexpr uint8_t kStartMarker = 0x3e;
constexpr uint8_t kEndMarker = 0x3c;
constexpr uint8_t kEscapeMarker = 0x3d;
constexpr uint8_t kEscaped60 = 0x2c;
constexpr uint8_t kEscaped61 = 0x2d;
constexpr uint8_t kEscaped62 = 0x2e;

constexpr uint8_t kDataMdr = 0x0c;
constexpr uint8_t kAck = 0x01;

constexpr uint8_t kConnectGetProtocolInfo = 0x00;
constexpr uint8_t kConnectRetProtocolInfo = 0x01;
constexpr uint8_t kConnectGetSupportFunction = 0x06;
constexpr uint8_t kConnectRetSupportFunction = 0x07;
constexpr uint8_t kPowerGetStatus = 0x22;
constexpr uint8_t kPowerRetStatus = 0x23;

constexpr uint8_t kFixedValue = 0x00;
constexpr uint8_t kLeftRightBattery = 0x01;
constexpr uint8_t kCradleBattery = 0x02;
constexpr uint8_t kLeftRightBatteryWithThreshold = 0x09;
constexpr uint8_t kCradleBatteryWithThreshold = 0x0a;

struct Packet {
    uint8_t type = 0;
    uint8_t seq = 0;
    std::vector<uint8_t> payload;
};

uint8_t checksum(const std::vector<uint8_t>& data) {
    uint8_t sum = 0;
    for (uint8_t byte : data) sum += byte;
    return sum;
}

void append_be32(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(static_cast<uint8_t>((value >> 24) & 0xff));
    out.push_back(static_cast<uint8_t>((value >> 16) & 0xff));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xff));
    out.push_back(static_cast<uint8_t>(value & 0xff));
}

std::vector<uint8_t> escape_payload(const std::vector<uint8_t>& data) {
    std::vector<uint8_t> out;
    out.reserve(data.size() * 2);
    for (uint8_t byte : data) {
        if (byte == 0x3c) {
            out.push_back(kEscapeMarker);
            out.push_back(kEscaped60);
        } else if (byte == 0x3d) {
            out.push_back(kEscapeMarker);
            out.push_back(kEscaped61);
        } else if (byte == 0x3e) {
            out.push_back(kEscapeMarker);
            out.push_back(kEscaped62);
        } else {
            out.push_back(byte);
        }
    }
    return out;
}

std::optional<std::vector<uint8_t>> unescape_payload(const std::vector<uint8_t>& data) {
    std::vector<uint8_t> out;
    out.reserve(data.size());

    for (size_t i = 0; i < data.size(); ++i) {
        uint8_t byte = data[i];
        if (byte != kEscapeMarker) {
            out.push_back(byte);
            continue;
        }

        if (++i >= data.size()) return std::nullopt;
        switch (data[i]) {
        case kEscaped60: out.push_back(0x3c); break;
        case kEscaped61: out.push_back(0x3d); break;
        case kEscaped62: out.push_back(0x3e); break;
        default: return std::nullopt;
        }
    }

    return out;
}

std::vector<uint8_t> pack_command(uint8_t type, uint8_t seq, const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> body;
    body.reserve(payload.size() + 7);
    body.push_back(type);
    body.push_back(seq);
    append_be32(body, static_cast<uint32_t>(payload.size()));
    body.insert(body.end(), payload.begin(), payload.end());
    body.push_back(checksum(body));

    std::vector<uint8_t> packet;
    packet.push_back(kStartMarker);
    auto escaped = escape_payload(body);
    packet.insert(packet.end(), escaped.begin(), escaped.end());
    packet.push_back(kEndMarker);
    return packet;
}

std::optional<Packet> unpack_command(const std::vector<uint8_t>& framed) {
    if (framed.size() < 8 || framed.front() != kStartMarker || framed.back() != kEndMarker) {
        return std::nullopt;
    }

    std::vector<uint8_t> escaped(framed.begin() + 1, framed.end() - 1);
    auto maybe_body = unescape_payload(escaped);
    if (!maybe_body || maybe_body->size() < 7) return std::nullopt;

    const auto& body = *maybe_body;
    uint8_t expected_checksum = body.back();
    std::vector<uint8_t> check_data(body.begin(), body.end() - 1);
    if (checksum(check_data) != expected_checksum) return std::nullopt;

    uint32_t size = (static_cast<uint32_t>(body[2]) << 24) |
                    (static_cast<uint32_t>(body[3]) << 16) |
                    (static_cast<uint32_t>(body[4]) << 8) |
                    static_cast<uint32_t>(body[5]);

    if (body.size() != static_cast<size_t>(size) + 7) return std::nullopt;

    Packet packet;
    packet.type = body[0];
    packet.seq = body[1];
    packet.payload.assign(body.begin() + 6, body.end() - 1);
    return packet;
}

int uuid_to_bytes(std::string_view uuid, std::array<uint8_t, 16>& out) {
    if (uuid.size() != 36) return -1;

    unsigned int bytes[16] = {};
    int parsed = std::sscanf(std::string(uuid).c_str(),
                             "%2x%2x%2x%2x-%2x%2x-%2x%2x-%2x%2x-%2x%2x%2x%2x%2x%2x",
                             &bytes[0], &bytes[1], &bytes[2], &bytes[3],
                             &bytes[4], &bytes[5], &bytes[6], &bytes[7],
                             &bytes[8], &bytes[9], &bytes[10], &bytes[11],
                             &bytes[12], &bytes[13], &bytes[14], &bytes[15]);
    if (parsed != 16) return -1;

    for (size_t i = 0; i < out.size(); ++i) out[i] = static_cast<uint8_t>(bytes[i]);
    return 0;
}

uint8_t find_rfcomm_channel(const std::string& mac) {
    bdaddr_t target{};
    str2ba(mac.c_str(), &target);

    const bdaddr_t any = {{0, 0, 0, 0, 0, 0}};
    sdp_session_t* session = sdp_connect(&any, &target, 0);
    if (!session) throw std::runtime_error("failed to connect to remote SDP server");

    std::array<uint8_t, 16> uuid_bytes{};
    if (uuid_to_bytes(kSonyMdrRfcommUuid, uuid_bytes) != 0) {
        sdp_close(session);
        throw std::runtime_error("invalid MDR service UUID");
    }

    uuid_t service_uuid{};
    sdp_uuid128_create(&service_uuid, uuid_bytes.data());
    sdp_list_t* search_list = sdp_list_append(nullptr, &service_uuid);

    uint32_t range = 0x0000ffff;
    sdp_list_t* attrid_list = sdp_list_append(nullptr, &range);
    sdp_list_t* response_list = nullptr;

    uint8_t port = 0;
    int status = sdp_service_search_attr_req(session, search_list, SDP_ATTR_REQ_RANGE, attrid_list, &response_list);
    if (status == 0) {
        for (sdp_list_t* item = response_list; item; item = item->next) {
            auto* record = static_cast<sdp_record_t*>(item->data);
            sdp_list_t* proto_list = nullptr;
            if (sdp_get_access_protos(record, &proto_list) == 0) {
                port = sdp_get_proto_port(proto_list, RFCOMM_UUID);
                sdp_list_free(proto_list, nullptr);
            }
            sdp_record_free(record);
            if (port != 0) break;
        }
    }

    sdp_list_free(response_list, nullptr);
    sdp_list_free(search_list, nullptr);
    sdp_list_free(attrid_list, nullptr);
    sdp_close(session);

    if (port == 0) throw std::runtime_error("Sony MDR RFCOMM service not found");
    return port;
}

std::string bluez_device_path_for(const std::string& mac) {
    std::string path = "/org/bluez/hci0/dev_";
    for (char ch : mac) path += ch == ':' ? '_' : ch;
    return path;
}

bool device_connected(const std::string& mac) {
    GErrorHolder err;
    GDBusConnection* bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, nullptr, &err.error);
    if (!bus) return false;

    GVariant* result = g_dbus_connection_call_sync(
        bus,
        "org.bluez",
        bluez_device_path_for(mac).c_str(),
        "org.freedesktop.DBus.Properties",
        "Get",
        g_variant_new("(ss)", "org.bluez.Device1", "Connected"),
        G_VARIANT_TYPE("(v)"),
        G_DBUS_CALL_FLAGS_NONE,
        5000,
        nullptr,
        &err.error);

    g_object_unref(bus);
    if (!result) return false;

    GVariant* value = nullptr;
    g_variant_get(result, "(v)", &value);
    bool connected = g_variant_get_boolean(value);
    g_variant_unref(value);
    g_variant_unref(result);
    return connected;
}

bool wait_fd(int fd, short events, std::chrono::milliseconds timeout) {
    pollfd pfd{.fd = fd, .events = events, .revents = 0};
    int rc = poll(&pfd, 1, static_cast<int>(timeout.count()));
    if (rc <= 0) return false;
    return (pfd.revents & events) != 0;
}

Fd connect_rfcomm(const std::string& mac, uint8_t channel) {
    Fd fd(socket(AF_BLUETOOTH, SOCK_STREAM | SOCK_NONBLOCK, BTPROTO_RFCOMM));
    if (!fd) throw std::runtime_error("failed to create RFCOMM socket");

    unsigned int link_mode = RFCOMM_LM_AUTH | RFCOMM_LM_ENCRYPT;
    setsockopt(fd.get(), SOL_RFCOMM, RFCOMM_LM, &link_mode, sizeof(link_mode));

    sockaddr_rc addr{};
    addr.rc_family = AF_BLUETOOTH;
    addr.rc_channel = channel;
    str2ba(mac.c_str(), &addr.rc_bdaddr);

    int rc = connect(fd.get(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    if (rc != 0 && errno != EINPROGRESS) {
        throw std::runtime_error(std::string("RFCOMM connect failed: ") + std::strerror(errno));
    }

    if (rc != 0) {
        if (!wait_fd(fd.get(), POLLOUT, std::chrono::seconds(15))) {
            throw std::runtime_error("RFCOMM connect timed out");
        }

        int error = 0;
        socklen_t len = sizeof(error);
        if (getsockopt(fd.get(), SOL_SOCKET, SO_ERROR, &error, &len) != 0 || error != 0) {
            throw std::runtime_error(std::string("RFCOMM connect failed: ") + std::strerror(error));
        }
    }

    return fd;
}

void send_all(int fd, const std::vector<uint8_t>& data) {
    size_t offset = 0;
    while (offset < data.size()) {
        if (!wait_fd(fd, POLLOUT, std::chrono::seconds(3))) {
            throw std::runtime_error("send timed out");
        }

        ssize_t sent = send(fd, data.data() + offset, data.size() - offset, 0);
        if (sent < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            throw std::runtime_error(std::string("send failed: ") + std::strerror(errno));
        }
        if (sent == 0) throw std::runtime_error("send failed: connection closed");
        offset += static_cast<size_t>(sent);
    }
}

class SonyBatteryClient {
public:
    explicit SonyBatteryClient(std::string mac) : mac_(std::move(mac)) {}

    Battery read() {
        uint8_t channel = find_rfcomm_channel(mac_);
        fd_ = connect_rfcomm(mac_, channel);

        request({kConnectGetProtocolInfo, kFixedValue}, kConnectRetProtocolInfo, kFixedValue, std::chrono::seconds(5));
        request({kConnectGetSupportFunction, kFixedValue}, kConnectRetSupportFunction, kFixedValue, std::chrono::seconds(5));

        Battery battery;

        auto lr = request({kPowerGetStatus, kLeftRightBatteryWithThreshold},
                          kPowerRetStatus, kLeftRightBatteryWithThreshold, std::chrono::seconds(5));
        if (!lr) {
            lr = request({kPowerGetStatus, kLeftRightBattery},
                         kPowerRetStatus, kLeftRightBattery, std::chrono::seconds(5));
        }
        if (lr) parse_left_right(*lr, battery);

        auto case_battery = request({kPowerGetStatus, kCradleBatteryWithThreshold},
                                    kPowerRetStatus, kCradleBatteryWithThreshold, std::chrono::seconds(2));
        if (!case_battery) {
            case_battery = request({kPowerGetStatus, kCradleBattery},
                                   kPowerRetStatus, kCradleBattery, std::chrono::seconds(2));
        }
        if (case_battery) parse_case(*case_battery, battery);

        if (!battery.has_lr()) throw std::runtime_error("left/right battery unavailable");
        return battery;
    }

private:
    std::string mac_;
    Fd fd_;
    uint8_t seq_ = 0;
    std::vector<uint8_t> rx_;

    std::optional<std::vector<uint8_t>> request(std::vector<uint8_t> payload,
                                                uint8_t expected_command,
                                                uint8_t expected_type,
                                                std::chrono::milliseconds timeout) {
        send_all(fd_.get(), pack_command(kDataMdr, seq_, payload));

        auto deadline = std::chrono::steady_clock::now() + timeout;
        std::optional<std::vector<uint8_t>> response;
        bool got_ack = false;

        while (std::chrono::steady_clock::now() < deadline) {
            auto packet = receive_until(deadline);
            if (!packet) continue;

            seq_ = packet->seq;

            if (packet->type == kAck) {
                got_ack = true;
                continue;
            }

            if (packet->type != kDataMdr) continue;

            send_ack(packet->seq);

            if (packet->payload.size() >= 2 &&
                packet->payload[0] == expected_command &&
                packet->payload[1] == expected_type) {
                response = packet->payload;
                if (got_ack) return response;
            }
        }

        return response;
    }

    std::optional<Packet> receive_until(std::chrono::steady_clock::time_point deadline) {
        while (std::chrono::steady_clock::now() < deadline) {
            if (auto packet = pop_packet()) return packet;

            auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
            if (remaining.count() <= 0) return std::nullopt;
            if (!wait_fd(fd_.get(), POLLIN, std::min(remaining, std::chrono::milliseconds(250)))) {
                continue;
            }

            std::array<uint8_t, 2048> buffer{};
            ssize_t received = recv(fd_.get(), buffer.data(), buffer.size(), 0);
            if (received < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
                throw std::runtime_error(std::string("receive failed: ") + std::strerror(errno));
            }
            if (received == 0) throw std::runtime_error("connection closed");
            rx_.insert(rx_.end(), buffer.begin(), buffer.begin() + received);
        }

        return std::nullopt;
    }

    std::optional<Packet> pop_packet() {
        auto begin = std::find(rx_.begin(), rx_.end(), kStartMarker);
        if (begin == rx_.end()) {
            rx_.clear();
            return std::nullopt;
        }
        if (begin != rx_.begin()) rx_.erase(rx_.begin(), begin);

        auto end = std::find(rx_.begin() + 1, rx_.end(), kEndMarker);
        if (end == rx_.end()) return std::nullopt;

        std::vector<uint8_t> framed(rx_.begin(), end + 1);
        rx_.erase(rx_.begin(), end + 1);
        return unpack_command(framed);
    }

    void send_ack(uint8_t packet_seq) {
        send_all(fd_.get(), pack_command(kAck, static_cast<uint8_t>(1 - packet_seq), {}));
    }

    static void parse_left_right(const std::vector<uint8_t>& payload, Battery& battery) {
        if (payload.size() < 6) return;
        battery.left = payload[2];
        battery.right = payload[4];
    }

    static void parse_case(const std::vector<uint8_t>& payload, Battery& battery) {
        if (payload.size() < 4) return;
        battery.case_level = payload[2];
    }
};

} // namespace

namespace devices::sony {

bool available(const std::string& mac) {
    try {
        (void)find_rfcomm_channel(mac);
        return true;
    } catch (...) {
        return false;
    }
}

bool connected(const std::string& mac) {
    return device_connected(mac);
}

Battery read_battery(const std::string& mac) {
    SonyBatteryClient client(mac);
    return client.read();
}

} // namespace devices::sony
