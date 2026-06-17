#include <charconv>
#include <chrono>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <gio/gio.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "audio.hpp"
#include "common.hpp"
#include "devices/airpods/airpods.hpp"
#include "devices/sony/sony.hpp"
#include "fd.hpp"

#ifndef WAYBAR_EARBUD_VERSION
#define WAYBAR_EARBUD_VERSION "unknown"
#endif

namespace {

enum class DeviceKind {
    Auto,
    Sony,
    AirPods,
};

struct Options {
    DeviceKind device = DeviceKind::Auto;
    bool service = false;
    bool watch = false;
    bool toggle = false;
    bool explicit_mac = false;
    std::optional<std::string> mac;
    std::string format = "{text}";
    std::string connected_format;
    std::string connecting_format;
    std::string disconnected_format;
    int interval = 0;
};

struct GErrorHolder {
    ~GErrorHolder() {
        if (error) g_error_free(error);
    }

    GError* error = nullptr;
};

struct SonyWatchState {
    std::mutex mutex;
    std::string label = "Sony earbuds";
    bool connected = false;
    bool connecting = false;
    bool reading = false;
    unsigned int generation = 0;
    int interval = 30;
    std::chrono::steady_clock::time_point next_refresh{};
};

constexpr int kSonyBatteryRetrySeconds = 2;

int sony_toggle_write_fd = -1;

struct JsonState {
    std::string text;
    std::string device;
    std::optional<int> battery;
    std::optional<int> left;
    std::optional<int> right;
    std::optional<int> case_level;
    std::string status;
    std::string klass;
    std::string alt;
};

std::optional<DeviceKind> parse_device_kind(std::string_view value) {
    if (value == "auto") return DeviceKind::Auto;
    if (value == "sony") return DeviceKind::Sony;
    if (value == "airpods") return DeviceKind::AirPods;
    return std::nullopt;
}

bool is_hex_digit(char value) {
    return std::isxdigit(static_cast<unsigned char>(value)) != 0;
}

bool is_mac_address(std::string_view value) {
    if (value.size() != 17) return false;

    for (std::size_t i = 0; i < value.size(); ++i) {
        if ((i + 1) % 3 == 0) {
            if (value[i] != ':') return false;
        } else if (!is_hex_digit(value[i])) {
            return false;
        }
    }

    return true;
}

std::optional<std::string> parse_mac(std::string_view value) {
    if (!is_mac_address(value)) return std::nullopt;
    return std::string(value);
}

bool parse_interval(std::string_view value, int& interval) {
    int parsed = 0;
    auto* begin = value.data();
    auto* end = value.data() + value.size();
    auto result = std::from_chars(begin, end, parsed);
    if (result.ec != std::errc{} || result.ptr != end || parsed < 0) return false;
    interval = parsed;
    return true;
}

void print_usage(std::ostream& out) {
    out
        << "Usage: waybar-earbud [options]\n"
        << "\n"
        << "Waybar helper for Bluetooth earbud battery status.\n"
        << "\n"
        << "Run one foreground service, then point Waybar at client invocations:\n"
        << "  waybar-earbud --service\n"
        << "  waybar-earbud --watch\n"
        << "\n"
        << "Options:\n"
        << "  --service                   Own Bluetooth providers and serve clients over a Unix socket.\n"
        << "  --device auto|sony|airpods  Provider to use in service mode. Defaults to auto.\n"
        << "  --mac AA:BB:CC:DD:EE:FF     Pin service to this Bluetooth MAC instead of current audio output.\n"
        << "  --watch                     Client mode: stream JSON from the service.\n"
        << "  --toggle                    Client mode: toggle service connect/disconnect and exit.\n"
        << "  --preset battery|split|icon\n"
        << "  --format template           Client text template. Defaults to {text}.\n"
        << "  --connected-format template\n"
        << "  --connecting-format template\n"
        << "  --disconnected-format template\n"
        << "  --interval seconds          Sony service battery refresh interval. Defaults to 30, minimum 5.\n"
        << "  -v, --version               Print version and exit.\n"
        << "  -h, --help                  Show this help and exit.\n";
}

void print_version() {
    std::cout << "waybar-earbud " << WAYBAR_EARBUD_VERSION << "\n";
}

bool apply_preset(Options& options, std::string_view preset) {
    if (preset == "battery") {
        options.connected_format = "{?battery}{battery}%{/battery}";
        options.connecting_format = "";
        options.disconnected_format = "";
        return true;
    }

    if (preset == "split") {
        options.connected_format = "{?left}L:{left}%{/left}{?right} R:{right}%{/right}{?case} C:{case}%{/case}";
        options.connecting_format = "";
        options.disconnected_format = "";
        return true;
    }

    if (preset == "icon") {
        options.connected_format = "\U000f184f{?left} \U000f0c0d  {left}%{/left}{?right} \U000f0c1f  {right}%{/right}{?case} \uED75  {case}%{/case}";
        options.connecting_format = "\U000f184f";
        options.disconnected_format = "\U000f1850";
        return true;
    }

    return false;
}

std::string runtime_prefix() {
    const char* xdg_runtime_dir = std::getenv("XDG_RUNTIME_DIR");
    if (xdg_runtime_dir && *xdg_runtime_dir) return std::string(xdg_runtime_dir) + "/waybar-earbud";
    return "/tmp/waybar-earbud-" + std::to_string(getuid());
}

std::string socket_path() {
    return runtime_prefix() + ".sock";
}

void write_all(int fd, std::string_view text) {
    while (!text.empty()) {
        ssize_t written = write(fd, text.data(), text.size());
        if (written < 0) {
            if (errno == EINTR) continue;
            return;
        }
        text.remove_prefix(static_cast<size_t>(written));
    }
}

bool write_line(int fd, const std::string& line) {
    std::string output = line + "\n";
    size_t offset = 0;
    while (offset < output.size()) {
        ssize_t written = send(fd, output.data() + offset, output.size() - offset, MSG_NOSIGNAL);
        if (written < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (written == 0) return false;
        offset += static_cast<size_t>(written);
    }
    return true;
}

std::optional<std::string> read_line(int fd, int timeout_ms = -1) {
    std::string line;
    char ch = '\0';
    while (true) {
        if (timeout_ms >= 0) {
            pollfd pfd{.fd = fd, .events = POLLIN, .revents = 0};
            int rc = poll(&pfd, 1, timeout_ms);
            if (rc < 0) {
                if (errno == EINTR) continue;
                return std::nullopt;
            }
            if (rc == 0 || (pfd.revents & POLLIN) == 0) return std::nullopt;
        }

        ssize_t count = read(fd, &ch, 1);
        if (count < 0) {
            if (errno == EINTR) continue;
            return std::nullopt;
        }
        if (count == 0) return std::nullopt;
        if (ch == '\n') return line;
        line += ch;
    }
}

std::optional<std::string> json_string_value(const std::string& json, const std::string& key) {
    std::string needle = "\"" + key + "\":\"";
    size_t start = json.find(needle);
    if (start == std::string::npos) return std::nullopt;
    start += needle.size();

    std::string value;
    bool escape = false;
    for (size_t i = start; i < json.size(); ++i) {
        char ch = json[i];
        if (escape) {
            value += ch == 'n' ? '\n' : ch;
            escape = false;
            continue;
        }
        if (ch == '\\') {
            escape = true;
            continue;
        }
        if (ch == '"') return value;
        value += ch;
    }

    return std::nullopt;
}

std::optional<int> json_int_value(const std::string& json, const std::string& key) {
    std::string needle = "\"" + key + "\":";
    size_t start = json.find(needle);
    if (start == std::string::npos) return std::nullopt;
    start += needle.size();

    if (json.compare(start, 4, "null") == 0) return std::nullopt;

    int value = 0;
    auto* begin = json.data() + start;
    auto* end = json.data() + json.size();
    auto result = std::from_chars(begin, end, value);
    if (result.ec != std::errc{}) return std::nullopt;
    return value;
}

std::optional<JsonState> parse_state_json(const std::string& json) {
    auto text = json_string_value(json, "text");
    auto device = json_string_value(json, "device");
    auto status = json_string_value(json, "status");
    auto klass = json_string_value(json, "class");
    auto alt = json_string_value(json, "alt");
    if (!text || !status || !klass || !alt) return std::nullopt;

    JsonState state;
    state.text = *text;
    state.device = device.value_or("Earbuds");
    state.battery = json_int_value(json, "battery");
    state.left = json_int_value(json, "left");
    state.right = json_int_value(json, "right");
    state.case_level = json_int_value(json, "case");
    state.status = *status;
    state.klass = *klass;
    state.alt = *alt;
    return state;
}

std::string charge_value(const std::optional<int>& value) {
    return value ? std::to_string(*value) : "";
}

std::string title_status(std::string status) {
    if (!status.empty()) status[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(status[0])));
    return status;
}

void replace_all(std::string& value, const std::string& needle, const std::string& replacement) {
    size_t pos = 0;
    while ((pos = value.find(needle, pos)) != std::string::npos) {
        value.replace(pos, needle.size(), replacement);
        pos += replacement.size();
    }
}

bool field_present(const JsonState& state, const std::string& field) {
    if (field == "text") return !state.text.empty();
    if (field == "device") return !state.device.empty();
    if (field == "battery") return state.battery.has_value();
    if (field == "left") return state.left.has_value();
    if (field == "right") return state.right.has_value();
    if (field == "case") return state.case_level.has_value();
    if (field == "status") return !state.status.empty();
    if (field == "class") return !state.klass.empty();
    if (field == "alt") return !state.alt.empty();
    return false;
}

void render_optional_blocks(std::string& templ, const JsonState& state) {
    size_t search = 0;
    while (true) {
        size_t open = templ.find("{?", search);
        if (open == std::string::npos) return;

        size_t name_end = templ.find("}", open + 2);
        if (name_end == std::string::npos) return;

        std::string field = templ.substr(open + 2, name_end - open - 2);
        std::string close_token = "{/" + field + "}";
        size_t close = templ.find(close_token, name_end + 1);
        if (close == std::string::npos) return;

        size_t body_start = name_end + 1;
        std::string body = templ.substr(body_start, close - body_start);
        if (field_present(state, field)) {
            templ.replace(open, close + close_token.size() - open, body);
            search = open + body.size();
        } else {
            templ.erase(open, close + close_token.size() - open);
            search = open;
        }
    }
}

std::string render_template(std::string templ, const JsonState& state) {
    render_optional_blocks(templ, state);
    replace_all(templ, "{text}", state.text);
    replace_all(templ, "{device}", state.device);
    replace_all(templ, "{battery}", charge_value(state.battery));
    replace_all(templ, "{left}", charge_value(state.left));
    replace_all(templ, "{right}", charge_value(state.right));
    replace_all(templ, "{case}", charge_value(state.case_level));
    replace_all(templ, "{status}", state.status);
    replace_all(templ, "{class}", state.klass);
    replace_all(templ, "{alt}", state.alt);
    return templ;
}

std::string json_escape(const std::string& value) {
    std::string escaped;
    for (char ch : value) {
        switch (ch) {
        case '\\':
            escaped += "\\\\";
            break;
        case '"':
            escaped += "\\\"";
            break;
        case '\n':
            escaped += "\\n";
            break;
        default:
            escaped += ch;
            break;
        }
    }
    return escaped;
}

const std::string& state_format(const Options& options, const JsonState& state) {
    if (state.status == "connected" && !options.connected_format.empty()) return options.connected_format;
    if (state.status == "connecting" && !options.connecting_format.empty()) return options.connecting_format;
    if (state.status == "disconnected" && !options.disconnected_format.empty()) return options.disconnected_format;
    return options.format;
}

std::string client_json(const std::string& service_json, const Options& options) {
    auto state = parse_state_json(service_json);
    if (!state) return service_json;

    std::ostringstream out;
    out
        << "{\"text\":\"" << json_escape(render_template(state_format(options, *state), *state)) << "\","
        << "\"class\":\"" << state->klass << "\","
        << "\"alt\":\"" << state->status << "\","
        << "\"tooltip\":\"" << json_escape(state->device + " " + title_status(state->status)) << "\"}";
    return out.str();
}

std::string bluez_device_path_for(const std::string& mac) {
    std::string path = "/org/bluez/hci0/dev_";
    for (char ch : mac) path += ch == ':' ? '_' : ch;
    return path;
}

std::optional<std::string> bluez_device_string_property(const std::string& mac, const char* property) {
    GErrorHolder err;
    GDBusConnection* bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, nullptr, &err.error);
    if (!bus) return std::nullopt;

