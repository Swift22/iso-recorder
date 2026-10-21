#include "iso-settings.hpp"

#include <obs.h>
#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/config-file.h>
#include <util/platform.h>

namespace iso {

static const char *legacySection = "iso-recorder";
static const char *fileName = "settings.json";
static const char *stringKeys[] = {"basePath", "videoEncoderId", "videoSize"};
static const char *boolKeys[] = {"withStream", "recordComposite", "withRecording"};
static const char *setKeys[] = {"armedSources", "halfRateSources"};

static obs_data_t *g_store = nullptr;

static std::string settingsPath()
{
	char *path = obs_module_config_path(fileName);
	std::string out = path ? path : "";
	bfree(path);
	return out;
}

// Copies one profile's [iso-recorder] section into the store. Profiles are read current
// one first, so a value or a collection's set another profile already gave is kept.
static void migrateProfile(config_t *config, bool *haveScalars)
{
	if (!config_has_user_value(config, legacySection, "withStream") &&
	    !config_has_user_value(config, legacySection, "armedSources")) {
		return;
	}
	if (!*haveScalars && config_has_user_value(config, legacySection, "withStream")) {
		for (const char *key : stringKeys) {
			const char *value = config_get_string(config, legacySection, key);
			if (value) {
				obs_data_set_string(g_store, key, value);
			}
		}
		for (const char *key : boolKeys) {
			if (config_has_user_value(config, legacySection, key)) {
				obs_data_set_bool(g_store, key, config_get_bool(config, legacySection, key));
			}
		}
		*haveScalars = true;
	}
	for (const char *key : setKeys) {
		const char *json = config_get_string(config, legacySection, key);
		obs_data_t *legacy = json ? obs_data_create_from_json(json) : nullptr;
		if (!legacy) {
			continue;
		}
		obs_data_t *table = obs_data_get_obj(g_store, key);
		if (!table) {
			table = obs_data_create();
			obs_data_set_obj(g_store, key, table);
		}
		for (obs_data_item_t *item = obs_data_first(legacy); item; obs_data_item_next(&item)) {
			const char *collection = obs_data_item_get_name(item);
			obs_data_array_t *list = obs_data_item_get_array(item);
			if (list && obs_data_array_count(list) > 0 && !obs_data_has_user_value(table, collection)) {
				obs_data_set_array(table, collection, list);
			}
			if (list) {
				obs_data_array_release(list);
			}
		}
		obs_data_release(table);
		obs_data_release(legacy);
	}
}

static void migrateFromProfiles()
{
	bool haveScalars = false;
	config_t *current = obs_frontend_get_profile_config();
	if (current) {
		migrateProfile(current, &haveScalars);
	}

	char *currentPath = obs_frontend_get_current_profile_path();
	std::string currentDir = currentPath ? currentPath : "";
	bfree(currentPath);
	while (!currentDir.empty() && (currentDir.back() == '/' || currentDir.back() == '\\')) {
		currentDir.pop_back();
	}
	const size_t slash = currentDir.find_last_of("/\\");
	if (slash == std::string::npos) {
		return;
	}
	const std::string profilesDir = currentDir.substr(0, slash);
	const std::string currentName = currentDir.substr(slash + 1);
	os_dir_t *dir = os_opendir(profilesDir.c_str());
	if (!dir) {
		return;
	}
	for (os_dirent *entry = os_readdir(dir); entry; entry = os_readdir(dir)) {
		const std::string name = entry->d_name;
		if (!entry->directory || name == "." || name == ".." || name == currentName) {
			continue;
		}
		const std::string ini = profilesDir + "/" + name + "/basic.ini";
		config_t *config = nullptr;
		if (config_open(&config, ini.c_str(), CONFIG_OPEN_EXISTING) == CONFIG_SUCCESS) {
			migrateProfile(config, &haveScalars);
			config_close(config);
		}
	}
	os_closedir(dir);
}

static obs_data_t *store()
{
	if (g_store) {
		return g_store;
	}
	const std::string path = settingsPath();
	if (!path.empty() && os_file_exists(path.c_str())) {
		g_store = obs_data_create_from_json_file_safe(path.c_str(), "bak");
		if (!g_store) {
			blog(LOG_WARNING, "[iso-recorder] could not read %s; starting from the OBS profiles' settings",
			     path.c_str());
		}
	}
	if (!g_store) {
		g_store = obs_data_create();
		migrateFromProfiles();
		std::string err;
		if (!writeSettings(&err)) {
			blog(LOG_WARNING, "[iso-recorder] %s", err.c_str());
		} else {
			blog(LOG_INFO, "[iso-recorder] settings moved from the OBS profiles to %s", path.c_str());
		}
	}
	return g_store;
}

bool writeSettings(std::string *error)
{
	char *dir = obs_module_config_path("");
	const bool madeDir = dir && os_mkdirs(dir) != MKDIR_ERROR;
	bfree(dir);
	const std::string path = settingsPath();
	if (!madeDir || path.empty() || !obs_data_save_json_safe(store(), path.c_str(), "tmp", "bak")) {
		if (error) {
			*error = "could not save the settings to " + path;
		}
		return false;
	}
	return true;
}

void releaseSettings()
{
	obs_data_release(g_store);
	g_store = nullptr;
}

SessionConfig loadConfig()
{
	SessionConfig cfg;
	obs_data_t *data = store();
	if (obs_data_has_user_value(data, "basePath")) {
		cfg.basePath = obs_data_get_string(data, "basePath");
	}
	if (obs_data_has_user_value(data, "videoEncoderId")) {
		cfg.videoEncoderId = obs_data_get_string(data, "videoEncoderId");
	}
	const char *size = obs_data_get_string(data, "videoSize");
	if (size && *size)
		cfg.videoSize = size;
	if (obs_data_has_user_value(data, "withStream")) {
		cfg.withStream = obs_data_get_bool(data, "withStream");
	}
	if (obs_data_has_user_value(data, "recordComposite")) {
		cfg.recordComposite = obs_data_get_bool(data, "recordComposite");
	}
	if (obs_data_has_user_value(data, "withRecording")) {
		cfg.withRecording = obs_data_get_bool(data, "withRecording");
	}
	return cfg;
}

void saveConfig(const SessionConfig &cfg)
{
	obs_data_t *data = store();
	obs_data_set_string(data, "basePath", cfg.basePath.c_str());
	obs_data_set_string(data, "videoEncoderId", cfg.videoEncoderId.c_str());
	obs_data_set_string(data, "videoSize", cfg.videoSize.c_str());
	obs_data_set_bool(data, "withStream", cfg.withStream);
	obs_data_set_bool(data, "recordComposite", cfg.recordComposite);
	obs_data_set_bool(data, "withRecording", cfg.withRecording);
}

// One object per key holds every collection's set, so saving one collection cannot
// drop another's: {"Mist": [{"name": "Mic"}], "Untitled": [{"name": "Mic"}]}.
static std::vector<std::string> loadSourceSet(const char *key, const std::string &collection)
{
	std::vector<std::string> names;
	obs_data_t *table = obs_data_get_obj(store(), key);
	obs_data_array_t *list = table ? obs_data_get_array(table, collection.c_str()) : nullptr;
	const size_t count = list ? obs_data_array_count(list) : 0;
	for (size_t i = 0; i < count; ++i) {
		obs_data_t *entry = obs_data_array_item(list, i);
		const char *name = obs_data_get_string(entry, "name");
		if (name && *name)
			names.push_back(name);
		obs_data_release(entry);
	}
	obs_data_array_release(list);
	obs_data_release(table);
	return names;
}

static void saveSourceSet(const char *key, const std::string &collection, const std::vector<std::string> &names)
{
	obs_data_t *table = obs_data_get_obj(store(), key);
	if (!table) {
		table = obs_data_create();
		obs_data_set_obj(store(), key, table);
	}
	obs_data_array_t *list = obs_data_array_create();
	for (const std::string &name : names) {
		obs_data_t *entry = obs_data_create();
		obs_data_set_string(entry, "name", name.c_str());
		obs_data_array_push_back(list, entry);
		obs_data_release(entry);
	}
	obs_data_set_array(table, collection.c_str(), list);
	obs_data_array_release(list);
	obs_data_release(table);
}

std::vector<std::string> loadArmedSources(const std::string &collection)
{
	return loadSourceSet("armedSources", collection);
}

void saveArmedSources(const std::string &collection, const std::vector<std::string> &names)
{
	saveSourceSet("armedSources", collection, names);
}

std::vector<std::string> loadHalfRateSources(const std::string &collection)
{
	return loadSourceSet("halfRateSources", collection);
}

void saveHalfRateSources(const std::string &collection, const std::vector<std::string> &names)
{
	saveSourceSet("halfRateSources", collection, names);
}

} // namespace iso
