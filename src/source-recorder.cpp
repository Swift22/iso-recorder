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

SourceRecorder::SourceRecorder(obs_source_t *source, std::string name, std::string path,
			       std::string videoEncoderId, obs_data_t *videoSettings,
			       obs_data_t *audioSettings, uint32_t capWidth, uint32_t capHeight,
			       Kind kind, std::string audioCodec)
	: source_(obs_source_get_ref(source)), name_(std::move(name)), path_(std::move(path)),
	  kind_(kind), videoEncoderId_(std::move(videoEncoderId)), capWidth_(capWidth),
	  capHeight_(capHeight), audioCodec_(std::move(audioCodec))
{
	const uint32_t flags = obs_source_get_output_flags(source_);
	hasVideo_ = (flags & OBS_SOURCE_VIDEO) != 0;
	hasAudio_ = (flags & (OBS_SOURCE_AUDIO | OBS_SOURCE_COMPOSITE)) != 0;
	if (videoSettings) {
		const char *json = obs_data_get_json(videoSettings);
		if (json)
			videoSettings_ = obs_data_create_from_json(json);
	}
	if (audioSettings) {
		const char *json = obs_data_get_json(audioSettings);
		if (json)
			audioSettings_ = obs_data_create_from_json(json);
	}
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
	if (startNs_ == 0)
		return 0.0;
	const uint64_t end = stopNs_ ? stopNs_ : os_gettime_ns();
	return (double)(end - startNs_) / 1e9;
}

void SourceRecorder::noteStart(uint64_t epochNs, bool atSessionStart)
{
	epochNs_ = epochNs;
	startNs_ = os_gettime_ns();
	startOffsetNs_ = atSessionStart ? 0 : startNs_ - epochNs_;
}

bool SourceRecorder::start(std::string *error)
{
	auto fail = [&](std::string msg) {
		failed_ = true;
		error_ = msg;
		if (error)
			*error = std::move(msg);
		return false;
	};

	if (!hasVideo_) {
		struct audio_output_info oi {};
		oi.name = name_.c_str();
		oi.speakers = SPEAKERS_STEREO;
		oi.samples_per_sec = 48000;
		oi.format = AUDIO_FORMAT_FLOAT_PLANAR;
		oi.input_param = this;
		oi.input_callback = onAudioInput;
		if (audio_output_open(&audio_, &oi) != AUDIO_OUTPUT_SUCCESS)
			return fail("the audio device did not open");
		if (!wav_.open(path_, oi.samples_per_sec, 2, 24)) {
			audio_output_close(audio_);
			audio_ = nullptr;
			return fail("could not write the WAV file");
		}
		if (!audio_output_connect(audio_, 0, nullptr, onPcm, this)) {
			audio_output_close(audio_);
			audio_ = nullptr;
			wav_.close();
			return fail("the audio device did not accept the recorder");
		}
		obs_source_inc_showing(source_);
		showingInced_ = true;
		codec_ = "pcm_s24le";
		started_ = true;
		return true;
	}

	struct obs_video_info ovi;
	if (!obs_get_video_info(&ovi))
		return fail("OBS has no video yet");
	uint32_t w = obs_source_get_width(source_);
	uint32_t h = obs_source_get_height(source_);
	if (w == 0 || h == 0)
		return fail("the source has no picture yet");
	w += (w & 1);
	h += (h & 1);

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
		return fail("could not make a private video output");

	obs_data_t *bg = obs_data_create();
	obs_data_set_int(bg, "color", kGreenScreenColor);
	obs_data_set_int(bg, "width", (long long)w);
	obs_data_set_int(bg, "height", (long long)h);
	background_ = obs_source_create_private(obs_get_latest_input_type_id("color_source"),
						name_.c_str(), bg);
	obs_data_release(bg);
	if (!background_)
		return fail("could not make the recording background");
	obs_view_set_source(view_, kBackgroundChannel, background_);
	obs_view_set_source(view_, kSourceChannel, source_);
	obs_source_inc_showing(background_);
	bgShowingInced_ = true;
	obs_source_inc_showing(source_);
	showingInced_ = true;

	if (!openVideoPipeline(videoEncoderId_)) {
		// A hardware encoder can refuse a source it cannot handle — most often
		// because the picture is smaller than its minimum frame size, which a
		// small avatar or overlay source easily is. The software encoder has no
		// such floor, so give the source one more chance before failing it.
		const std::string first = error_;
		closeVideoPipeline();
		if (videoEncoderId_ != "obs_x264" && openVideoPipeline("obs_x264")) {
			blog(LOG_WARNING,
			     "[iso-recorder] %s: the %s encoder would not take this source (%s); recording it with obs_x264 instead",
			     name_.c_str(), videoEncoderId_.c_str(), first.c_str());
			codec_ = "h264";
			return true;
		}
		const std::string second = error_;
		closeVideoPipeline();
		std::string msg = first;
		if (!second.empty() && second != first)
			msg += "; the software encoder also failed: " + second;
		return fail(std::move(msg));
	}
	codec_ = codecForEncoderId(videoEncoderId_);
	return true;
}

