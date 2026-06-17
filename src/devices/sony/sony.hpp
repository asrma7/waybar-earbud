#pragma once

#include <string>

#include "../../common.hpp"

namespace devices::sony {

bool available(const std::string& mac);
bool connected(const std::string& mac);
Battery read_battery(const std::string& mac);

} // namespace devices::sony
