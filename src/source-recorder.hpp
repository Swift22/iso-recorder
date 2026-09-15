#pragma once

#include <obs.h>

#include "obs-events.h"

#include <cstdint>
#include <string>

#include "session-writer.hpp"
#include "wav-writer.hpp"

namespace iso {

class SourceRecorder {
public:
	SourceRecorder(obs_source_t *source, std::string name, std::string path,
		       std::string videoEncoderId, obs_data_t *videoSettings,
		       obs_data_t *audioSettings, uint32_t capWidth, uint32_t capHeight, Kind kind,
		       std::string audioCodec);
	~SourceRecorder();

	bool start(std::string *error);
	void stop();
	void setSource(obs_source_t *next);

	bool active() const { return started_; }
	bool failed() const { return failed_; }
	const std::string &error() const { return error_; }
	const std::string &sourceName() const { return name_; }
	const std::string &path() const { return path_; }
	const std::string &codec() const { return codec_; }
	uint64_t startOffsetNs() const { return startOffsetNs_; }
	double durationSeconds() const;
	// Video packets the output actually delivered; final once stop() has run.
	uint32_t totalFrames() const { return totalFrames_; }

	void noteStart(uint64_t epochNs, bool atSessionStart = false);

private:
	static void onPcm(void *param, size_t mixIdx, struct audio_data *data);
	static void onOutputStopped(void *param, calldata_t *calldata);
	static bool onAudioInput(void *param, uint64_t startTs, uint64_t endTs, uint64_t *newTs,
				 uint32_t activeMixers, struct audio_output_data *mixes);
	bool fillAudio(uint64_t startTs, uint64_t endTs, uint64_t *newTs, uint32_t activeMixers,
		       struct audio_output_data *mixes);
	void mixComposite(struct audio_output_data *mixes, size_t mixIdx, uint64_t *newTs);
	void mixPlain(struct audio_output_data *mixes, size_t mixIdx);
	bool openVideoPipeline(const std::string &encoderId);
	void closeVideoPipeline();
	bool isComposite() const
	{
		return hasVideo_ && (obs_source_get_output_flags(source_) & OBS_SOURCE_COMPOSITE);
	}

	obs_source_t *source_ = nullptr; // strong ref, released in dtor
	std::string name_;
	std::string path_;
	std::string codec_;
	Kind kind_;
	bool hasVideo_ = false;
	bool hasAudio_ = false;
	std::string videoEncoderId_;
	obs_data_t *videoSettings_ = nullptr; // owned copy
	obs_data_t *audioSettings_ = nullptr; // owned copy
	uint32_t capWidth_ = 0;               // recording-size cap; 0 means the source's own size
	uint32_t capHeight_ = 0;
	std::string audioCodec_;

	obs_view_t *view_ = nullptr;
	video_t *video_ = nullptr;
	audio_t *audio_ = nullptr;
	obs_source_t *background_ = nullptr; // strong ref, released in stop()
	WavWriter wav_;
	obs_encoder_t *videoEnc_ = nullptr;
	obs_encoder_t *audioEnc_ = nullptr;
	obs_output_t *output_ = nullptr;
	bool outputStarted_ = false;
	os_event_t *stopEvent_ = nullptr;

	uint64_t startNs_ = 0;
	uint64_t stopNs_ = 0;
	uint64_t epochNs_ = 0;
	uint64_t startOffsetNs_ = 0;
	uint32_t totalFrames_ = 0;
	bool started_ = false;
	bool failed_ = false;
	bool showingInced_ = false;
	bool bgShowingInced_ = false;
	std::string error_;
};

} // namespace iso
