#pragma once

#include <string>

namespace devices::airpods {

bool available(const std::string& mac);
int run(const std::string& mac, bool watch);

} // namespace devices::airpods