    std::string device_path = bluez_device_path_for(mac);
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

    g_object_unref(bus);
    if (!result) return std::nullopt;

    GVariant* value = nullptr;
    g_variant_get(result, "(v)", &value);
    std::string text;
    if (g_variant_is_of_type(value, G_VARIANT_TYPE_STRING)) {
        const char* raw = g_variant_get_string(value, nullptr);
        if (raw && *raw) text = raw;
    }
    g_variant_unref(value);
    g_variant_unref(result);

    if (text.empty()) return std::nullopt;
    return text;
}

std::string bluez_device_label(const std::string& mac, const std::string& fallback) {
    if (auto alias = bluez_device_string_property(mac, "Alias")) return *alias;
    if (auto name = bluez_device_string_property(mac, "Name")) return *name;
    return fallback;
}

class Service {
public:
    explicit Service(std::string path) : path_(std::move(path)) {}

    bool start() {
        if (service_socket_exists()) {
            std::cerr << "waybar-earbud: service is already running\n";
            return false;
        }

        server_fd_ = Fd(socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
        if (!server_fd_) {
            std::cerr << "waybar-earbud: failed to create service socket: " << std::strerror(errno) << "\n";
            return false;
        }

        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        if (path_.size() >= sizeof(addr.sun_path)) {
            std::cerr << "waybar-earbud: service socket path is too long\n";
            return false;
        }
        std::strncpy(addr.sun_path, path_.c_str(), sizeof(addr.sun_path) - 1);

        unlink(path_.c_str());
        if (bind(server_fd_.get(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            std::cerr << "waybar-earbud: failed to bind service socket: " << std::strerror(errno) << "\n";
            return false;
        }

        if (listen(server_fd_.get(), 16) != 0) {
            std::cerr << "waybar-earbud: failed to listen on service socket: " << std::strerror(errno) << "\n";
            return false;
        }

        running_ = true;
        server_thread_ = std::thread(&Service::accept_loop, this);
        return true;
    }

    ~Service() {
        running_ = false;
        if (server_fd_) server_fd_.reset();
        if (server_thread_.joinable()) server_thread_.join();
        unlink(path_.c_str());
    }

    void emit(const std::string& json) {
        std::lock_guard lock(mutex_);
        current_json_ = json;
        for (auto it = watchers_.begin(); it != watchers_.end();) {
            if (write_line(it->get(), json)) {
                ++it;
            } else {
                it = watchers_.erase(it);
            }
        }
    }

private:
    std::string path_;
    Fd server_fd_;
    std::thread server_thread_;
    std::mutex mutex_;
    std::vector<Fd> watchers_;
    std::string current_json_ = disconnected_json("Earbuds");
    bool running_ = false;

    bool service_socket_exists() {
        Fd fd(socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0));
        if (!fd) return false;

        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        if (path_.size() >= sizeof(addr.sun_path)) return false;
        std::strncpy(addr.sun_path, path_.c_str(), sizeof(addr.sun_path) - 1);
        return connect(fd.get(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
    }

    void accept_loop() {
        while (running_) {
            pollfd pfd{.fd = server_fd_.get(), .events = POLLIN, .revents = 0};
            int rc = poll(&pfd, 1, 500);
            if (rc < 0) {
                if (errno == EINTR) continue;
                return;
            }
            if (rc == 0 || (pfd.revents & POLLIN) == 0) continue;

            while (true) {
                Fd client(accept4(server_fd_.get(), nullptr, nullptr, SOCK_CLOEXEC));
                if (!client) {
                    if (errno == EINTR) continue;
                    if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                    return;
                }
                handle_client(std::move(client));
            }
        }
    }

    void handle_client(Fd client) {
        auto command = read_line(client.get(), 1000);
        if (!command) return;

        if (*command == "TOGGLE") {
            invoke_toggle_handler();
            return;
        }

        std::lock_guard lock(mutex_);
        write_line(client.get(), current_json_);
        if (*command == "WATCH") {
            watchers_.push_back(std::move(client));
        }
    }
};

Service* active_service = nullptr;
char** service_argv = nullptr;

void service_emit_json(const std::string& json) {
    if (active_service) active_service->emit(json);
}

[[noreturn]] void rediscover_exec() {
    if (service_argv) {
        execv("/proc/self/exe", service_argv);
    }

    _exit(127);
}

void start_default_output_monitor(std::optional<std::string> current_mac) {
    std::thread([current_mac = std::move(current_mac)] {
        while (true) {
            std::this_thread::sleep_for(std::chrono::seconds(2));
            auto next_mac = audio::default_bluetooth_mac();
            if (!next_mac) continue;
            if (next_mac == current_mac) continue;
            rediscover_exec();
        }
    }).detach();
}

void on_sony_properties_changed(GDBusConnection*,
                                const char*,
                                const char*,
                                const char*,
                                const char*,
                                GVariant* parameters,
                                gpointer user_data) {
    auto* state = static_cast<SonyWatchState*>(user_data);

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

    auto now = std::chrono::steady_clock::now();
    std::lock_guard lock(state->mutex);
    if (is_connected == state->connected) return;

    state->connected = is_connected;
    state->connecting = false;
    ++state->generation;
    if (!is_connected) state->reading = false;
    state->next_refresh = is_connected ? now : now + std::chrono::seconds(state->interval);

    if (!is_connected) {
        print_disconnected(state->label);
        return;
    }

    print_json(battery_json(state->label, Battery{}));
}

void start_sony_battery_read(const std::string& mac, SonyWatchState& state) {
    unsigned int generation = 0;
    {
        std::lock_guard lock(state.mutex);
        if (!state.connected || state.reading) return;
        state.reading = true;
        generation = state.generation;
    }

    std::thread([mac, &state, generation] {
        std::optional<Battery> battery;
        try {
            battery = devices::sony::read_battery(mac);
        } catch (const std::exception& ex) {
            std::cerr << "sony: " << ex.what() << "\n";
        }

        bool still_connected = battery.has_value() || devices::sony::connected(mac);
        auto now = std::chrono::steady_clock::now();

        std::lock_guard lock(state.mutex);
        state.reading = false;

        if (!state.connected || state.generation != generation) return;

        if (battery) {
            state.next_refresh = now + std::chrono::seconds(state.interval);
            print_battery(state.label, *battery);
            return;
        }

        if (still_connected) {
            state.next_refresh = now + std::chrono::seconds(kSonyBatteryRetrySeconds);
            return;
        }

        state.connected = false;
        state.connecting = false;
        ++state.generation;
        state.next_refresh = now + std::chrono::seconds(state.interval);
        print_disconnected(state.label);
    }).detach();
}

void finish_sony_connect_attempt(const std::string& mac, SonyWatchState& state, unsigned int generation) {
    bool connected = devices::sony::connected(mac);
    auto now = std::chrono::steady_clock::now();

    std::lock_guard lock(state.mutex);
    if (state.generation != generation) return;

    state.connecting = false;
    state.connected = connected;
    state.next_refresh = connected ? now : now + std::chrono::seconds(state.interval);

    if (connected) {
        print_json(battery_json(state.label, Battery{}));
    } else {
        print_disconnected(state.label);
    }
}

bool call_bluez_device_method(const std::string& mac, const char* method) {
    GErrorHolder err;
    GDBusConnection* bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, nullptr, &err.error);
    if (!bus) {
        if (err.error) std::cerr << "sony: failed to open system bus: " << err.error->message << "\n";
        return false;
    }

    std::string device_path = bluez_device_path_for(mac);
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
    g_object_unref(bus);

    if (!result) {
        if (err.error) std::cerr << "sony: " << method << ": " << err.error->message << "\n";
        return false;
    }

    return true;
}

int run_sony_service(const std::string& mac, int interval) {
    if (interval < 5) interval = 5;

    int pipe_fds[2] = {-1, -1};
    if (pipe2(pipe_fds, O_NONBLOCK | O_CLOEXEC) != 0) {
        std::cerr << "sony: failed to set up toggle pipe\n";
        return 1;
    }

    Fd read_fd(pipe_fds[0]);
    Fd write_fd(pipe_fds[1]);
    sony_toggle_write_fd = write_fd.get();
    set_toggle_handler([] {
        if (sony_toggle_write_fd < 0) return;

        const uint8_t byte = 1;
        ssize_t written = write(sony_toggle_write_fd, &byte, sizeof(byte));
        (void)written;
    });

    SonyWatchState state;
    state.label = bluez_device_label(mac, "Sony earbuds");
    state.connected = devices::sony::connected(mac);
    state.interval = interval;
    state.next_refresh = std::chrono::steady_clock::now();

    GErrorHolder err;
    GDBusConnection* bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, nullptr, &err.error);
    if (!bus) {
        if (err.error) std::cerr << "sony: failed to open system bus: " << err.error->message << "\n";
        return 1;
    }

    std::string device_path = bluez_device_path_for(mac);
    g_dbus_connection_signal_subscribe(
        bus,
        "org.bluez",
        "org.freedesktop.DBus.Properties",
        "PropertiesChanged",
        device_path.c_str(),
        nullptr,
        G_DBUS_SIGNAL_FLAGS_NONE,
        on_sony_properties_changed,
        &state,
        nullptr);

    if (!state.connected) {
        print_disconnected(state.label);
    } else {
        print_json(battery_json(state.label, Battery{}));
    }

    auto wait_for_toggle = [&](std::chrono::milliseconds timeout) {
        pollfd pfd{.fd = read_fd.get(), .events = POLLIN, .revents = 0};
        while (true) {
            int rc = poll(&pfd, 1, static_cast<int>(timeout.count()));
            if (rc < 0 && errno == EINTR) continue;
            if (rc <= 0 || (pfd.revents & POLLIN) == 0) return false;

            uint8_t buffer[64] = {};
            while (read(read_fd.get(), buffer, sizeof(buffer)) > 0) {
            }
            return true;
        }
    };

    while (true) {
        while (g_main_context_iteration(nullptr, FALSE)) {
        }

        auto now = std::chrono::steady_clock::now();
        bool read_battery = false;
        {
            std::lock_guard lock(state.mutex);
            if (state.connected && !state.reading && now >= state.next_refresh) {
                state.next_refresh = now + std::chrono::seconds(interval);
                read_battery = true;
            }
        }
        if (read_battery) start_sony_battery_read(mac, state);

        if (wait_for_toggle(std::chrono::milliseconds(100))) {
            bool connected = false;
            bool connecting = false;
            {
                std::lock_guard lock(state.mutex);
                connected = state.connected;
                connecting = state.connecting;
            }

            if (connected || connecting) {
                {
                    std::lock_guard lock(state.mutex);
                    state.connected = false;
                    state.connecting = false;
                    state.reading = false;
                    ++state.generation;
                    print_disconnected(state.label);
                }
                std::thread([mac] { call_bluez_device_method(mac, "Disconnect"); }).detach();
            } else {
                unsigned int generation = 0;
                {
                    std::lock_guard lock(state.mutex);
                    state.connecting = true;
                    ++state.generation;
                    generation = state.generation;
                    print_json(status_json(state.label, "connecting", "connecting"));
                }
                std::thread([mac, &state, generation] {
                    call_bluez_device_method(mac, "Connect");
                    finish_sony_connect_attempt(mac, state, generation);
                }).detach();
            }
        }
    }
}

std::optional<DeviceKind> auto_detect(const std::string& mac) {
    if (devices::airpods::available(mac)) return DeviceKind::AirPods;
    if (devices::sony::available(mac)) return DeviceKind::Sony;
    return std::nullopt;
}

int run_provider_service(DeviceKind selected, const std::string& mac, int interval) {
    switch (selected) {
    case DeviceKind::Sony:
        return run_sony_service(mac, interval);
    case DeviceKind::AirPods:
        return devices::airpods::run(mac, true);
    case DeviceKind::Auto:
        break;
    }

    return 1;
}

Fd connect_service_socket() {
    Fd fd(socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0));
    if (!fd) return {};

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::string path = socket_path();
    if (path.size() >= sizeof(addr.sun_path)) return {};
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);

