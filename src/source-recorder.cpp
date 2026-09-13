#include "source-recorder.hpp"

#include <util/platform.h>

#include <cerrno>
#include <cmath>
#include <utility>

namespace iso {

SourceRecorder::SourceRecorder(obs_source_t *source, std::string name, std::string path,
			       std::string videoEncoderId, obs_data_t *videoSettings, Kind kind,
			       std::string audioCodec)
	: source_(obs_source_get_ref(source)), name_(std::move(name)), path_(std::move(path)),
	  kind_(kind), videoEncoderId_(std::move(videoEncoderId)),
	  audioCodec_(std::move(audioCodec))
{
	const uint32_t flags = obs_source_get_output_flags(source_);
	hasVideo_ = (flags & OBS_SOURCE_VIDEO) != 0;
	hasAudio_ = (flags & (OBS_SOURCE_AUDIO | OBS_SOURCE_COMPOSITE)) != 0;
	if (videoSettings) {
		const char *json = obs_data_get_json(videoSettings);
		if (json)
			videoSettings_ = obs_data_create_from_json(json);
	}
}

SourceRecorder::~SourceRecorder()
{
	stop();
	if (videoSettings_)
		obs_data_release(videoSettings_);
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
	ovi.base_width = w;
	ovi.base_height = h;
	ovi.output_width = w;
	ovi.output_height = h;

	view_ = obs_view_create();
	video_ = obs_view_add2(view_, &ovi);
	if (!video_)
		return fail("could not make a private video output");
	obs_view_set_source(view_, 0, source_);
	obs_source_inc_showing(source_);
	showingInced_ = true;

	videoEnc_ = obs_video_encoder_create(videoEncoderId_.c_str(), name_.c_str(),
					     videoSettings_, nullptr);
	if (!videoEnc_)
		return fail("the encoder did not open");
	obs_encoder_set_video(videoEnc_, video_);

	obs_data_t *settings = obs_data_create();
	obs_data_set_string(settings, "path", path_.c_str());
	output_ = obs_output_create("mov_output", name_.c_str(), settings, nullptr);
	obs_data_release(settings);
	if (!output_)
		return fail("the container did not open");
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
	if (audio_output_open(&audio_, &oi) != AUDIO_OUTPUT_SUCCESS)
		return fail("the audio device did not open");
	obs_data_t *as = obs_data_create();
	audioEnc_ = obs_audio_encoder_create("ffmpeg_aac", name_.c_str(), as, 0, nullptr);
	obs_data_release(as);
	if (!audioEnc_)
		return fail("the audio encoder did not open");
	obs_encoder_set_audio(audioEnc_, audio_);
	obs_output_set_audio_encoder(output_, audioEnc_, 0);
	if (!obs_output_start(output_)) {
		const char *last = obs_output_get_last_error(output_);
		std::string msg = last ? last : "";
		if (msg.empty())
			msg = "the output refused to start";
		return fail(std::move(msg));
	}
	started_ = true;
	outputStarted_ = true;
	os_event_init(&stopEvent_, OS_EVENT_TYPE_AUTO);
	signal_handler_connect(obs_output_get_signal_handler(output_), "stop", onOutputStopped, this);
	codec_ = codecForEncoderId(videoEncoderId_);
	return true;
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
			if (stopEvent_ && os_event_timedwait(stopEvent_, 5000) == ETIMEDOUT)
				blog(LOG_WARNING,
				     "[iso-recorder] %s: the recorder did not report stopping within 5 seconds",
				     name_.c_str());
			signal_handler_disconnect(obs_output_get_signal_handler(output_), "stop",
						  onOutputStopped, this);
		}
		obs_output_release(output_);
		output_ = nullptr;
	}
	outputStarted_ = false;
	if (stopEvent_) {
		os_event_destroy(stopEvent_);
		stopEvent_ = nullptr;
	}
	if (audioEnc_)
		obs_encoder_release(audioEnc_);
	if (videoEnc_)
		obs_encoder_release(videoEnc_);
	audioEnc_ = nullptr;
	videoEnc_ = nullptr;
	if (showingInced_) {
		obs_source_dec_showing(source_);
		showingInced_ = false;
	}
	if (view_) {
		obs_view_remove(view_);
		obs_view_destroy(view_);
		view_ = nullptr;
		video_ = nullptr;
	}
	if (audio_) {
		if (!hasVideo_)
			audio_output_disconnect(audio_, 0, onPcm, this);
		audio_output_close(audio_);
		audio_ = nullptr;
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
	obs_view_set_source(view_, 0, source_);
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
