#pragma once

#include <string>
#include <vector>

#include "iso-session.hpp"

namespace iso {

SessionConfig loadConfig();
void saveConfig(const SessionConfig &cfg);

// The sources ticked in the dock, remembered per scene collection so a fresh
// start comes up with the same set. A name that no longer exists is skipped.
std::vector<std::string> loadArmedSources(const std::string &collection);
void saveArmedSources(const std::string &collection, const std::vector<std::string> &names);

} // namespace iso
