#include "airpods.hpp"

#include <array>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <mutex>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <glib-unix.h>

#include "../../common.hpp"
#include "../../fd.hpp"

namespace {

constexpr const char* kAirpodsUuid = "74ec2172-0bad-4d01-8f77-997b2be0722a";
constexpr const char* kProfilePath = "/com/earbud_battery/airpods/profile";
constexpr const char* kDeviceLabel = "AirPods";

constexpr std::array<uint8_t, 16> kHandshake{
    0x00, 0x00, 0x04, 0x00, 0x01, 0x00, 0x02, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

constexpr std::array<uint8_t, 10> kRequestNotifications{
    0x04, 0x00, 0x04, 0x00, 0x0f, 0x00, 0xff, 0xff, 0xff, 0xff,
};

constexpr std::array<uint8_t, 6> kBatteryPrefix{
    0x04, 0x00, 0x04, 0x00, 0x04, 0x00,
};

constexpr uint8_t kBatterySingle = 0x01;
constexpr uint8_t kBatteryRight = 0x02;
constexpr uint8_t kBatteryLeft = 0x04;
constexpr uint8_t kBatteryCase = 0x08;

constexpr const char* kProfileXml = R"xml(
<node>
  <interface name="org.bluez.Profile1">
    <method name="Release"/>
    <method name="NewConnection">
      <arg type="o" name="device" direction="in"/>
      <arg type="h" name="fd" direction="in"/>
      <arg type="a{sv}" name="fd_properties" direction="in"/>
    </method>
    <method name="RequestDisconnection">
      <arg type="o" name="device" direction="in"/>
    </method>
  </interface>
</node>
)xml";

std::string device_path_for(const std::string& mac) {
    std::string path = "/org/bluez/hci0/dev_";
    for (char ch : mac) path += ch == ':' ? '_' : ch;
    return path;
}

struct GErrorHolder {
    ~GErrorHolder() {
        if (error) g_error_free(error);
    }

    GError* error = nullptr;
};

bool variant_string_array_contains(GVariant* value, const char* needle) {
    if (!value) return false;

    GVariantIter iter;
    const char* item = nullptr;
    g_variant_iter_init(&iter, value);
    while (g_variant_iter_next(&iter, "&s", &item)) {
        if (g_ascii_strcasecmp(item, needle) == 0) return true;
    }
    return false;
}

GVariant* get_device_property(GDBusConnection* bus, const std::string& device_path, const char* property) {
    GErrorHolder err;
    GVariant* result = g_dbus_connection_call_sync(
        bus,
        "org.bluez",
        device_path.c_str(),
        "org.freedesktop.DBus.Properties",
        "Get",
        g_variant_new("(ss)", "org.bluez.Device1", property),
        G_VARIANT_TYPE("(v)"),
        G_DBUS_CALL_FLAGS_NONE,
        3000,
        nullptr,
        &err.error);

    if (!result) return nullptr;

    GVariant* wrapped = nullptr;
    g_variant_get(result, "(@v)", &wrapped);
    GVariant* value = g_variant_get_variant(wrapped);
    g_variant_unref(wrapped);
    g_variant_unref(result);
    return value;
}

bool device_connected(GDBusConnection* bus, const std::string& device_path) {
    GVariant* value = get_device_property(bus, device_path, "Connected");
    if (!value) return false;
    bool connected = g_variant_get_boolean(value);
    g_variant_unref(value);
    return connected;
}

void call_device_method(GDBusConnection* bus, const std::string& device_path, const char* method) {
    GErrorHolder err;
    GVariant* result = g_dbus_connection_call_sync(
        bus,
        "org.bluez",
        device_path.c_str(),
        "org.bluez.Device1",
        method,
        nullptr,
        nullptr,
        G_DBUS_CALL_FLAGS_NONE,
        10000,
        nullptr,
        &err.error);
    if (result) g_variant_unref(result);
}

void connect_profile(GDBusConnection* bus, const std::string& device_path) {
    for (int attempt = 0; attempt < 4; ++attempt) {
        GErrorHolder err;
        GVariant* result = g_dbus_connection_call_sync(
            bus,
            "org.bluez",
            device_path.c_str(),
            "org.bluez.Device1",
            "ConnectProfile",
            g_variant_new("(s)", kAirpodsUuid),
            nullptr,
            G_DBUS_CALL_FLAGS_NONE,
            5000,
            nullptr,
            &err.error);

        if (result) {
            g_variant_unref(result);
            return;
        }

        if (!err.error || (std::string(err.error->message).find("InProgress") == std::string::npos &&
                           std::string(err.error->message).find("busy") == std::string::npos)) {
            if (err.error) std::cerr << "airpods: ConnectProfile: " << err.error->message << "\n";
            return;
        }

        sleep(static_cast<unsigned int>(1 + attempt));
    }
}

void send_all(int fd, const uint8_t* data, size_t size) {
    size_t offset = 0;
    while (offset < size) {
        ssize_t sent = send(fd, data + offset, size - offset, 0);
        if (sent < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                pollfd pfd{.fd = fd, .events = POLLOUT, .revents = 0};
                poll(&pfd, 1, 3000);
                continue;
            }
            throw std::runtime_error(std::string("send failed: ") + std::strerror(errno));
        }
        if (sent == 0) throw std::runtime_error("connection closed");
        offset += static_cast<size_t>(sent);
    }
}

