#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace iso {

enum class Kind { Video, Audio };
enum class Status { Recording, Complete, Aborted, Failed };

// How a source's audio was set up and how it came out, so a bad-sounding file can be traced
// to a cause (a device that changed, a filter stack, clipping) from session.json alone.
struct AudioHealth {
	uint32_t sampleRate = 0;
	std::string sourceKind;
	std::string device; // the capture device the source follows; "default" follows the system's
	std::vector<std::string> filters;
	std::vector<std::string> changes; // setup edits made while recording, each with its time
	std::vector<std::string> flags;   // plain-words problems worth a look
	std::optional<double> peakDb;     // none while nothing but digital silence arrived
	uint64_t clipped = 0;
	uint64_t jumps = 0;
	double silentPct = 0;
	double idlePct = 0;
	double longestSilence = 0; // seconds
	double maxGapMs = 0;
};

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
	std::optional<AudioHealth> audio;
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
// The OBS output a codec is written through, and the extension its file gets.
struct Container {
	const char *outputId;
	const char *extension;
};
Container containerFor(const std::string &codec);
const char *extensionFor(Kind kind, const std::string &codec);
std::string codecForEncoderId(const std::string &id);

std::string sanitizeName(const std::string &source);
std::string makeFilename(Kind kind, int index, const std::string &source, int segment, const std::string &codec);
std::string formatClock(double seconds);
std::string formatSeconds(double seconds);

std::string buildSessionJson(const SessionInfo &info, const std::vector<Recording> &recordings);
std::string buildReadme(const SessionInfo &info, const std::vector<Recording> &recordings);

} // namespace iso
