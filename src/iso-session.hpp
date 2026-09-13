#pragma once

#include <obs.h>

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "session-writer.hpp"

namespace iso {

// Four concurrent 1080p60 hardware encodes were measured clean on an RTX 2050; six
// dropped about a third of the frames from one file without failing it, so the stream's
// own encode leaves room for three.
constexpr size_t kEncoderWarnThreshold = 3;

// What a source's row says at a glance, so the list is readable without hovering.
enum class RowState { Idle, WillRecord, Recording, Failed };

struct SessionConfig {
	std::string basePath;
	std::string videoEncoderId;
	obs_data_t *videoSettings = nullptr;            // borrowed for the call
	std::string audioCodec = "pcm_s24le";
	uint64_t bitrateBitsPerSec = 6000ull * 1000ull; // drives the disk check in Task 9
	bool recordComposite = false;
	bool withStream = true;
};

class IsoSession {
public:
	IsoSession();
	~IsoSession();

	bool active() const { return active_; }
	bool preflight(const SessionConfig &cfg, std::string *why);
	bool start(const SessionConfig &cfg, std::string *error);
	void stop();
	// Close every running file and start a fresh numbered game folder, keeping the
	// session and its clock. Returns false when no session is running.
	bool newGame(std::string *error);
	// Returns the newest failure message when a recorder died since the last call,
	// empty otherwise; the dock puts it on screen.
	std::string refreshManifest();
	bool arm(obs_source_t *source, std::string *error);
	void disarm(obs_source_t *source);
	void onSourceRemoved(obs_source_t *source);
	void onSceneChanged(obs_source_t *scene);
	uint64_t epochNs() const { return epochNs_; }
	double elapsedSeconds() const;
	const std::string &folder() const { return folder_; }
	const std::string &gameFolder() const { return gameFolder_; }
	std::vector<Recording> recordings() const;

	bool isArmed(obs_source_t *source) const;
	bool isArmedAny() const { return !entries_.empty(); }
	size_t armedVisualCount() const;
	// The names of the sources currently ticked, for remembering them.
	std::vector<std::string> armedSourceNames() const;
	std::string statusFor(obs_source_t *source) const;
	RowState rowStateFor(obs_source_t *source) const;
	void setConfig(const SessionConfig &cfg);
	const SessionConfig &config() const { return config_; }

private:
	struct Entry {
		obs_source_t *source = nullptr;
		std::string name;
		std::string label;
		Kind kind = Kind::Video;
		int index = 1;
		int segment = 1;
		std::unique_ptr<class SourceRecorder> recorder;
	};
	Entry *find(obs_source_t *source);
	const Entry *find(obs_source_t *source) const;
	bool startEntry(Entry &e, std::string *error, bool atSessionStart);
	void closeEntry(Entry &e, Status okStatus);
	bool startComposite(std::string *error, bool atSessionStart);
	void closeComposite();
	std::string segmentDir() const;
	double offsetFor(Kind kind, uint64_t offsetNs) const;
	void rewriteManifest();

	bool active_ = false;
	std::string folder_;
	std::string gameFolder_; // "01_Game", "02_Game", ... inside folder_
	int gameNumber_ = 1;
	uint64_t epochNs_ = 0;
	size_t failedSeen_ = 0;
	std::vector<Entry> entries_;
	std::unique_ptr<class SourceRecorder> composite_;
	std::vector<Recording> finished_;
	std::map<std::pair<std::string, Kind>, int> segmentCount_;
	std::map<std::pair<std::string, Kind>, int> indexFor_;
	std::map<Kind, int> nextIndex_{{Kind::Video, 1}, {Kind::Audio, 1}};
	std::string videoEncoderId_ = "obs_x264";
	obs_data_t *videoSettings_ = nullptr; // owned by the session (strong ref)
	std::string audioCodec_ = "pcm_s24le";
	SessionConfig config_;
	SessionInfo info_;
	bool globalAudioHeld_ = false;
};

} // namespace iso