bool SourceRecorder::openVideoPipeline(const std::string &encoderId)
{
	error_.clear();

	videoEnc_ = obs_video_encoder_create(encoderId.c_str(), name_.c_str(), videoSettings_, nullptr);
	if (!videoEnc_) {
		error_ = "the encoder did not open";
		return false;
	}
	obs_encoder_set_video(videoEnc_, video_);

	obs_data_t *settings = obs_data_create();
	obs_data_set_string(settings, "path", path_.c_str());
	output_ = obs_output_create("mov_output", name_.c_str(), settings, nullptr);
	obs_data_release(settings);
	if (!output_) {
		error_ = "the container did not open";
		return false;
	}
	obs_output_set_video_encoder(output_, videoEnc_);

	// An OBS AV output refuses to start without both encoders, so even a
	// source with no sound gets an audio track; the tap writes silence.
	struct audio_output_info oi {};
	oi.name = name_.c_str();
	oi.speakers = SPEAKERS_STEREO;
	oi.samples_per_sec = audio_output_get_sample_rate(obs_get_audio());
	oi.format = AUDIO_FORMAT_FLOAT_PLANAR;
	oi.input_param = this;
	oi.input_callback = onAudioInput;
	if (audio_output_open(&audio_, &oi) != AUDIO_OUTPUT_SUCCESS) {
		error_ = "the audio device did not open";
		return false;
	}
	audioEnc_ = obs_audio_encoder_create("ffmpeg_aac", name_.c_str(), audioSettings_, 0, nullptr);
	if (!audioEnc_) {
		error_ = "the audio encoder did not open";
		return false;
	}
	obs_encoder_set_audio(audioEnc_, audio_);
	obs_output_set_audio_encoder(output_, audioEnc_, 0);

	if (!obs_output_start(output_)) {
		const char *last = obs_output_get_last_error(output_);
		error_ = last ? last : "";
		if (error_.empty())
			error_ = "the output refused to start";
		return false;
	}
	started_ = true;
	outputStarted_ = true;
	os_event_init(&stopEvent_, OS_EVENT_TYPE_AUTO);
	signal_handler_connect(obs_output_get_signal_handler(output_), "stop", onOutputStopped, this);
	return true;
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
	self->error_ = last ? last : "";
	if (self->error_.empty())
		self->error_ = "the recorder stopped early";
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
	if (self->failed_)
		return;
	if (!self->wav_.write(reinterpret_cast<const float *const *>(data->data), data->frames)) {
		self->failed_ = true;
		self->error_ = "could not write the WAV file";
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

void SourceRecorder::mixComposite(struct audio_output_data *mixes, size_t mixIdx, uint64_t *newTs)
{
	uint64_t minTs = 0;
	obs_source_enum_active_tree(source_, calcMinTs, &minTs);
	MixCtx ctx {};
	ctx.audio = audio_;
	ctx.minTs = minTs;
	for (size_t ch = 0; ch < MAX_AUDIO_CHANNELS; ++ch)
		ctx.target[ch] = mixes[mixIdx].data[ch];
	obs_source_enum_active_tree(source_, mixChild, &ctx);
	*newTs = minTs ? minTs : obs_source_get_audio_timestamp(source_);
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
	if (!composite) {
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
			mixComposite(mixes, mix, newTs);
		else
			mixPlain(mixes, mix);
	}
	if (*newTs == 0)
		*newTs = startTs;
	return true;
}

} // namespace iso