bool wait_readable(int fd, int timeout_ms) {
    pollfd pfd{.fd = fd, .events = POLLIN, .revents = 0};
    int rc = poll(&pfd, 1, timeout_ms);
    if (rc < 0 && errno == EINTR) return false;
    return rc > 0 && (pfd.revents & POLLIN) != 0;
}

class AirpodsMonitor {
public:
    explicit AirpodsMonitor(std::string mac, bool watch)
        : mac_(std::move(mac)), device_path_(device_path_for(mac_)), watch_(watch) {}

    int run() {
        GErrorHolder err;
        bus_ = g_bus_get_sync(G_BUS_TYPE_SYSTEM, nullptr, &err.error);
        if (!bus_) {
            std::cerr << "airpods: failed to open system bus: " << err.error->message << "\n";
            print_disconnected(kDeviceLabel);
            return 1;
        }

        if (!register_profile()) {
            print_disconnected(kDeviceLabel);
            return 1;
        }

        bool connected = device_connected(bus_, device_path_);
        if (connected) {
            set_connected(true);
            std::thread([this] { connect_profile(bus_, device_path_); }).detach();
        }

        subscribe_properties();
        if (watch_) {
            print_current();
        } else if (!connected) {
            print_disconnected(kDeviceLabel);
            unregister_profile();
            return 1;
        }

        loop_ = g_main_loop_new(nullptr, false);
        g_unix_signal_add(SIGUSR1, &AirpodsMonitor::on_toggle_signal, this);
        g_unix_signal_add(SIGTERM, &AirpodsMonitor::on_quit_signal, this);
        g_unix_signal_add(SIGINT, &AirpodsMonitor::on_quit_signal, this);
        g_main_loop_run(loop_);

        unregister_profile();
        g_main_loop_unref(loop_);
        loop_ = nullptr;
        return 0;
    }

private:
    std::string mac_;
    std::string device_path_;
    GDBusConnection* bus_ = nullptr;
    GDBusNodeInfo* introspection_ = nullptr;
    guint object_id_ = 0;
    guint signal_id_ = 0;
    GMainLoop* loop_ = nullptr;
    std::mutex mutex_;
    Battery battery_;
    bool watch_ = false;
    bool connected_ = false;
    bool connecting_ = false;

    static gboolean on_toggle_signal(gpointer user_data) {
        auto* self = static_cast<AirpodsMonitor*>(user_data);
        self->toggle_connection();
        return G_SOURCE_CONTINUE;
    }

    static gboolean on_quit_signal(gpointer user_data) {
        auto* self = static_cast<AirpodsMonitor*>(user_data);
        if (self->loop_) g_main_loop_quit(self->loop_);
        return G_SOURCE_REMOVE;
    }

