#pragma once

#include "iso-session.hpp"

namespace iso {

SessionConfig loadConfig();
void saveConfig(const SessionConfig &cfg);

} // namespace iso
