#include <charconv>
#include <cctype>
#include <cerrno>
#include <csignal>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

#include <fcntl.h>
#include <gio/gio.h>
#include <poll.h>
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

int sony_signal_write_fd = -1;
const char* rediscover_exec_path = nullptr;
char** rediscover_exec_argv = nullptr;

void on_rediscover_signal(int) {
    if (rediscover_exec_path && rediscover_exec_argv) {
        execv(rediscover_exec_path, rediscover_exec_argv);
    }

    _exit(127);
}

void on_sony_update_signal(int) {
    if (sony_signal_write_fd < 0) return;

    const uint8_t byte = 1;
    ssize_t written = write(sony_signal_write_fd, &byte, sizeof(byte));
    (void)written;
}

struct GErrorHolder {
    ~GErrorHolder() {
        if (error) g_error_free(error);
    }

    GError* error = nullptr;
};

std::string bluez_device_path_for(const std::string& mac) {
    std::string path = "/org/bluez/hci0/dev_";
    for (char ch : mac) path += ch == ':' ? '_' : ch;
    return path;
}

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

void print_usage(std::ostream& out) {
    out
        << "Usage: waybar-earbud [options]\n"
        << "\n"
        << "Waybar helper for Bluetooth earbud battery status.\n"
        << "\n"
        << "By default, the tool follows the current Bluetooth audio output, auto-detects\n"
        << "the supported provider, prints one Waybar JSON object, and exits. If no\n"
        << "supported/current Bluetooth device is found, it prints a disconnected JSON\n"
        << "object and exits successfully so Waybar can hide the module with CSS.\n"
        << "\n"
        << "Options:\n"
        << "  --device auto|sony|airpods  Provider to use. Defaults to auto.\n"
        << "  --mac AA:BB:CC:DD:EE:FF     Use this Bluetooth MAC instead of the current audio output.\n"
        << "  --watch                     Keep running instead of printing once.\n"
        << "                              SIGUSR1 toggles connection; SIGUSR2 rediscovers current output when no --mac is set.\n"
        << "  --interval seconds          Sony watch refresh interval. Defaults to 30, minimum 5.\n"
        << "  -v, --version               Print version and exit.\n"
        << "  -h, --help                  Show this help and exit.\n";
}

void print_version() {
    std::cout << "waybar-earbud " << WAYBAR_EARBUD_VERSION << "\n";
}