    bool register_profile() {
        GErrorHolder err;
        introspection_ = g_dbus_node_info_new_for_xml(kProfileXml, &err.error);
        if (!introspection_) {
            std::cerr << "airpods: failed to parse Profile1 XML: " << err.error->message << "\n";
            return false;
        }

        static const GDBusInterfaceVTable vtable{
            &AirpodsMonitor::on_method_call,
            nullptr,
            nullptr,
            {nullptr}
        };

        object_id_ = g_dbus_connection_register_object(
            bus_,
            kProfilePath,
            introspection_->interfaces[0],
            &vtable,
            this,
            nullptr,
            &err.error);

        if (object_id_ == 0) {
            std::cerr << "airpods: failed to export Profile1 object: " << err.error->message << "\n";
            return false;
        }

        GVariantBuilder options;
        g_variant_builder_init(&options, G_VARIANT_TYPE("a{sv}"));
        g_variant_builder_add(&options, "{sv}", "Name", g_variant_new_string("Earbud Battery AirPods"));
        g_variant_builder_add(&options, "{sv}", "Role", g_variant_new_string("client"));
        g_variant_builder_add(&options, "{sv}", "AutoConnect", g_variant_new_boolean(true));

        GVariant* result = g_dbus_connection_call_sync(
            bus_,
            "org.bluez",
            "/org/bluez",
            "org.bluez.ProfileManager1",
            "RegisterProfile",
            g_variant_new("(osa{sv})", kProfilePath, kAirpodsUuid, &options),
            nullptr,
            G_DBUS_CALL_FLAGS_NONE,
            -1,
            nullptr,
            &err.error);

        if (!result) {
            std::cerr << "airpods: profile registration failed: " << err.error->message << "\n";
            return false;
        }

        g_variant_unref(result);
        return true;
    }

    void unregister_profile() {
        GErrorHolder err;
        GVariant* result = g_dbus_connection_call_sync(
            bus_,
            "org.bluez",
            "/org/bluez",
            "org.bluez.ProfileManager1",
            "UnregisterProfile",
            g_variant_new("(o)", kProfilePath),
            nullptr,
            G_DBUS_CALL_FLAGS_NONE,
            -1,
            nullptr,
            &err.error);
        if (result) g_variant_unref(result);

        if (signal_id_ != 0) g_dbus_connection_signal_unsubscribe(bus_, signal_id_);
        if (object_id_ != 0) g_dbus_connection_unregister_object(bus_, object_id_);
        if (introspection_) g_dbus_node_info_unref(introspection_);
    }

    void subscribe_properties() {
        signal_id_ = g_dbus_connection_signal_subscribe(
            bus_,
            "org.bluez",
            "org.freedesktop.DBus.Properties",
            "PropertiesChanged",
            device_path_.c_str(),
            nullptr,
            G_DBUS_SIGNAL_FLAGS_NONE,
            &AirpodsMonitor::on_properties_changed,
            this,
            nullptr);
    }

    static void on_method_call(GDBusConnection*,
                               const char*,
                               const char*,
                               const char*,
                               const char* method_name,
                               GVariant* parameters,
                               GDBusMethodInvocation* invocation,
                               gpointer user_data) {
        auto* self = static_cast<AirpodsMonitor*>(user_data);

        if (std::strcmp(method_name, "NewConnection") == 0) {
            const char* device = nullptr;
            int fd_index = -1;
            GVariant* properties = nullptr;
            g_variant_get(parameters, "(&oh@a{sv})", &device, &fd_index, &properties);
            if (properties) g_variant_unref(properties);

            GDBusMessage* message = g_dbus_method_invocation_get_message(invocation);
            GUnixFDList* fd_list = g_dbus_message_get_unix_fd_list(message);
            GErrorHolder err;
            int fd = fd_list ? g_unix_fd_list_get(fd_list, fd_index, &err.error) : -1;
            if (fd >= 0) {
                self->set_connected(true);
                std::thread(&AirpodsMonitor::socket_reader, self, fd).detach();
            }
            g_dbus_method_invocation_return_value(invocation, nullptr);
            return;
        }

        if (std::strcmp(method_name, "RequestDisconnection") == 0) {
            self->set_connected(false);
            g_dbus_method_invocation_return_value(invocation, nullptr);
            return;
        }

        g_dbus_method_invocation_return_value(invocation, nullptr);
    }

    static void on_properties_changed(GDBusConnection*,
                                      const char*,
                                      const char*,
                                      const char*,
                                      const char*,
                                      GVariant* parameters,
                                      gpointer user_data) {
        auto* self = static_cast<AirpodsMonitor*>(user_data);

        const char* iface = nullptr;
        GVariant* changed = nullptr;
        GVariant* invalidated = nullptr;
        g_variant_get(parameters, "(&s@a{sv}@as)", &iface, &changed, &invalidated);
        if (invalidated) g_variant_unref(invalidated);

        if (std::strcmp(iface, "org.bluez.Device1") != 0) {
            if (changed) g_variant_unref(changed);
            return;
        }

        GVariant* connected = g_variant_lookup_value(changed, "Connected", G_VARIANT_TYPE_BOOLEAN);
        if (!connected) {
            if (changed) g_variant_unref(changed);
            return;
        }

        bool is_connected = g_variant_get_boolean(connected);
        g_variant_unref(connected);
        if (changed) g_variant_unref(changed);

        self->set_connected(is_connected);
        if (is_connected) {
            std::thread([self] { connect_profile(self->bus_, self->device_path_); }).detach();
        }
    }

