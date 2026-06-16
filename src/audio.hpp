#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace audio {

std::optional<std::string> mac_from_bluez_token(std::string_view token);
std::optional<std::string> mac_from_pactl_sinks(std::string_view default_sink, std::string_view sinks);
std::optional<std::string> default_bluetooth_mac();

} // namespace audio
