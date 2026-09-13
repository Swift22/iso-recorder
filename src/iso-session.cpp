#include "iso-session.hpp"

#include "source-recorder.hpp"

#include <obs-frontend-api.h>
#include <util/platform.h>

#include <cmath>
#include <cstdio>
#include <ctime>

namespace iso {

// A per-source tap can only read a source's public audio buffer, and OBS fills
// that buffer only when the global audio device is being rendered for mix 0.
// With nothing streaming or recording there is no consumer on the global audio,
// so every source's buffer stays silent. Holding one consumer for the length of
// the session keeps the engine rendering so the taps see real audio. The buffer
// itself is ignored here; the recorders each read their own source.
static void onGlobalAudioKeepalive(void *, size_t, struct audio_data *) {}

IsoSession::IsoSession() = default;
IsoSession::~IsoSession()
{
	stop();
	if (videoSettings_)
		obs_data_release(videoSettings_);
}

static std::tm localTime(std::time_t t)
{
	std::tm tm{};
#ifdef _WIN32
	localtime_s(&tm, &t);
#else
	localtime_r(&t, &tm);
#endif
	return tm;
}

static std::string nowStamp()
{
	std::time_t t = std::time(nullptr);
	std::tm tm = localTime(t);
	char b[32];
	std::strftime(b, sizeof b, "%Y-%m-%d %H-%M-%S", &tm);
	return b;
}

static std::string isoNow()
{
	std::time_t t = std::time(nullptr);
	std::tm tm = localTime(t);
	char b[32];
	std::strftime(b, sizeof b, "%Y-%m-%dT%H:%M:%S%z", &tm);
	return b;
}

static std::string humanDate()
{
	std::time_t t = std::time(nullptr);
	std::tm tm = localTime(t);
	char b[64];
	std::strftime(b, sizeof b, "%d %B %Y", &tm);
	return b;
}

static std::string platformName()
{
#ifdef __APPLE__
	return "macos";
#else
	return "windows";
#endif
}

static void writeFile(const std::string &path, const std::string &text)
{
	FILE *f = fopen(path.c_str(), "wb");
	if (!f) {
		blog(LOG_WARNING, "[iso-recorder] could not write %s", path.c_str());
		return;
	}
	fwrite(text.data(), 1, text.size(), f);
	fclose(f);
}

static std::string labelFor(const std::string &name, Kind kind)
{
	if (name == "Mic")
		return "microphone";
	if (name == "Discord")
		return "Discord call";
	if (name == "Session")
		return "the stream as viewers saw it (reference)";
	return kind == Kind::Video ? name + ", on its own" : name;
}

IsoSession::Entry *IsoSession::find(obs_source_t *source)
{
	for (auto &e : entries_)
		if (e.source == source)
			return &e;
	return nullptr;
}

const IsoSession::Entry *IsoSession::find(obs_source_t *source) const
{
	for (const auto &e : entries_)
		if (e.source == source)
			return &e;
	return nullptr;
}

bool IsoSession::isArmed(obs_source_t *source) const { return find(source) != nullptr; }

size_t IsoSession::armedVisualCount() const
{
	size_t count = 0;
	for (const auto &e : entries_)
		if (e.kind == Kind::Video)
			++count;
	return count;
}

std::vector<std::string> IsoSession::armedSourceNames() const
{
	std::vector<std::string> names;
	names.reserve(entries_.size());
	for (const auto &e : entries_)
		names.push_back(e.name);
	return names;
}

std::string IsoSession::statusFor(obs_source_t *source) const
{
	const Entry *e = find(source);
	if (e && e->recorder) {
		if (e->recorder->failed())
			return e->recorder->error();
		return makeFilename(e->kind, e->index, e->name, e->segment);
	}
	const std::string name = obs_source_get_name(source);
	for (auto it = finished_.rbegin(); it != finished_.rend(); ++it)
		if (it->status == Status::Failed && it->source == name)
			return it->error;
	return "";
}

RowState IsoSession::rowStateFor(obs_source_t *source) const
{
	const Entry *e = find(source);
	if (e && e->recorder)
		return e->recorder->failed() ? RowState::Failed : RowState::Recording;
	if (e)
		return RowState::WillRecord;
	const std::string name = obs_source_get_name(source);
	for (auto it = finished_.rbegin(); it != finished_.rend(); ++it)
		if (it->status == Status::Failed && it->source == name)
			return RowState::Failed;
	return RowState::Idle;
}

double IsoSession::elapsedSeconds() const
{
	return active_ ? (double)(os_gettime_ns() - epochNs_) / 1e9 : 0.0;
}

static uint64_t freeBytes(const std::string &path)
{
	const int64_t free = os_get_free_space(path.c_str());
	return free > 0 ? (uint64_t)free : 0;
}

static uint64_t requiredBytes(size_t armedVisuals, uint64_t bitrateBitsPerSec)
{
	const uint64_t oneHour = bitrateBitsPerSec / 8ull * 3600ull;
	return (uint64_t)armedVisuals * oneHour * 11ull / 10ull;
}

static std::string humanBytes(uint64_t bytes)
{
	static const char *units[] = {"B", "KB", "MB", "GB", "TB"};
	double v = (double)bytes;
	size_t u = 0;
	while (v >= 1024.0 && u < 4) {
		v /= 1024.0;
		++u;
	}
	char b[32];
	snprintf(b, sizeof b, "%.1f %s", v, units[u]);
	return b;
}

bool IsoSession::preflight(const SessionConfig &cfg, std::string *why)
{
	if (cfg.basePath.empty()) {
		if (why)
			*why = "Choose a folder to save in.";
		return false;
	}
	if (os_mkdirs(cfg.basePath.c_str()) != 0 && !os_file_exists(cfg.basePath.c_str())) {
		if (why)
			*why = "That folder cannot be created.";
		return false;
	}
	const uint64_t need = requiredBytes(armedVisualCount() + (cfg.recordComposite ? 1 : 0),
					    cfg.bitrateBitsPerSec);
	const uint64_t free = freeBytes(cfg.basePath);
	if (free < need) {
		if (why)
			*why = "Not enough space: the selection needs about " + humanBytes(need) +
			       " and only " + humanBytes(free) + " is free.";
		return false;
	}
	return true;
}

void IsoSession::setConfig(const SessionConfig &cfg)
{
	if (!active_)
		config_ = cfg;
}

double IsoSession::offsetFor(Kind kind, uint64_t offsetNs) const
{
	double offset = (double)offsetNs / 1e9;
	if (kind == Kind::Video && info_.video.fps > 0.0)
		offset = (double)std::llround(offset * info_.video.fps) / info_.video.fps;
	return offset;
}

bool IsoSession::arm(obs_source_t *source, std::string *error)
{
	if (find(source))
		return true; // already armed
	Entry e;
	e.source = obs_source_get_ref(source);
	e.name = obs_source_get_name(source);
	e.kind = (obs_source_get_output_flags(source) & OBS_SOURCE_VIDEO) ? Kind::Video : Kind::Audio;
	const auto key = std::make_pair(e.name, e.kind);
	e.segment = ++segmentCount_[key];
	int &index = indexFor_[key];
	if (index == 0)
		index = nextIndex_[e.kind]++;
	e.index = index;
	e.label = labelFor(e.name, e.kind);
	entries_.push_back(std::move(e));
	bool ok = true;
	if (active_) {
		ok = startEntry(entries_.back(), error, false);
		rewriteManifest();
	}
	return ok;
}

void IsoSession::disarm(obs_source_t *source)
{
	Entry *e = find(source);
	if (!e)
		return;
	closeEntry(*e, Status::Aborted);
	obs_source_release(e->source);
	const auto at = e - entries_.data();
	entries_.erase(entries_.begin() + at);
	rewriteManifest();
}

std::string IsoSession::segmentDir() const { return folder_ + "/" + gameFolder_; }

// Close one recorder into the manifest. Everything the recording needs is read off
// the recorder before stop() clears its start time.
void IsoSession::closeEntry(Entry &e, Status okStatus)
{
	if (!e.recorder)
		return;
	const std::string file = makeFilename(e.kind, e.index, e.name, e.segment);
	const std::string codec = e.recorder->codec();
	const double offset = offsetFor(e.kind, e.recorder->startOffsetNs());
	const double duration = e.recorder->durationSeconds();
	const bool failed = e.recorder->failed();
	const std::string error = failed ? e.recorder->error() : "";
	e.recorder->stop();
	finished_.push_back({e.name, e.kind, file, codec, offset, duration,
			     failed ? Status::Failed : okStatus, error, e.label, gameFolder_});
	e.recorder.reset();
}

bool IsoSession::startComposite(std::string *error, bool atSessionStart)
{
	obs_source_t *scene = obs_frontend_get_current_scene();
	if (!scene) {
		finished_.push_back({"Session", Kind::Video, "", "", std::nullopt, std::nullopt,
				     Status::Failed, "no live scene to record yet",
				     labelFor("Session", Kind::Video), gameFolder_});
		blog(LOG_WARNING,
		     "[iso-recorder] the live scene is not ready; not recording 00_Session.mov");
		return false;
	}
	composite_ = std::make_unique<SourceRecorder>(scene, "Session",
						      segmentDir() + "/00_Session.mov", videoEncoderId_,
						      videoSettings_, Kind::Video, audioCodec_);
	obs_source_release(scene);
	std::string err;
	if (!composite_->start(&err)) {
		finished_.push_back({"Session", Kind::Video, "", "", std::nullopt, std::nullopt,
				     Status::Failed, err, labelFor("Session", Kind::Video),
				     gameFolder_});
		blog(LOG_WARNING, "[iso-recorder] Session: %s", err.c_str());
		composite_.reset();
		if (error && error->empty())
			*error = "the live scene: " + err;
		return false;
	}
	composite_->noteStart(epochNs_, atSessionStart);
	return true;
}

void IsoSession::closeComposite()
{
	if (!composite_)
		return;
	const double offset = offsetFor(Kind::Video, composite_->startOffsetNs());
	const double duration = composite_->durationSeconds();
	const bool failed = composite_->failed();
	const std::string error = failed ? composite_->error() : "";
	composite_->stop();
	finished_.push_back({"Session", Kind::Video,
			     makeFilename(Kind::Video, 0, "Session", 1), composite_->codec(),
			     offset, duration, failed ? Status::Failed : Status::Complete,
			     error, labelFor("Session", Kind::Video), gameFolder_});
	composite_.reset();
}

bool IsoSession::startEntry(Entry &e, std::string *error, bool atSessionStart)
{
	const std::string file = makeFilename(e.kind, e.index, e.name, e.segment);
	const std::string path = segmentDir() + "/" + file;
	e.recorder = std::make_unique<SourceRecorder>(e.source, e.name, path, videoEncoderId_,
						      videoSettings_, e.kind, audioCodec_);
	std::string err;
	if (!e.recorder->start(&err)) {
		finished_.push_back({e.name, e.kind, "", "", std::nullopt, std::nullopt,
				     Status::Failed, err, e.label, gameFolder_});
		blog(LOG_WARNING, "[iso-recorder] %s: %s", e.name.c_str(), err.c_str());
		if (error && error->empty())
			*error = e.name + ": " + err;
		// Destroy last: a half-started recorder is torn down here, and a
		// failure while doing so must not hide the error above.
		e.recorder.reset();
		return false;
	}
	e.recorder->noteStart(epochNs_, atSessionStart);
	return true;
}

bool IsoSession::start(const SessionConfig &cfg, std::string *error)
{
	if (error)
		error->clear();
	config_ = cfg;
	folder_ = cfg.basePath + "/" + nowStamp();
	if (os_mkdirs(folder_.c_str()) != 0 && !os_file_exists(folder_.c_str())) {
		if (error)
			*error = "could not create the session folder";
		return false;
	}
	// One session can hold several games; every session starts with its first.
	gameNumber_ = 1;
	gameFolder_ = "01_Game";
	if (os_mkdirs(segmentDir().c_str()) != 0 && !os_file_exists(segmentDir().c_str())) {
		if (error)
			*error = "could not create the game folder";
		return false;
	}
	videoEncoderId_ = cfg.videoEncoderId;
	if (videoSettings_)
		obs_data_release(videoSettings_);
	videoSettings_ = cfg.videoSettings;
	if (videoSettings_)
		obs_data_addref(videoSettings_);
	audioCodec_ = cfg.audioCodec;
	active_ = true;
	if (!globalAudioHeld_) {
		obs_add_raw_audio_callback(0, nullptr, onGlobalAudioKeepalive, nullptr);
		globalAudioHeld_ = true;
	}
	epochNs_ = os_gettime_ns();
	info_ = {};
	info_.started = isoNow();
	info_.dateLine = humanDate();
	info_.obsVersion = obs_get_version_string();
	info_.platform = platformName();
	struct obs_video_info ovi;
	if (obs_get_video_info(&ovi)) {
		info_.video.width = (int)ovi.output_width;
		info_.video.height = (int)ovi.output_height;
		info_.video.fps = (double)ovi.fps_num / (double)ovi.fps_den;
	}
	finished_.clear();
	failedSeen_ = 0;
	// Numbering counters are NOT reset here: sources may have been armed while
	// the session was inactive, and their arm() already assigned index/segment.
	bool ok = true;
	for (auto &e : entries_)
		if (!startEntry(e, error, true))
			ok = false;
	if (config_.recordComposite)
		startComposite(nullptr, true);
	rewriteManifest();
	return ok;
}

void IsoSession::stop()
{
	for (auto &e : entries_) {
		closeEntry(e, Status::Complete);
		if (e.source)
			obs_source_release(e.source);
	}
	entries_.clear();
	closeComposite();
	if (globalAudioHeld_) {
		obs_remove_raw_audio_callback(0, onGlobalAudioKeepalive, nullptr);
		globalAudioHeld_ = false;
	}
	if (active_) {
		info_.ended = isoNow();
		info_.duration = (double)(os_gettime_ns() - epochNs_) / 1e9;
		rewriteManifest();
		active_ = false;
	}
	// Reset per-session numbering only now, after the final manifest is written
	// and the live entries are gone, so a later session's pre-arm cannot inherit
	// a stale index/segment.
	segmentCount_.clear();
	indexFor_.clear();
	nextIndex_ = {{Kind::Video, 1}, {Kind::Audio, 1}};
	failedSeen_ = 0;
}

std::vector<Recording> IsoSession::recordings() const { return finished_; }

// Close the current game's files and open the next one. The session, its clock and
// the armed sources all survive; only the folder and the file numbering restart.
bool IsoSession::newGame(std::string *error)
{
	if (error)
		error->clear();
	if (!active_)
		return false;
	for (auto &e : entries_)
		closeEntry(e, Status::Complete);
	closeComposite();
	// Renumber the surviving sources from one, so each game folder has its own
	// 01_, 02_ ... regardless of what happened in the folder before it.
	nextIndex_ = {{Kind::Video, 1}, {Kind::Audio, 1}};
	segmentCount_.clear();
	indexFor_.clear();
	for (auto &e : entries_) {
		const auto key = std::make_pair(e.name, e.kind);
		e.segment = 1;
		e.index = nextIndex_[e.kind]++;
		indexFor_[key] = e.index;
		segmentCount_[key] = 1;
	}
	++gameNumber_;
	char name[32];
	std::snprintf(name, sizeof name, "%02d_Game", gameNumber_);
	gameFolder_ = name;
	if (os_mkdirs(segmentDir().c_str()) != 0 && !os_file_exists(segmentDir().c_str())) {
		if (error)
			*error = "could not create the game folder";
		return false;
	}
	bool ok = true;
	for (auto &e : entries_)
		if (!startEntry(e, error, false))
			ok = false;
	if (config_.recordComposite)
		startComposite(nullptr, false);
	rewriteManifest();
	return ok;
}

std::string IsoSession::refreshManifest()
{
	if (!active_)
		return {};
	size_t failed = 0;
	std::string message;
	for (const auto &e : entries_) {
		if (e.recorder && e.recorder->failed()) {
			++failed;
			message = (e.label.empty() ? e.name : e.label) + ": " + e.recorder->error();
		}
	}
	if (composite_ && composite_->failed()) {
		++failed;
		message = std::string("the live scene: ") + composite_->error();
	}
	if (failed > failedSeen_) {
		rewriteManifest();
		failedSeen_ = failed;
		return message;
	}
	return {};
}

void IsoSession::onSourceRemoved(obs_source_t *source) { disarm(source); }

void IsoSession::onSceneChanged(obs_source_t *scene)
{
	if (composite_ && composite_->active())
		composite_->setSource(scene);
}

void IsoSession::rewriteManifest()
{
	if (!active_)
		return;
	std::vector<Recording> all = finished_;
	for (auto &e : entries_) {
		if (!e.recorder)
			continue; // failed entries were appended to finished_ already
		Recording r;
		r.source = e.name;
		r.kind = e.kind;
		r.label = e.label;
		r.folder = gameFolder_;
		r.file = makeFilename(e.kind, e.index, e.name, e.segment);
		r.codec = e.recorder->codec();
		r.startOffset = offsetFor(e.kind, e.recorder->startOffsetNs());
		if (e.recorder->failed()) {
			r.status = Status::Failed;
			r.error = e.recorder->error();
			r.duration = e.recorder->durationSeconds();
		} else {
			r.status = Status::Recording;
		}
		all.push_back(std::move(r));
	}
	if (composite_ && (composite_->active() || composite_->failed())) {
		Recording r;
		r.source = "Session";
		r.kind = Kind::Video;
		r.label = labelFor("Session", Kind::Video);
		r.folder = gameFolder_;
		r.file = makeFilename(Kind::Video, 0, "Session", 1);
		r.codec = composite_->codec();
		r.startOffset = offsetFor(Kind::Video, composite_->startOffsetNs());
		if (composite_->failed()) {
			r.status = Status::Failed;
			r.error = composite_->error();
			r.duration = composite_->durationSeconds();
		} else {
			r.status = Status::Recording;
		}
		all.push_back(std::move(r));
	}
	writeFile(folder_ + "/session.json", buildSessionJson(info_, all));
	writeFile(folder_ + "/README.txt", buildReadme(info_, all));
}

} // namespace iso