    if (connect(fd.get(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) return {};
    return fd;
}

int run_client_once(const Options& options) {
    Fd fd = connect_service_socket();
    if (!fd) {
        print_json(client_json(disconnected_json("Earbuds"), options));
        return 0;
    }

    write_all(fd.get(), "GET\n");
    auto line = read_line(fd.get());
    if (line) {
        print_json(client_json(*line, options));
    } else {
        print_json(client_json(disconnected_json("Earbuds"), options));
    }
    return 0;
}

int run_client_watch(const Options& options) {
    std::string last;

    while (true) {
        Fd fd = connect_service_socket();
        if (!fd) {
            std::string disconnected = client_json(disconnected_json("Earbuds"), options);
            if (last != disconnected) {
                print_json(disconnected);
                last = disconnected;
            }
            sleep(1);
            continue;
        }

        write_all(fd.get(), "WATCH\n");
        while (auto line = read_line(fd.get())) {
            std::string rendered = client_json(*line, options);
            print_json(rendered);
            last = rendered;
        }

        sleep(1);
    }
}

int run_client_toggle() {
    Fd fd = connect_service_socket();
    if (!fd) return 1;

    write_all(fd.get(), "TOGGLE\n");
    return 0;
}

int run_service(const Options& options, char** argv) {
    int interval = options.interval > 0 ? options.interval : 30;
    service_argv = argv;

    Service service(socket_path());
    if (!service.start()) return 1;
    active_service = &service;
    set_json_sink(service_emit_json);

    std::optional<std::string> mac = options.mac;
    if (!mac) mac = audio::default_bluetooth_mac();
    if (!options.explicit_mac) start_default_output_monitor(mac);

    if (!mac) {
        print_disconnected("Earbuds");
        while (true) pause();
    }

    DeviceKind selected = options.device;
    if (selected == DeviceKind::Auto) {
        auto detected = auto_detect(*mac);
        if (!detected) {
            std::cerr << "waybar-earbud: could not auto-detect supported device type\n";
            print_disconnected("Earbuds");
            while (true) pause();
        }
        selected = *detected;
    }

    return run_provider_service(selected, *mac, interval);
}

std::optional<Options> parse_args(int argc, char** argv) {
    Options options;
    int arg = 1;

    while (arg < argc) {
        std::string_view current = argv[arg];
        if (current == "--help" || current == "-h") {
            print_usage(std::cout);
            std::exit(0);
        }

        if (current == "--version" || current == "-v") {
            print_version();
            std::exit(0);
        }

        if (current == "--service") {
            options.service = true;
            ++arg;
            continue;
        }

        if (current == "--watch") {
            options.watch = true;
            ++arg;
            continue;
        }

        if (current == "--toggle") {
            options.toggle = true;
            ++arg;
            continue;
        }

        if (current == "--preset") {
            if (arg + 1 >= argc || !apply_preset(options, argv[arg + 1])) {
                std::cerr << "waybar-earbud: invalid value for --preset\n";
                print_usage(std::cerr);
                return std::nullopt;
            }

            arg += 2;
            continue;
        }

        if (current == "--format") {
            if (arg + 1 >= argc) {
                std::cerr << "waybar-earbud: missing value for --format\n";
                print_usage(std::cerr);
                return std::nullopt;
            }

            options.format = argv[arg + 1];
            arg += 2;
            continue;
        }

        if (current == "--connected-format") {
            if (arg + 1 >= argc) {
                std::cerr << "waybar-earbud: missing value for --connected-format\n";
                print_usage(std::cerr);
                return std::nullopt;
            }

            options.connected_format = argv[arg + 1];
            arg += 2;
            continue;
        }

        if (current == "--connecting-format") {
            if (arg + 1 >= argc) {
                std::cerr << "waybar-earbud: missing value for --connecting-format\n";
                print_usage(std::cerr);
                return std::nullopt;
            }

            options.connecting_format = argv[arg + 1];
            arg += 2;
            continue;
        }

        if (current == "--disconnected-format") {
            if (arg + 1 >= argc) {
                std::cerr << "waybar-earbud: missing value for --disconnected-format\n";
                print_usage(std::cerr);
                return std::nullopt;
            }

            options.disconnected_format = argv[arg + 1];
            arg += 2;
            continue;
        }

        if (current == "--mac") {
            if (arg + 1 >= argc || std::string_view(argv[arg + 1]).starts_with("-")) {
                std::cerr << "waybar-earbud: missing value for --mac\n";
                print_usage(std::cerr);
                return std::nullopt;
            }

            auto parsed_mac = parse_mac(argv[arg + 1]);
            if (!parsed_mac) {
                std::cerr << "waybar-earbud: invalid value for --mac\n";
                print_usage(std::cerr);
                return std::nullopt;
            }

            options.mac = *parsed_mac;
            options.explicit_mac = true;
            arg += 2;
            continue;
        }

        if (current == "--interval") {
            if (arg + 1 >= argc || !parse_interval(argv[arg + 1], options.interval)) {
                std::cerr << "waybar-earbud: invalid value for --interval\n";
                print_usage(std::cerr);
                return std::nullopt;
            }

            arg += 2;
            continue;
        }

        if (current == "--device") {
            if (arg + 1 >= argc) {
                print_usage(std::cerr);
                return std::nullopt;
            }

            auto parsed = parse_device_kind(argv[arg + 1]);
            if (!parsed) {
                print_usage(std::cerr);
                return std::nullopt;
            }

            options.device = *parsed;
            arg += 2;
            continue;
        }

        if (current.starts_with("-")) {
            std::cerr << "waybar-earbud: unknown option: " << current << "\n";
        } else {
            std::cerr << "waybar-earbud: unexpected argument: " << current << "\n";
        }
        print_usage(std::cerr);
        return std::nullopt;
    }

    return options;
}

} // namespace

int main(int argc, char** argv) {
    auto options = parse_args(argc, argv);
    if (!options) return 2;

    if (options->service) return run_service(*options, argv);
    if (options->toggle) return run_client_toggle();
    if (options->watch) return run_client_watch(*options);
    return run_client_once(*options);
}
