#include "iso-settings.hpp"

#include <obs.h>
#include <obs-frontend-api.h>
#include <util/config-file.h>

namespace iso {

static const char *section = "iso-recorder";

SessionConfig loadConfig()
{
	SessionConfig cfg;
	config_t *config = obs_frontend_get_profile_config();
	if (!config)
		return cfg;
	const char *path = config_get_string(config, section, "basePath");
	if (path)
		cfg.basePath = path;
	const char *encoder = config_get_string(config, section, "videoEncoderId");
	if (encoder)
		cfg.videoEncoderId = encoder;
	const char *size = config_get_string(config, section, "videoSize");
	if (size && *size)
		cfg.videoSize = size;
	if (config_has_user_value(config, section, "withStream"))
		cfg.withStream = config_get_bool(config, section, "withStream");
	if (config_has_user_value(config, section, "recordComposite"))
		cfg.recordComposite = config_get_bool(config, section, "recordComposite");
	if (config_has_user_value(config, section, "withRecording")) {
		cfg.withRecording = config_get_bool(config, section, "withRecording");
	}
	return cfg;
}

void saveConfig(const SessionConfig &cfg)
{
	config_t *config = obs_frontend_get_profile_config();
	if (!config)
		return;
	config_set_string(config, section, "basePath", cfg.basePath.c_str());
	config_set_string(config, section, "videoEncoderId", cfg.videoEncoderId.c_str());
	config_set_string(config, section, "videoSize", cfg.videoSize.c_str());
	config_set_bool(config, section, "withStream", cfg.withStream);
	config_set_bool(config, section, "recordComposite", cfg.recordComposite);
	config_set_bool(config, section, "withRecording", cfg.withRecording);
}

// One JSON object per key holds every collection's set, so saving one collection cannot
// drop another's: {"Mist": ["Mic", "Browser"], "Untitled": ["Mic"]}.
static obs_data_t *openSourceTable(config_t *config, const char *key)
{
	const char *json = config_get_string(config, section, key);
	obs_data_t *table = json ? obs_data_create_from_json(json) : nullptr;
	return table ? table : obs_data_create();
}

static std::vector<std::string> loadSourceSet(const char *key, const std::string &collection)
{
	std::vector<std::string> names;
	config_t *config = obs_frontend_get_profile_config();
	if (!config)
		return names;
	obs_data_t *table = openSourceTable(config, key);
	obs_data_array_t *list = obs_data_get_array(table, collection.c_str());
	const size_t count = list ? obs_data_array_count(list) : 0;
	for (size_t i = 0; i < count; ++i) {
		obs_data_t *entry = obs_data_array_item(list, i);
		const char *name = obs_data_get_string(entry, "name");
		if (name && *name)
			names.push_back(name);
		obs_data_release(entry);
	}
	if (list)
		obs_data_array_release(list);
	obs_data_release(table);
	return names;
}

static void saveSourceSet(const char *key, const std::string &collection, const std::vector<std::string> &names)
{
	config_t *config = obs_frontend_get_profile_config();
	if (!config)
		return;
	obs_data_t *table = openSourceTable(config, key);
	obs_data_array_t *list = obs_data_array_create();
	for (const std::string &name : names) {
		obs_data_t *entry = obs_data_create();
		obs_data_set_string(entry, "name", name.c_str());
		obs_data_array_push_back(list, entry);
		obs_data_release(entry);
	}
	obs_data_set_array(table, collection.c_str(), list);
	obs_data_array_release(list);
	const char *json = obs_data_get_json(table);
	if (json)
		config_set_string(config, section, key, json);
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
