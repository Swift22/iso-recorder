#pragma once

#include <string>
#include <vector>

#include "iso-session.hpp"

namespace iso {

// Everything here lives in the plugin's own settings.json, not in an OBS profile: a
// profile switch after startup would otherwise leave the dock holding another profile's
// ticks. The first load copies what older versions kept in every profile's basic.ini.
SessionConfig loadConfig();
void saveConfig(const SessionConfig &cfg);

// The sources ticked in the dock, remembered per scene collection so a fresh
// start comes up with the same set. A name that no longer exists is skipped.
std::vector<std::string> loadArmedSources(const std::string &collection);
void saveArmedSources(const std::string &collection, const std::vector<std::string> &names);
// The sources set to record at half frame rate, remembered the same way.
std::vector<std::string> loadHalfRateSources(const std::string &collection);
void saveHalfRateSources(const std::string &collection, const std::vector<std::string> &names);

// Writes the save* calls above to disk.
bool writeSettings(std::string *error);
void releaseSettings();

} // namespace iso
