#include "iso-session.hpp"

#include "source-recorder.hpp"

#include <obs-frontend-api.h>
#include <util/platform.h>

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

#include <algorithm>
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

// The live-scene recording's name in the manifest; its file is 00_Session.mov or .mp4.
static constexpr const char *kSessionName = "Session";

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

static std::string formatNow(const char *format)
{
	std::tm tm = localTime(std::time(nullptr));
	char b[64];
	std::strftime(b, sizeof b, format, &tm);
	return b;
}

static std::string nowStamp()
{
	return formatNow("%Y-%m-%d %H-%M-%S");
}
static std::string isoNow()
{
	return formatNow("%Y-%m-%dT%H:%M:%S%z");
}
static std::string humanDate()
{
	return formatNow("%d %B %Y");
}

static bool ensureDir(const std::string &path)
{
	return os_mkdirs(path.c_str()) != MKDIR_ERROR;
}

static void replaceRef(obs_data_t *&slot, obs_data_t *next)
{
	if (next) {
		obs_data_addref(next);
	}
	if (slot) {
		obs_data_release(slot);
	}
	slot = next;
}

IsoSession::IsoSession() = default;
IsoSession::~IsoSession()
{
	stop();
	replaceRef(videoSettings_, nullptr);
	replaceRef(audioSettings_, nullptr);
}

static std::string platformName()
{
#ifdef __APPLE__
	return "macos";
#else
	return "windows";
#endif
}

static bool replaceFile(const std::string &from, const std::string &to)
{
#ifdef _WIN32
	// The C runtime's rename() refuses to replace an existing file, and the manifest is
	// rewritten in place for the whole session; MOVEFILE_REPLACE_EXISTING is the atomic
	// overwrite this tmp-then-swap needs.
	return MoveFileExA(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
#else
	return std::rename(from.c_str(), to.c_str()) == 0;
#endif
}

// A crash or force-kill while rewriting a manifest used to leave session.json torn in
// place, which reads as "OBS crashed here" even when it did not. Write the whole file
// beside the real one, force it to the platter, then swap it in, so the manifest is
// always either the old one or the new one.
static void writeFile(const std::string &path, const std::string &text)
{
	const std::string tmp = path + ".tmp";
	FILE *f = fopen(tmp.c_str(), "wb");
	if (!f) {
		blog(LOG_WARNING, "[iso-recorder] could not write %s", path.c_str());
		return;
	}
	bool ok = fwrite(text.data(), 1, text.size(), f) == text.size();
	if (ok && fflush(f) != 0)
		ok = false;
#ifdef _WIN32
	if (ok && _commit(_fileno(f)) != 0)
		ok = false;
#else
	if (ok && fsync(fileno(f)) != 0)
		ok = false;
#endif
	if (fclose(f) != 0)
		ok = false;
	if (!ok || !replaceFile(tmp, path)) {
		blog(LOG_WARNING, "[iso-recorder] could not write %s", path.c_str());
		std::remove(tmp.c_str());
	}
}

// A hardware encoder that falls behind drops frames without failing the output, so the
// container comes out short while every status still says complete. Compare the packets
// the output delivered against what the recording's own length should have produced.
static bool frameShortfall(uint32_t delivered, double duration, double fps, uint64_t *missing)
{
	if (fps <= 0.0 || duration <= 0.0)
		return false;
	// An encoder takes a moment to deliver its first frames, which is a large share of a
	// short file and none of a long one. Judging a few seconds of recording on that ramp
	// called healthy files broken, so short files are not judged and half a second of
	// start-up is never counted as missing.
	if (duration < 10.0) {
		return false;
	}
	const double expected = fps * duration;
	if (expected < 1.0)
		return false;
	const double lost = expected - (double)delivered;
	if (lost <= std::fmax(expected * 0.02, fps * 0.5)) {
		return false;
	}
	*missing = (uint64_t)std::llround(lost);
	return true;
}

static std::string shortfallReason(uint64_t missing)
{
	uint64_t rounded = missing;
	if (missing >= 1000)
		rounded = (missing / 100) * 100;
	else if (missing >= 100)
		rounded = (missing / 10) * 10;
	return "the encoder could not keep up: about " + std::to_string(rounded) +
	       " frames were never recorded";
}

static std::string labelFor(const std::string &name, Kind kind)
{
	if (name == "Mic")
		return "microphone";
	if (name == "Discord")
		return "Discord call";
	if (name == kSessionName) {
		return "the stream as viewers saw it (reference)";
	}
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
		return makeFilename(e->kind, e->index, e->name, e->segment, videoCodec());
	}
	if (e && e->waitingForPicture) {
		return "waiting for a picture — it will record as soon as one appears";
	}
	const std::string name = obs_source_get_name(source);
	for (auto it = finished_.rbegin(); it != finished_.rend(); ++it)
		if (it->source == name &&
		    (it->status == Status::Failed ||
		     (it->status == Status::Aborted && !it->error.empty())))
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
	for (auto it = finished_.rbegin(); it != finished_.rend(); ++it) {
		if (it->source != name)
			continue;
		if (it->status == Status::Failed)
			return RowState::Failed;
		if (it->status == Status::Aborted && !it->error.empty())
			return RowState::Aborted;
	}
	return RowState::Idle;
}

