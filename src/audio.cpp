#include "audio.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <cstdio>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

namespace {

std::string run_command(const char* command) {
    std::array<char, 256> buffer{};
    std::string output;

    FILE* pipe = popen(command, "r");
    if (!pipe) return {};

    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe)) {
        output += buffer.data();
    }

    pclose(pipe);
    return output;
}

std::string trim(std::string value) {
    auto is_space = [](unsigned char ch) { return std::isspace(ch) != 0; };
    value.erase(value.begin(), std::find_if_not(value.begin(), value.end(), is_space));
    value.erase(std::find_if_not(value.rbegin(), value.rend(), is_space).base(), value.end());
    return value;
}

bool is_hex_pair(std::string_view value) {
    return value.size() == 2 &&
        std::isxdigit(static_cast<unsigned char>(value[0])) &&
        std::isxdigit(static_cast<unsigned char>(value[1]));
}

} // namespace

namespace audio {

std::optional<std::string> mac_from_bluez_token(std::string_view token) {
    for (size_t i = 0; i + 17 <= token.size(); ++i) {
        std::array<std::string_view, 6> parts{
            token.substr(i, 2),
            token.substr(i + 3, 2),
            token.substr(i + 6, 2),
            token.substr(i + 9, 2),
            token.substr(i + 12, 2),
            token.substr(i + 15, 2),
        };

        if (token[i + 2] != '_' || token[i + 5] != '_' || token[i + 8] != '_' ||
            token[i + 11] != '_' || token[i + 14] != '_') {
            continue;
        }

        if (!std::all_of(parts.begin(), parts.end(), is_hex_pair)) continue;

        std::string mac;
        for (size_t part = 0; part < parts.size(); ++part) {
            if (part != 0) mac += ':';
            mac += std::string(parts[part]);
        }
        return mac;
    }

    return std::nullopt;
}

std::optional<std::string> mac_from_pactl_sinks(std::string_view default_sink, std::string_view sinks) {
    if (default_sink.empty()) return std::nullopt;

    std::istringstream stream{std::string(sinks)};
    std::string line;
    bool in_default_sink = false;

    while (std::getline(stream, line)) {
        std::string stripped = trim(line);
        if (stripped.rfind("Sink #", 0) == 0) {
            in_default_sink = false;
            continue;
        }

        if (stripped == "Name: " + std::string(default_sink)) {
            in_default_sink = true;
            if (auto mac = mac_from_bluez_token(default_sink)) return mac;
            continue;
        }

        if (!in_default_sink) continue;

        if (auto mac = mac_from_bluez_token(stripped)) return mac;
    }

    return std::nullopt;
}

} // namespace audio

namespace {

std::optional<std::string> mac_from_default_sink_name() {
    std::string sink = trim(run_command("pactl get-default-sink 2>/dev/null"));
    if (sink.empty()) return std::nullopt;
    return audio::mac_from_bluez_token(sink);
}

std::optional<std::string> mac_from_sink_list() {
    std::string default_sink = trim(run_command("pactl get-default-sink 2>/dev/null"));
    if (default_sink.empty()) return std::nullopt;

    std::string sinks = run_command("pactl list sinks 2>/dev/null");
    return audio::mac_from_pactl_sinks(default_sink, sinks);
}

} // namespace

namespace audio {

std::optional<std::string> default_bluetooth_mac() {
    if (const char* disabled = std::getenv("EARBUD_BATTERY_DISABLE_AUDIO_DISCOVERY")) {
        if (std::string_view(disabled) == "1") return std::nullopt;
    }

    if (auto mac = mac_from_default_sink_name()) return mac;
    return mac_from_sink_list();
}

} // namespace audio