    void toggle_connection() {
        std::lock_guard lock(mutex_);
        if (connecting_) return;

        if (connected_) {
            std::thread([this] { call_device_method(bus_, device_path_, "Disconnect"); }).detach();
            return;
        }

        connecting_ = true;
        print_current_locked();
        std::thread([this] {
            call_device_method(bus_, device_path_, "Connect");
            connect_profile(bus_, device_path_);
            std::lock_guard lock(mutex_);
            connecting_ = false;
            print_current_locked();
        }).detach();
    }

    void set_connected(bool connected) {
        std::lock_guard lock(mutex_);
        connected_ = connected;
        connecting_ = false;
        if (!connected_) battery_ = {};
        if (watch_ || !connected_) print_current_locked();
        if (!watch_ && !connected_ && loop_) g_main_loop_quit(loop_);
    }

    void print_current() {
        std::lock_guard lock(mutex_);
        print_current_locked();
    }

    void print_current_locked() {
        if (connecting_) {
            std::cout
                << "{\"text\":\"\","
                << "\"tooltip\":\"" << kDeviceLabel << " connecting...\","
                << "\"class\":\"connecting\","
                << "\"alt\":\"connecting\"}"
                << std::endl;
            return;
        }

        if (!connected_) {
            print_disconnected(kDeviceLabel);
            return;
        }

        print_battery(kDeviceLabel, battery_);
    }

    void socket_reader(int fd) {
        Fd owned(fd);
        try {
            send_all(owned.get(), kHandshake.data(), kHandshake.size());
            usleep(300000);
            send_all(owned.get(), kRequestNotifications.data(), kRequestNotifications.size());

            while (true) {
                if (!wait_readable(owned.get(), 30000)) {
                    send_all(owned.get(), kRequestNotifications.data(), kRequestNotifications.size());
                    continue;
                }

                std::array<uint8_t, 1024> buffer{};
                ssize_t received = recv(owned.get(), buffer.data(), buffer.size(), 0);
                if (received < 0) {
                    if (errno == EINTR) continue;
                    if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
                    throw std::runtime_error(std::string("receive failed: ") + std::strerror(errno));
                }
                if (received == 0) break;
                parse_battery({buffer.begin(), buffer.begin() + received});
            }
        } catch (const std::exception& ex) {
            std::cerr << "airpods: socket error: " << ex.what() << "\n";
        }

        set_connected(false);
    }

    void parse_battery(const std::vector<uint8_t>& data) {
        if (data.size() < 12 || !std::equal(kBatteryPrefix.begin(), kBatteryPrefix.end(), data.begin())) return;
        uint8_t count = data[6];
        if (count < 1 || count > 3) return;

        std::lock_guard lock(mutex_);
        size_t pos = 7;
        for (uint8_t i = 0; i < count; ++i) {
            if (pos + 4 >= data.size()) break;

            uint8_t type = data[pos];
            uint8_t raw_level = data[pos + 2];
            uint8_t status = data[pos + 3];
            bool disconnected = (status & 0x04) != 0;
            int level = raw_level > 100 || disconnected ? -1 : raw_level;

            if (!Battery::valid(level)) {
                pos += 5;
                continue;
            }

            if (type == kBatterySingle || type == kBatteryLeft) {
                battery_.left = level;
            } else if (type == kBatteryRight) {
                battery_.right = level;
            } else if (type == kBatteryCase) {
                battery_.case_level = level;
            }

            pos += 5;
        }
        print_battery(kDeviceLabel, battery_);
        if (!watch_ && loop_) g_main_loop_quit(loop_);
    }
};

} // namespace

namespace devices::airpods {

bool available(const std::string& mac) {
    GErrorHolder err;
    GDBusConnection* bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, nullptr, &err.error);
    if (!bus) return false;

    GVariant* uuids = get_device_property(bus, device_path_for(mac), "UUIDs");
    bool found = variant_string_array_contains(uuids, kAirpodsUuid);
    if (uuids) g_variant_unref(uuids);
    g_object_unref(bus);
    return found;
}

int run(const std::string& mac, bool watch) {
    AirpodsMonitor monitor(mac, watch);
    return monitor.run();
}

} // namespace devices::airpods
