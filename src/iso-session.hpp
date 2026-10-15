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
enum class RowState { Idle, WillRecord, Recording, Failed, Aborted };

struct SessionConfig {
	std::string basePath;
	std::string videoEncoderId;
	obs_data_t *videoSettings = nullptr; // borrowed; the dock owns the copy it hands over
	obs_data_t *audioSettings = nullptr; // borrowed; the dock owns the copy it hands over
	std::string videoSize = "stream"; // "stream", "source", or a height: "2160".."480"
	uint32_t capWidth = 0;            // resolved from videoSize once per session; 0 means no cap
	uint32_t capHeight = 0;
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
	// Preflight and start from the current config; returns the status line for the dock.
	std::string startWithReport();
	// canResume belongs to the dock's Stop button: pressing Record again carries on in
	// the same folder and on the same clock, in a new numbered game folder, so an
	// editor gets one session rather than two that cannot be lined up. Every other
	// stop — the stream ending, OBS shutting down — ends the session for good.
	void stop(bool canResume = false);
	// Close every running file and start a fresh numbered game folder, keeping the
	// session and its clock. Returns false when no session is running.
	bool newGame(std::string *error);
	// Returns the newest failure message when a recorder died since the last call,
	// empty otherwise; the dock puts it on screen.
	std::string refreshManifest();
	bool arm(obs_source_t *source, std::string *error);
	void disarm(obs_source_t *source);
	void onSceneChanged(obs_source_t *scene);
	double elapsedSeconds() const;
	const std::string &gameFolder() const { return gameFolder_; }

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
		// A game or window capture has no picture until its app is up, which can be
		// long after the stream started. The entry stays armed and keeps trying.
		bool waitingForPicture = false;
		std::unique_ptr<class SourceRecorder> recorder;
	};
	Entry *find(obs_source_t *source);
	const Entry *find(obs_source_t *source) const;
	bool startEntry(Entry &e, std::string *error, bool atSessionStart);
	// Start any armed source that finally has a picture. Called once a second.
	void retryWaiting();
	// Move any source whose size changed into a new segment file. Called once a second.
	void restartResized();
	void removeEntry(obs_source_t *source, Status status, const std::string &reason);
	// Drop any armed source deleted from OBS. The entry's reference keeps it alive
	// until then, so polling once a second is safe.
	void dropRemovedSources();
	void closeEntry(Entry &e, Status okStatus, const std::string &abortReason = {});
	// Stop a recorder and move its final state into finished_.
	void finishRecorder(class SourceRecorder &rec, const std::string &name, Kind kind, const std::string &file,
			    const std::string &label, Status okStatus, const std::string &abortReason = {});
	void recordFailure(const std::string &name, Kind kind, const std::string &label, const std::string &reason);
	Recording liveRecording(const class SourceRecorder &rec, const std::string &name, Kind kind,
				const std::string &file, const std::string &label) const;
	bool startComposite(std::string *error, bool atSessionStart);
	void closeComposite();
	void resetNumbering();
	// Advance to the next "NN_Game" folder and create it.
	bool openNextGameFolder(std::string *error);
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
	obs_data_t *videoSettings_ = nullptr; // owned by the session (strong ref)
	obs_data_t *audioSettings_ = nullptr; // owned by the session (strong ref)
	SessionConfig config_;
	SessionInfo info_;
	bool globalAudioHeld_ = false;
	bool canResume_ = false; // the last stop was the Stop button, so Record continues it
};

} // namespace iso
