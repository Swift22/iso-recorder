#pragma once

#include <optional>
#include <string>
#include <vector>

namespace iso {

enum class Kind { Video, Audio };
enum class Status { Recording, Complete, Aborted, Failed };

struct Recording {
	std::string source;
	Kind kind = Kind::Video;
	std::string file;
	std::string codec;
	std::optional<double> startOffset;
	std::optional<double> duration;
	Status status = Status::Recording;
	std::string error;
	std::string label;
	// The game folder this file lives in ("01_Game"), or empty for a session that
	// was never split. The file itself is always relative to this folder.
	std::string folder;
};

struct VideoFormat {
	int width = 1920;
	int height = 1080;
	double fps = 60.0;
};

struct SessionInfo {
	std::string started;
	std::string dateLine;
	std::optional<std::string> ended;
	std::optional<double> duration;
	std::string obsVersion;
	std::string platform;
	VideoFormat video;
};

const char *kindName(Kind kind);
const char *statusName(Status status);
const char *extensionFor(Kind kind);
std::string codecForEncoderId(const std::string &id);

std::string sanitizeName(const std::string &source);
std::string makeFilename(Kind kind, int index, const std::string &source, int segment);
std::string formatClock(double seconds);
std::string formatSeconds(double seconds);

std::string buildSessionJson(const SessionInfo &info, const std::vector<Recording> &recordings);
std::string buildReadme(const SessionInfo &info, const std::vector<Recording> &recordings);

} // namespace iso