double IsoSession::elapsedSeconds() const
{
	if (!active_) {
		return 0.0;
	}
	const uint64_t now = os_gettime_ns();
	return (double)(now - epochNs_ - pausedBefore(now)) / 1e9;
}

uint64_t IsoSession::pausedBefore(uint64_t wallNs) const
{
	uint64_t total = 0;
	for (const PauseSpan &span : pauses_) {
		const uint64_t end = span.end ? span.end : wallNs;
		const uint64_t until = (std::min)(end, wallNs);
		if (until > span.start) {
			total += until - span.start;
		}
	}
	return total;
}

std::string IsoSession::videoCodec() const
{
	return codecForEncoderId(config_.videoEncoderId);
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
	if (!ensureDir(cfg.basePath)) {
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
	// A file that starts after a pause sits further along the timeline by the time it
	// was paused, not by the wall clock.
	const uint64_t timeline = offsetNs - (std::min)(offsetNs, pausedBefore(epochNs_ + offsetNs));
	double offset = (double)timeline / 1e9;
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

// Unticking is a decision, not a fault: the file it closes is complete.
void IsoSession::disarm(obs_source_t *source)
{
	removeEntry(source, Status::Complete, {});
}

void IsoSession::removeEntry(obs_source_t *source, Status status, const std::string &reason)
{
	Entry *e = find(source);
	if (!e)
		return;
	closeEntry(*e, status, reason);
	obs_source_release(e->source);
	const auto at = e - entries_.data();
	entries_.erase(entries_.begin() + at);
	rewriteManifest();
}

std::string IsoSession::segmentDir() const { return folder_ + "/" + gameFolder_; }

// Close one recorder into the manifest. Everything the recording needs is read off
// the recorder before stop() clears its start time.
void IsoSession::finishRecorder(SourceRecorder &rec, const std::string &name, Kind kind, const std::string &file,
				const std::string &label, Status okStatus, const std::string &abortReason)
{
	const std::string codec = rec.codec();
	const double offset = offsetFor(kind, rec.startOffsetNs());
	const double duration = rec.durationSeconds();
	const bool failed = rec.failed();
	std::string reason = failed ? rec.error() : "";
	rec.stop();
	Status status = failed ? Status::Failed : okStatus;
	if (!failed && okStatus == Status::Aborted) {
		reason = abortReason;
	}
	uint64_t missing = 0;
	if (!failed && okStatus == Status::Complete && kind == Kind::Video &&
	    frameShortfall(rec.totalFrames(), duration, info_.video.fps / rec.frameRateDivisor(), &missing)) {
		status = Status::Aborted;
		reason = shortfallReason(missing);
		blog(LOG_WARNING, "[iso-recorder] %s: %s", name.c_str(), reason.c_str());
	}
	finished_.push_back({name, kind, file, codec, offset, duration, status, reason, label, gameFolder_});
}

void IsoSession::recordFailure(const std::string &name, Kind kind, const std::string &label, const std::string &reason)
{
	finished_.push_back(
		{name, kind, "", "", std::nullopt, std::nullopt, Status::Failed, reason, label, gameFolder_});
}

Recording IsoSession::liveRecording(const SourceRecorder &rec, const std::string &name, Kind kind,
				    const std::string &file, const std::string &label) const
{
	Recording r;
	r.source = name;
	r.kind = kind;
	r.label = label;
	r.folder = gameFolder_;
	r.file = file;
	r.codec = rec.codec();
	r.startOffset = offsetFor(kind, rec.startOffsetNs());
	if (rec.failed()) {
		r.status = Status::Failed;
		r.error = rec.error();
		r.duration = rec.durationSeconds();
	} else {
		r.status = Status::Recording;
	}
	return r;
}

void IsoSession::closeEntry(Entry &e, Status okStatus, const std::string &abortReason)
{
	if (!e.recorder)
		return;
	finishRecorder(*e.recorder, e.name, e.kind, makeFilename(e.kind, e.index, e.name, e.segment, videoCodec()),
		       e.label, okStatus, abortReason);
	e.recorder.reset();
}

bool IsoSession::startComposite(std::string *error, bool atSessionStart)
{
	obs_source_t *scene = obs_frontend_get_current_scene();
	if (!scene) {
		recordFailure(kSessionName, Kind::Video, labelFor(kSessionName, Kind::Video),
			      "no live scene to record yet");
		blog(LOG_WARNING,
		     "[iso-recorder] the live scene is not ready; not recording 00_Session.mov");
		return false;
	}
	// The live scene is the picture the stream is already encoding, and a second
	// full-canvas encode on the same GPU is what starves the stream. Take the
	// stream's packets while it is live; encode separately only without a stream.
	obs_output_t *stream = obs_frontend_get_streaming_output();
	obs_encoder_t *streamVideo = nullptr;
	obs_encoder_t *streamAudio = nullptr;
	if (stream && obs_output_active(stream)) {
		streamVideo = obs_output_get_video_encoder(stream);
		streamAudio = obs_output_get_audio_encoder(stream, 0);
	}
	const bool share = streamVideo && streamAudio && obs_encoder_active(streamVideo) &&
			   obs_encoder_active(streamAudio);
	// A shared composite is written in the stream's codec, so its file takes that container.
	const std::string codec = share ? codecForEncoderId(obs_encoder_get_id(streamVideo)) : videoCodec();
	compositeFile_ = makeFilename(Kind::Video, 0, kSessionName, 1, codec);
	composite_ = std::make_unique<SourceRecorder>(scene, kSessionName, segmentDir() + "/" + compositeFile_,
						      config_.videoEncoderId, videoSettings_, audioSettings_,
						      config_.capWidth, config_.capHeight, 1);
	obs_source_release(scene);
	std::string err;
	const bool started = share ? composite_->startShared(streamVideo, streamAudio, &err) : composite_->start(&err);
	if (stream) {
		obs_output_release(stream);
	}
	if (!started) {
		recordFailure(kSessionName, Kind::Video, labelFor(kSessionName, Kind::Video), err);
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
	finishRecorder(*composite_, kSessionName, Kind::Video, compositeFile_, labelFor(kSessionName, Kind::Video),
		       Status::Complete);
	composite_.reset();
}

bool IsoSession::startEntry(Entry &e, std::string *error, bool atSessionStart)
{
	// A capture whose app is not up yet has no picture. That is not a failure — the
	// source is still armed, and retryWaiting() starts it the moment a picture appears.
	if (e.kind == Kind::Video && (obs_source_get_width(e.source) == 0 || obs_source_get_height(e.source) == 0)) {
		if (!e.waitingForPicture) {
			e.waitingForPicture = true;
			blog(LOG_INFO, "[iso-recorder] %s: no picture yet, so it will start recording when one appears",
			     e.name.c_str());
		}
		return true;
	}
	e.waitingForPicture = false;
	const std::string file = makeFilename(e.kind, e.index, e.name, e.segment, videoCodec());
	const std::string path = segmentDir() + "/" + file;
	const uint32_t divisor = isHalfRate(e.source) ? 2u : 1u;
	e.recorder = std::make_unique<SourceRecorder>(e.source, e.name, path, config_.videoEncoderId, videoSettings_,
						      audioSettings_, config_.capWidth, config_.capHeight, divisor);
	std::string err;
	if (!e.recorder->start(&err)) {
		recordFailure(e.name, e.kind, e.label, err);
		blog(LOG_WARNING, "[iso-recorder] %s: %s", e.name.c_str(), err.c_str());
		if (error && error->empty())
			*error = e.name + ": " + err;
		// Destroy last: a half-started recorder is torn down here, and a
		// failure while doing so must not hide the error above.
		e.recorder.reset();
		return false;
	}
	e.recorder->noteStart(epochNs_, atSessionStart);
	// A source ticked during a pause starts paused, so it has nothing on the timeline
	// until the session resumes.
	if (paused_) {
		std::string pauseErr;
		if (!e.recorder->setPaused(true, &pauseErr)) {
			blog(LOG_WARNING, "[iso-recorder] %s: %s", e.name.c_str(), pauseErr.c_str());
			if (error && error->empty()) {
				*error = e.name + ": " + pauseErr;
			}
		}
	}
	return true;
}

bool IsoSession::start(const SessionConfig &cfg, std::string *error)
{
	if (error)
		error->clear();
	// Stopping with the button and pressing Record again is one session with a break in
	// it, not two: same folder, same clock, the next numbered game folder. Only a folder
	// that is still where it was, under the same base path, can be carried on.
	const bool resuming = canResume_ && !folder_.empty() && folder_.rfind(cfg.basePath + "/", 0) == 0 &&
			      os_file_exists(folder_.c_str());
	canResume_ = false;
	config_ = cfg;
	if (!resuming) {
		folder_ = cfg.basePath + "/" + nowStamp();
		if (!ensureDir(folder_)) {
			if (error) {
				*error = "could not create the session folder";
			}
			return false;
		}
		// One session can hold several games; every session starts with its first.
		gameNumber_ = 0;
	}
	if (!openNextGameFolder(error)) {
		return false;
	}
	replaceRef(videoSettings_, cfg.videoSettings);
	replaceRef(audioSettings_, cfg.audioSettings);
	active_ = true;
	if (!globalAudioHeld_) {
		obs_add_raw_audio_callback(0, nullptr, onGlobalAudioKeepalive, nullptr);
		globalAudioHeld_ = true;
	}
	// A resumed session keeps its clock, its start time and everything already recorded,
	// so every file in the folder stays on one timeline; only the "ended" line reopens.
	if (resuming) {
		info_.ended = std::nullopt;
		info_.duration = std::nullopt;
	} else {
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
		pauses_.clear();
	}
	paused_ = false;
	failedSeen_ = 0;
	// Numbering counters are NOT reset here: sources may have been armed while
	// the session was inactive, and their arm() already assigned index/segment.
	bool ok = true;
	// A resumed game is not the start of the clock: its files begin however long the break
	// lasted after the session epoch, exactly as a "New game" mid-session does, so the
	// editor drops them at their listed offset instead of on top of the first game.
	for (auto &e : entries_)
		if (!startEntry(e, error, !resuming))
			ok = false;
	if (config_.recordComposite)
		startComposite(nullptr, !resuming);
	rewriteManifest();
	return ok;
}

std::string IsoSession::startWithReport(StartedBy by)
{
	const SessionConfig cfg = config_; // a copy: start() assigns config_ from it
	std::string why;
	if (!preflight(cfg, &why)) {
		return why;
	}
	startedBy_ = by;
	std::string err;
	if (!start(cfg, &err)) {
		return active_ ? "Recording, but some sources failed: " + err : err;
	}
	if (armedVisualCount() > kEncoderWarnThreshold) {
		return "Recording anyway, but this is more videos than this machine is expected to handle.";
	}
	return "Recording.";
}

void IsoSession::stop(bool canResume)
{
	// A pause ends before the files close, so each one's clock stops on the timeline.
	if (paused_) {
		std::string ignored; // setPaused() has already warned about any file that would not resume
		setPaused(false, &ignored);
	}
	for (auto &e : entries_) {
		// A source that stayed pictureless for the whole session has no file and no
		// recorder, so say so in the manifest rather than leaving it out silently.
		if (!e.recorder && e.waitingForPicture) {
			recordFailure(e.name, e.kind, e.label, "the source never had a picture to record");
		}
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
		const uint64_t now = os_gettime_ns();
		info_.ended = isoNow();
		info_.duration = (double)(now - epochNs_ - pausedBefore(now)) / 1e9;
		rewriteManifest();
		active_ = false;
		canResume_ = canResume;
	}
	// Reset per-session numbering only now, after the final manifest is written
	// and the live entries are gone, so a later session's pre-arm cannot inherit
	// a stale index/segment.
	resetNumbering();
}

void IsoSession::resetNumbering()
{
	segmentCount_.clear();
	indexFor_.clear();
	nextIndex_ = {{Kind::Video, 1}, {Kind::Audio, 1}};
}

bool IsoSession::openNextGameFolder(std::string *error)
{
	++gameNumber_;
	char name[32];
	std::snprintf(name, sizeof name, "%02d_Game", gameNumber_);
	gameFolder_ = name;
	if (ensureDir(segmentDir())) {
		return true;
	}
	if (error) {
		*error = "could not create the game folder";
	}
	return false;
}

// Close the current game's files and open the next one. The session, its clock and
// the armed sources all survive; only the folder and the file numbering restart.
bool IsoSession::newGame(std::string *error)
{
	if (error)
		error->clear();
	if (!active_)
		return false;
	if (paused_) {
		if (error) {
			*error = "Recording is paused.";
		}
		return false;
	}
	for (auto &e : entries_)
		closeEntry(e, Status::Complete);
	closeComposite();
	// Renumber the surviving sources from one, so each game folder has its own
	// 01_, 02_ ... regardless of what happened in the folder before it.
	resetNumbering();
	for (auto &e : entries_) {
		const auto key = std::make_pair(e.name, e.kind);
		e.segment = 1;
		e.index = nextIndex_[e.kind]++;
		indexFor_[key] = e.index;
		segmentCount_[key] = 1;
	}
	if (!openNextGameFolder(error)) {
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

void IsoSession::retryWaiting()
{
	if (!active_) {
		return;
	}
	bool started = false;
	for (auto &e : entries_) {
		if (!e.waitingForPicture || e.recorder) {
			continue;
		}
		if (obs_source_get_width(e.source) == 0 || obs_source_get_height(e.source) == 0) {
			continue;
		}
		if (startEntry(e, nullptr, false) && e.recorder) {
			blog(LOG_INFO, "[iso-recorder] %s: a picture appeared, now recording", e.name.c_str());
			started = true;
		}
	}
	if (started) {
		rewriteManifest();
	}
}

// A view is built for one size, so a source that changes size mid-recording (a window
// resized, a game switching resolution) would be cropped or padded from then on. Close
// that file and carry on in the next segment at the new size.
void IsoSession::restartResized()
{
	bool restarted = false;
	for (auto &e : entries_) {
		if (!e.recorder || e.recorder->failed() || !e.recorder->sizeChanged()) {
			continue;
		}
		blog(LOG_INFO, "[iso-recorder] %s: the source changed size, so it continues in a new file",
		     e.name.c_str());
		closeEntry(e, Status::Complete);
		e.segment = ++segmentCount_[std::make_pair(e.name, e.kind)];
		startEntry(e, nullptr, false);
		restarted = true;
	}
	if (restarted) {
		rewriteManifest();
	}
}

std::string IsoSession::refreshManifest()
{
	dropRemovedSources();
	if (!active_)
		return {};
	// A paused session starts no files and closes none for size, so the pause stays clean.
	if (!paused_) {
		retryWaiting();
		restartResized();
	}
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

void IsoSession::dropRemovedSources()
{
	std::vector<obs_source_t *> gone;
	for (const auto &e : entries_) {
		if (obs_source_removed(e.source)) {
			gone.push_back(e.source);
		}
	}
	for (obs_source_t *source : gone) {
		removeEntry(source, Status::Aborted, "the source was deleted while it was recording");
	}
}

void IsoSession::onSceneChanged(obs_source_t *scene)
{
	if (composite_ && composite_->active())
		composite_->setSource(scene);
}

bool IsoSession::setPaused(bool paused, std::string *error)
{
	if (error) {
		error->clear();
	}
	if (!active_ || paused == paused_) {
		return true;
	}
	const uint64_t now = os_gettime_ns();
	if (paused) {
		pauses_.push_back({now, 0});
	} else {
		pauses_.back().end = now;
	}
	paused_ = paused;
	bool ok = true;
	auto apply = [&](SourceRecorder &rec, const std::string &label) {
		std::string err;
		if (rec.setPaused(paused, &err)) {
			return;
		}
		ok = false;
		blog(LOG_WARNING, "[iso-recorder] %s: %s", label.c_str(), err.c_str());
		if (error && error->empty()) {
			*error = std::string(paused ? "Could not pause " : "Could not resume ") + label + ": " + err +
				 ".";
		}
	};
	for (auto &e : entries_) {
		if (e.recorder) {
			apply(*e.recorder, e.label);
		}
	}
	if (composite_) {
		apply(*composite_, labelFor(kSessionName, Kind::Video));
	}
	return ok;
}

void IsoSession::setHalfRate(obs_source_t *source, bool half)
{
	const std::string name = obs_source_get_name(source);
	if (half) {
		halfRate_.insert(name);
	} else {
		halfRate_.erase(name);
	}
}

bool IsoSession::isHalfRate(obs_source_t *source) const
{
	return halfRate_.count(obs_source_get_name(source)) != 0;
}

void IsoSession::rewriteManifest()
{
	if (!active_)
		return;
	std::vector<Recording> all = finished_;
	for (const auto &e : entries_) {
		if (e.recorder) { // failed entries were appended to finished_ already
			all.push_back(liveRecording(*e.recorder, e.name, e.kind,
						    makeFilename(e.kind, e.index, e.name, e.segment, videoCodec()),
						    e.label));
		}
	}
	if (composite_ && (composite_->active() || composite_->failed())) {
		all.push_back(liveRecording(*composite_, kSessionName, Kind::Video, compositeFile_,
					    labelFor(kSessionName, Kind::Video)));
	}
	writeFile(folder_ + "/session.json", buildSessionJson(info_, all));
	writeFile(folder_ + "/README.txt", buildReadme(info_, all));
}

} // namespace iso