void configure_watch_rediscovery(bool explicit_mac, char** argv) {
    struct sigaction action {};
    sigemptyset(&action.sa_mask);

    if (explicit_mac) {
        action.sa_handler = SIG_IGN;
    } else {
        rediscover_exec_path = argv[0];
        rediscover_exec_argv = argv;
        action.sa_handler = on_rediscover_signal;
    }

    sigaction(SIGUSR2, &action, nullptr);
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

int run_sony_once(const std::string& mac) {
    try {
        print_battery("Sony earbuds", devices::sony::read_battery(mac));
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "sony: " << ex.what() << "\n";
        print_disconnected("Sony earbuds");
        return 1;
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

int run_sony(const std::string& mac, int interval) {
    if (interval <= 0) return run_sony_once(mac);
    if (interval < 5) interval = 5;

    int pipe_fds[2] = {-1, -1};
    if (pipe2(pipe_fds, O_NONBLOCK | O_CLOEXEC) != 0) {
        std::cerr << "sony: failed to set up SIGUSR1 wake pipe\n";
        return 1;
    }

    Fd read_fd(pipe_fds[0]);
    Fd write_fd(pipe_fds[1]);
    sony_signal_write_fd = write_fd.get();

    struct sigaction action {};
    action.sa_handler = on_sony_update_signal;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_RESTART;
    sigaction(SIGUSR1, &action, nullptr);

    auto wait_for_signal = [&] {
        pollfd pfd{.fd = read_fd.get(), .events = POLLIN, .revents = 0};
        while (true) {
            int rc = poll(&pfd, 1, interval * 1000);
            if (rc < 0 && errno == EINTR) continue;
            if (rc <= 0 || (pfd.revents & POLLIN) == 0) return false;

            uint8_t buffer[64] = {};
            while (read(read_fd.get(), buffer, sizeof(buffer)) > 0) {
            }
            return true;
        }
    };

    bool manually_disconnected = false;
    while (true) {
        if (!manually_disconnected) run_sony_once(mac);

        if (!wait_for_signal()) continue;

        if (manually_disconnected) {
            call_bluez_device_method(mac, "Connect");
            manually_disconnected = false;
        } else {
            call_bluez_device_method(mac, "Disconnect");
            manually_disconnected = true;
            print_disconnected("Sony earbuds");
        }
    }
}

std::optional<DeviceKind> auto_detect(const std::string& mac) {
    if (devices::airpods::available(mac)) return DeviceKind::AirPods;
    if (devices::sony::available(mac)) return DeviceKind::Sony;
    return std::nullopt;
}

} // namespace

int main(int argc, char** argv) {
    DeviceKind requested = DeviceKind::Auto;
    bool watch = false;
    bool explicit_mac = false;
    std::optional<std::string> mac;
    int interval = 0;
    int arg = 1;

    while (arg < argc) {
        std::string_view current = argv[arg];
        if (current == "--help" || current == "-h") {
            print_usage(std::cout);
            return 0;
        }

        if (current == "--version" || current == "-v") {
            print_version();
            return 0;
        }

        if (current == "--watch") {
            watch = true;
            ++arg;
            continue;
        }

        if (current == "--mac") {
            if (arg + 1 >= argc || std::string_view(argv[arg + 1]).starts_with("-")) {
                std::cerr << "waybar-earbud: missing value for --mac\n";
                print_usage(std::cerr);
                return 2;
            }

            auto parsed_mac = parse_mac(argv[arg + 1]);
            if (!parsed_mac) {
                std::cerr << "waybar-earbud: invalid value for --mac\n";
                print_usage(std::cerr);
                return 2;
            }

            mac = *parsed_mac;
            explicit_mac = true;
            arg += 2;
            continue;
        }

        if (current == "--interval") {
            if (arg + 1 >= argc || !parse_interval(argv[arg + 1], interval)) {
                std::cerr << "waybar-earbud: invalid value for --interval\n";
                print_usage(std::cerr);
                return 2;
            }

            arg += 2;
            continue;
        }

        if (current == "--device") {
            if (arg + 1 >= argc) {
                print_usage(std::cerr);
                return 2;
            }

            auto parsed = parse_device_kind(argv[arg + 1]);
            if (!parsed) {
                print_usage(std::cerr);
                return 2;
            }

            requested = *parsed;
            arg += 2;
            continue;
        }

        if (current.starts_with("-")) {
            std::cerr << "waybar-earbud: unknown option: " << current << "\n";
        } else {
            std::cerr << "waybar-earbud: unexpected argument: " << current << "\n";
        }
        print_usage(std::cerr);
        return 2;
    }

    if (watch && interval <= 0) interval = 30;
    if (watch) configure_watch_rediscovery(explicit_mac, argv);

    if (!mac) mac = audio::default_bluetooth_mac();

    if (!mac) {
        print_disconnected("Earbuds");
        return 0;
    }

    DeviceKind selected = requested;
    if (selected == DeviceKind::Auto) {
        auto detected = auto_detect(*mac);
        if (!detected) {
            std::cerr << "waybar-earbud: could not auto-detect supported device type\n";
            print_disconnected("Earbuds");
            return 0;
        }
        selected = *detected;
    }

    switch (selected) {
    case DeviceKind::Sony:
        return run_sony(*mac, interval);
    case DeviceKind::AirPods:
        return devices::airpods::run(*mac, watch);
    case DeviceKind::Auto:
        break;
    }

    return 1;
}
