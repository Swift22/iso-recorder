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
	const char *codec = config_get_string(config, section, "audioCodec");
	if (codec)
		cfg.audioCodec = codec;
	if (config_has_user_value(config, section, "withStream"))
		cfg.withStream = config_get_bool(config, section, "withStream");
	if (config_has_user_value(config, section, "recordComposite"))
		cfg.recordComposite = config_get_bool(config, section, "recordComposite");
	return cfg;
}

void saveConfig(const SessionConfig &cfg)
{
	config_t *config = obs_frontend_get_profile_config();
	if (!config)
		return;
	config_set_string(config, section, "basePath", cfg.basePath.c_str());
	config_set_string(config, section, "videoEncoderId", cfg.videoEncoderId.c_str());
	config_set_string(config, section, "audioCodec", cfg.audioCodec.c_str());
	config_set_bool(config, section, "withStream", cfg.withStream);
	config_set_bool(config, section, "recordComposite", cfg.recordComposite);
}

} // namespace iso
