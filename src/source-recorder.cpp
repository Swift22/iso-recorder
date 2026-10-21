#include "source-recorder.hpp"

#include <util/platform.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <utility>

namespace iso {

// A private view renders its channels in ascending order and the default straight
// alpha blend lets a source's transparent pixels show what is beneath it, so the
// underlay goes in channel 0 and the recorded source sits above it in channel 1.
static constexpr size_t kBackgroundChannel = 0;
static constexpr size_t kSourceChannel = 1;
static constexpr long long kGreenScreenColor = 0xFF00FF00LL;

bool hasPicture(obs_source_t *source)
{
	return obs_source_get_width(source) >= 145 && obs_source_get_height(source) >= 49;
}

// An owned deep copy, so later edits to the caller's settings never reach a running encoder.
static obs_data_t *cloneData(obs_data_t *src)
{
	const char *json = src ? obs_data_get_json(src) : nullptr;
	return json ? obs_data_create_from_json(json) : nullptr;
}

SourceRecorder::SourceRecorder(obs_source_t *source, std::string name, std::string path, std::string videoEncoderId,
			       obs_data_t *videoSettings, obs_data_t *audioSettings, uint32_t capWidth,
			       uint32_t capHeight, uint32_t frameRateDivisor)
	: source_(obs_source_get_ref(source)),
	  name_(std::move(name)),
	  path_(std::move(path)),
	  videoEncoderId_(std::move(videoEncoderId)),
	  capWidth_(capWidth),
	  capHeight_(capHeight),
	  divisor_(frameRateDivisor)
{
	const uint32_t flags = obs_source_get_output_flags(source_);
	hasVideo_ = (flags & OBS_SOURCE_VIDEO) != 0;
	hasAudio_ = (flags & (OBS_SOURCE_AUDIO | OBS_SOURCE_COMPOSITE)) != 0;
	videoSettings_ = cloneData(videoSettings);
	audioSettings_ = cloneData(audioSettings);
}

SourceRecorder::~SourceRecorder()
{
	stop();
	if (videoSettings_)
		obs_data_release(videoSettings_);
	if (audioSettings_)
		obs_data_release(audioSettings_);
	if (source_)
		obs_source_release(source_);
}

double SourceRecorder::durationSeconds() const
{
	const uint64_t first = firstFrameNs_.load();
	const uint64_t start = shared_ && first ? first : startNs_;
	if (start == 0) {
		return 0.0;
	}
	const uint64_t stopped = stopNs_.load();
	const uint64_t end = stopped ? stopped : os_gettime_ns();
	if (end <= start) {
		return 0.0;
	}
	const uint64_t paused = pausedNs_ + (paused_ ? end - pauseStartNs_ : 0);
	return paused < end - start ? (double)(end - start - paused) / 1e9 : 0.0;
}

bool SourceRecorder::setPaused(bool paused, std::string *error)
{
	if (paused == paused_ || !started_) {
		return true;
	}
	// OBS pauses at the encoder, and a shared recorder's encoders are the live stream's:
	// pausing this file would freeze the stream. It keeps recording through the pause.
	if (shared_) {
		if (paused) {
			blog(LOG_INFO,
			     "[iso-recorder] %s: shares the stream's encoder, so it records through the pause",
			     name_.c_str());
		}
		return true;
	}
	if (output_ && !obs_output_pause(output_, paused)) {
		if (error) {
			*error = "the recording would not pause";
		}
		return false;
	}
	const uint64_t now = os_gettime_ns();
	if (paused) {
		pauseStartNs_ = now;
	} else {
		pausedNs_ += now - pauseStartNs_;
	}
	paused_ = paused;
	return true;
}

uint64_t SourceRecorder::startOffsetNs() const
{
	const uint64_t first = firstFrameNs_.load();
	if (shared_ && first) {
		return first > epochNs_ ? first - epochNs_ : 0;
	}
	return startOffsetNs_;
}

void SourceRecorder::noteStart(uint64_t epochNs, bool atSessionStart)
{
	epochNs_ = epochNs;
	startNs_ = os_gettime_ns();
	startOffsetNs_ = atSessionStart ? 0 : startNs_ - epochNs_;
}

void SourceRecorder::setError(std::string msg)
{
	std::lock_guard<std::mutex> lock(errorMutex_);
	error_ = std::move(msg);
}

bool SourceRecorder::fail(std::string msg, std::string *error)
{
	failed_ = true;
	setError(msg);
	if (error) {
		*error = std::move(msg);
	}
	return false;
}

bool SourceRecorder::openAudio(uint32_t samplesPerSec)
{
	struct audio_output_info oi{};
	oi.name = name_.c_str();
	oi.speakers = SPEAKERS_STEREO;
	oi.samples_per_sec = samplesPerSec;
	oi.format = AUDIO_FORMAT_FLOAT_PLANAR;
	oi.input_param = this;
	oi.input_callback = onAudioInput;
	return audio_output_open(&audio_, &oi) == AUDIO_OUTPUT_SUCCESS;
}

bool SourceRecorder::start(std::string *error)
{
	if (!hasVideo_) {
		constexpr uint32_t kWavRate = 48000;
		if (!openAudio(kWavRate)) {
			return fail("the audio device did not open", error);
		}
		if (!wav_.open(path_, kWavRate, 2, 24)) {
			audio_output_close(audio_);
			audio_ = nullptr;
			return fail("could not write the WAV file", error);
		}
		if (!audio_output_connect(audio_, 0, nullptr, onPcm, this)) {
			audio_output_close(audio_);
			audio_ = nullptr;
			wav_.close();
			return fail("the audio device did not accept the recorder", error);
		}
		obs_source_inc_showing(source_);
		showingInced_ = true;
		codec_ = "pcm_s24le";
		started_ = true;
		return true;
	}

	struct obs_video_info ovi;
	if (!obs_get_video_info(&ovi))
		return fail("OBS has no video yet", error);
	uint32_t w = obs_source_get_width(source_);
	uint32_t h = obs_source_get_height(source_);
	if (w == 0 || h == 0)
		return fail("the source has no picture yet", error);
	w += (w & 1);
	h += (h & 1);
	baseWidth_ = w;
	baseHeight_ = h;

	uint32_t ow = w;
	uint32_t oh = h;
	if (capWidth_ && capHeight_) {
		const double scale = std::min({1.0, (double)capWidth_ / (double)w,
					      (double)capHeight_ / (double)h});
		ow = (uint32_t)std::llround((double)w * scale);
		oh = (uint32_t)std::llround((double)h * scale);
		ow += (ow & 1);
		oh += (oh & 1);
		if (ow > capWidth_)
			ow -= 2;
		if (oh > capHeight_)
			oh -= 2;
		if (ow == 0)
			ow = 2;
		if (oh == 0)
			oh = 2;
	}
	ovi.base_width = w;
	ovi.base_height = h;
	ovi.output_width = ow;
	ovi.output_height = oh;

	view_ = obs_view_create();
	video_ = obs_view_add2(view_, &ovi);
	if (!video_)
		return fail("could not make a private video output", error);

	obs_data_t *bg = obs_data_create();
	obs_data_set_int(bg, "color", kGreenScreenColor);
	obs_data_set_int(bg, "width", (long long)w);
	obs_data_set_int(bg, "height", (long long)h);
	background_ = obs_source_create_private(obs_get_latest_input_type_id("color_source"),
						name_.c_str(), bg);
	obs_data_release(bg);
	if (!background_)
		return fail("could not make the recording background", error);
	obs_view_set_source(view_, kBackgroundChannel, background_);
	obs_view_set_source(view_, kSourceChannel, source_);
	obs_source_inc_showing(background_);
	bgShowingInced_ = true;
	obs_source_inc_showing(source_);
	showingInced_ = true;

	// The file's name is fixed before the encoder opens, so the container follows the
	// requested codec even when the x264 fallback writes into it.
	const Container box = containerFor(codecForEncoderId(videoEncoderId_));
	if (!openVideoPipeline(videoEncoderId_, box.outputId)) {
		// A hardware encoder can refuse a source it cannot handle — most often
		// because the picture is smaller than its minimum frame size, which a
		// small avatar or overlay source easily is. The software encoder has no
		// such floor, so give the source one more chance before failing it.
		const std::string first = this->error();
		closeVideoPipeline();
		if (videoEncoderId_ != "obs_x264" && openVideoPipeline("obs_x264", box.outputId)) {
			blog(LOG_WARNING,
			     "[iso-recorder] %s: the %s encoder would not take this source (%s); recording it with obs_x264 instead",
			     name_.c_str(), videoEncoderId_.c_str(), first.c_str());
			codec_ = "h264";
			return true;
		}
		const std::string second = this->error();
		closeVideoPipeline();
		std::string msg = first;
		if (!second.empty() && second != first)
			msg += "; the software encoder also failed: " + second;
		return fail(std::move(msg), error);
	}
	codec_ = codecForEncoderId(videoEncoderId_);
	return true;
}

bool SourceRecorder::openVideoPipeline(const std::string &encoderId, const char *outputId)
{
	setError({});

	// Every NVENC session on a card shares one encoder chip with the stream. The stream's
	// quality settings (slow preset, two-pass, lookahead, B-frames) cost several plain
	// encodes each, and copying them to every recording starved the stream of frames.
	obs_data_t *encoderSettings = videoSettings_;
	obs_data_t *light = nullptr;
	if (encoderId.rfind("obs_nvenc", 0) == 0) {
		light = videoSettings_ ? cloneData(videoSettings_) : obs_data_create();
		obs_data_set_string(light, "preset", "p1");
		obs_data_set_string(light, "multipass", "disabled");
		obs_data_set_bool(light, "lookahead", false);
		obs_data_set_int(light, "bf", 0);
		encoderSettings = light;
	}
	videoEnc_ = obs_video_encoder_create(encoderId.c_str(), name_.c_str(), encoderSettings, nullptr);
	if (light) {
		obs_data_release(light);
	}
	if (!videoEnc_) {
		setError("the encoder did not open");
		return false;
	}
	if (divisor_ > 1 && !obs_encoder_set_frame_rate_divisor(videoEnc_, divisor_)) {
		blog(LOG_WARNING,
		     "[iso-recorder] %s: the %s encoder would not record at half frame rate; recording at full rate",
		     name_.c_str(), encoderId.c_str());
		divisor_ = 1;
	}
	obs_encoder_set_video(videoEnc_, video_);

	obs_data_t *settings = obs_data_create();
	obs_data_set_string(settings, "path", path_.c_str());
	output_ = obs_output_create(outputId, name_.c_str(), settings, nullptr);
	obs_data_release(settings);
	if (!output_) {
		setError("the container did not open");
		return false;
	}
	obs_output_set_video_encoder(output_, videoEnc_);

	// An OBS AV output refuses to start without both encoders, so even a
	// source with no sound gets an audio track; the tap writes silence.
	if (!openAudio(audio_output_get_sample_rate(obs_get_audio()))) {
		setError("the audio device did not open");
		return false;
	}
	audioEnc_ = obs_audio_encoder_create("ffmpeg_aac", name_.c_str(), audioSettings_, 0, nullptr);
	if (!audioEnc_) {
		setError("the audio encoder did not open");
		return false;
	}
	obs_encoder_set_audio(audioEnc_, audio_);
	obs_output_set_audio_encoder(output_, audioEnc_, 0);

	if (!obs_output_start(output_)) {
		const char *last = obs_output_get_last_error(output_);
		setError(last && *last ? last : "the output refused to start");
		return false;
	}
	started_ = true;
	outputStarted_ = true;
	os_event_init(&stopEvent_, OS_EVENT_TYPE_AUTO);
	signal_handler_connect(obs_output_get_signal_handler(output_), "stop", onOutputStopped, this);
	return true;
}

bool SourceRecorder::startShared(obs_encoder_t *video, obs_encoder_t *audio, std::string *error)
{
	shared_ = true;
	setError({});
	const std::string codec = codecForEncoderId(obs_encoder_get_id(video));
	obs_data_t *settings = obs_data_create();
	obs_data_set_string(settings, "path", path_.c_str());
	output_ = obs_output_create(containerFor(codec).outputId, name_.c_str(), settings, nullptr);
	obs_data_release(settings);
	if (!output_) {
		return fail("the container did not open", error);
	}
	videoEnc_ = obs_encoder_get_ref(video);
	audioEnc_ = obs_encoder_get_ref(audio);
	obs_output_set_video_encoder(output_, videoEnc_);
	obs_output_set_audio_encoder(output_, audioEnc_, 0);
	obs_output_add_packet_callback(output_, onPacket, this);

	if (!obs_output_start(output_)) {
		const char *last = obs_output_get_last_error(output_);
		std::string msg = last ? last : "";
		if (msg.empty()) {
			msg = "the output refused to start";
		}
		closeVideoPipeline();
		return fail(std::move(msg), error);
	}
	started_ = true;
	outputStarted_ = true;
	os_event_init(&stopEvent_, OS_EVENT_TYPE_AUTO);
	signal_handler_connect(obs_output_get_signal_handler(output_), "stop", onOutputStopped, this);
	codec_ = codec;
	return true;
}

// Runs after the output has dropped everything before its first keyframe, so the first
// video packet seen here is the first frame in the file.
void SourceRecorder::onPacket(obs_output_t *, struct encoder_packet *pkt, struct encoder_packet_time *, void *param)
{
	auto *self = static_cast<SourceRecorder *>(param);
	if (pkt->type != OBS_ENCODER_VIDEO || self->firstFrameNs_.load() != 0) {
		return;
	}
	self->firstFrameNs_.store((uint64_t)pkt->sys_dts_usec * 1000ull);
}

void SourceRecorder::closeVideoPipeline()
{
	// Only ever called after a start that failed, so the output never ran and
	// releasing it cannot block (the force_stop in stop() is what would hang).
	if (output_) {
		obs_output_release(output_);
		output_ = nullptr;
	}
	if (audioEnc_) {
		obs_encoder_release(audioEnc_);
		audioEnc_ = nullptr;
	}
	if (videoEnc_) {
		obs_encoder_release(videoEnc_);
		videoEnc_ = nullptr;
	}
	if (audio_) {
		audio_output_close(audio_);
		audio_ = nullptr;
	}
	started_ = false;
	outputStarted_ = false;
}

void SourceRecorder::onOutputStopped(void *param, calldata_t *calldata)
{
	auto *self = static_cast<SourceRecorder *>(param);
	if (self->stopEvent_)
		os_event_signal(self->stopEvent_);
	if (!self->started_)
		return; // our own stop() cleared this first
	self->failed_ = true;
	const char *last = nullptr;
	calldata_get_string(calldata, "last_error", &last);
	self->setError(last && *last ? last : "the recorder stopped early");
	self->stopNs_ = os_gettime_ns();
	self->started_ = false;
}

void SourceRecorder::stop()
{
	if (!started_ && !view_ && !audio_)
		return;
	started_ = false;
	if (stopNs_ == 0)
		stopNs_ = os_gettime_ns();
	if (output_) {
		if (outputStarted_) {
			// force_stop on an output that never started resets its
			// stopping event with nothing left to signal it, so the
			// release below would wait forever.
			obs_output_force_stop(output_);
			if (stopEvent_ && os_event_timedwait(stopEvent_, 5000) == ETIMEDOUT) {
				blog(LOG_WARNING,
				     "[iso-recorder] %s: the recorder did not report stopping within 5 seconds",
				     name_.c_str());
				// A mov_output only finalises when one more encoded
				// packet reaches it, and obs_output_destroy() waits
				// on that same stopping event for ever. Ending the
				// capture signals the event and deactivates the
				// output, so the release below still returns.
				obs_output_end_data_capture(output_);
			}
			signal_handler_disconnect(obs_output_get_signal_handler(output_), "stop",
						  onOutputStopped, this);
		}
		totalFrames_ = (uint32_t)obs_output_get_total_frames(output_);
		if (shared_) {
			obs_output_remove_packet_callback(output_, onPacket, this);
		}
		obs_output_release(output_);
		output_ = nullptr;
	}
	outputStarted_ = false;
	if (stopEvent_) {
		os_event_destroy(stopEvent_);
		stopEvent_ = nullptr;
	}
	// The encoders go before the audio device so the output's interleaver keeps
	// being fed for as long as the output is still draining.
	if (audioEnc_) {
		obs_encoder_release(audioEnc_);
		audioEnc_ = nullptr;
	}
	if (videoEnc_) {
		obs_encoder_release(videoEnc_);
		videoEnc_ = nullptr;
	}
	if (audio_) {
		if (!hasVideo_)
			audio_output_disconnect(audio_, 0, onPcm, this);
		audio_output_close(audio_);
		audio_ = nullptr;
	}
	if (showingInced_) {
		obs_source_dec_showing(source_);
		showingInced_ = false;
	}
	if (bgShowingInced_ && background_) {
		obs_source_dec_showing(background_);
		bgShowingInced_ = false;
	}
	if (view_) {
		obs_view_remove(view_);
		obs_view_destroy(view_);
		view_ = nullptr;
		video_ = nullptr;
	}
	if (background_) {
		obs_source_release(background_);
		background_ = nullptr;
	}
	wav_.close();
}

bool SourceRecorder::sizeChanged() const
{
	if (!view_ || !started_) {
		return false;
	}
	// A minimised window reports 0x0 for a moment, and a Spout capture whose sender quit
	// falls back to its placeholder; neither is a new size to record.
	if (!hasPicture(source_)) {
		return false;
	}
	uint32_t w = obs_source_get_width(source_);
	uint32_t h = obs_source_get_height(source_);
	w += (w & 1);
	h += (h & 1);
	return w != baseWidth_ || h != baseHeight_;
}

void SourceRecorder::setSource(obs_source_t *next)
{
	if (!view_ || !next || next == source_)
		return;
	if (showingInced_) {
		obs_source_dec_showing(source_);
		showingInced_ = false;
	}
	obs_source_release(source_);
	source_ = obs_source_get_ref(next);
	obs_view_set_source(view_, kSourceChannel, source_);
	obs_source_inc_showing(source_);
	showingInced_ = true;
}

struct MixCtx {
	audio_t *audio = nullptr;
	float *target[MAX_AUDIO_CHANNELS] = {};
	uint64_t minTs = 0;
};

// obs_source_enum_proc_t is void(obs_source_t *, obs_source_t *, void *) in OBS 32.
static void calcMinTs(obs_source_t *parent, obs_source_t *child, void *param)
{
	(void)parent;
	uint64_t *minTs = static_cast<uint64_t *>(param);
	if (!(obs_source_get_output_flags(child) & OBS_SOURCE_AUDIO))
		return;
	if (obs_source_audio_pending(child))
		return;
	const uint64_t ts = obs_source_get_audio_timestamp(child);
	if (ts == 0)
		return;
	if (*minTs == 0 || ts < *minTs)
		*minTs = ts;
}

static void mixChild(obs_source_t *parent, obs_source_t *child, void *param)
{
	(void)parent;
	auto *ctx = static_cast<MixCtx *>(param);
	if (!(obs_source_get_output_flags(child) & OBS_SOURCE_AUDIO))
		return;
	if (obs_source_audio_pending(child))
		return;
	const uint64_t cts = obs_source_get_audio_timestamp(child);
	if (cts == 0 || cts < ctx->minTs)
		return;
	struct obs_source_audio_mix cm {};
	obs_source_get_audio_mix(child, &cm);
	const uint64_t off = ns_to_audio_frames(audio_output_get_sample_rate(ctx->audio),
						cts - ctx->minTs);
	const size_t chans = audio_output_get_channels(ctx->audio);
	for (size_t ch = 0; ch < chans; ++ch) {
		const float *in = cm.output[0].data[ch];
		float *out = ctx->target[ch];
		if (!in || !out)
			continue;
		for (size_t f = 0; f + off < AUDIO_OUTPUT_FRAMES; ++f) {
			const float v = out[f + off] + in[f];
			out[f + off] = std::fmax(-1.0f, std::fmin(1.0f, v));
		}
	}
}

void SourceRecorder::onPcm(void *param, size_t, struct audio_data *data)
{
	auto *self = static_cast<SourceRecorder *>(param);
	if (self->failed_ || self->paused_) {
		return;
	}
	if (!self->wav_.write(reinterpret_cast<const float *const *>(data->data), data->frames)) {
		self->failed_ = true;
		self->setError("could not write the WAV file");
		blog(LOG_WARNING, "[iso-recorder] %s: could not write the WAV file",
		     self->name_.c_str());
	}
}

bool SourceRecorder::onAudioInput(void *param, uint64_t startTs, uint64_t endTs, uint64_t *newTs,
				  uint32_t activeMixers, struct audio_output_data *mixes)
{
	auto *self = static_cast<SourceRecorder *>(param);
	return self->fillAudio(startTs, endTs, newTs, activeMixers, mixes);
}

void SourceRecorder::mixPlain(struct audio_output_data *mixes, size_t mixIdx)
{
	struct obs_source_audio_mix audio {};
	obs_source_get_audio_mix(source_, &audio);
	for (size_t ch = 0; ch < audio_output_get_channels(audio_); ++ch) {
		float *out = mixes[mixIdx].data[ch];
		const float *in = audio.output[0].data[ch];
		if (!in || !out)
			continue;
		for (size_t f = 0; f < AUDIO_OUTPUT_FRAMES; ++f)
			out[f] = std::fmax(-1.0f, std::fmin(1.0f, out[f] + in[f]));
	}
}

void SourceRecorder::mixComposite(struct audio_output_data *mixes, size_t mixIdx, uint64_t minTs)
{
	MixCtx ctx {};
	ctx.audio = audio_;
	ctx.minTs = minTs;
	for (size_t ch = 0; ch < MAX_AUDIO_CHANNELS; ++ch)
		ctx.target[ch] = mixes[mixIdx].data[ch];
	obs_source_enum_active_tree(source_, mixChild, &ctx);
}

bool SourceRecorder::fillAudio(uint64_t startTs, uint64_t, uint64_t *newTs, uint32_t activeMixers,
			       struct audio_output_data *mixes)
{
	// Never return false here. OBS skips the whole tick on false, so the
	// audio encoder would never receive its first frame; the video encoder
	// paired to it then drops every frame while waiting for that frame.
	// OBS pre-zeroes the mix buffers, so leaving them untouched writes
	// real-time silence, which is what a live recorder should do when a
	// source has nothing to say yet.
	*newTs = startTs;
	if (obs_source_audio_pending(source_))
		return true;
	const bool composite = isComposite();
	uint64_t minTs = 0;
	if (composite) {
		obs_source_enum_active_tree(source_, calcMinTs, &minTs);
		*newTs = minTs ? minTs : obs_source_get_audio_timestamp(source_);
	} else {
		if (!hasAudio_)
			return true;
		const uint64_t ts = obs_source_get_audio_timestamp(source_);
		if (ts == 0)
			return true;
		*newTs = ts;
	}
	for (size_t mix = 0; mix < MAX_AUDIO_MIXES; ++mix) {
		if (!(activeMixers & (1u << mix)))
			continue;
		if (composite)
			mixComposite(mixes, mix, minTs);
		else
			mixPlain(mixes, mix);
	}
	if (*newTs == 0)
		*newTs = startTs;
	return true;
}

} // namespace iso
