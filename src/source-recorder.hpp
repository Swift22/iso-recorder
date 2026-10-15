#pragma once

#include <obs.h>

#include "obs-events.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

#include "session-writer.hpp"
#include "wav-writer.hpp"

namespace iso {

class SourceRecorder {
public:
	SourceRecorder(obs_source_t *source, std::string name, std::string path, std::string videoEncoderId,
		       obs_data_t *videoSettings, obs_data_t *audioSettings, uint32_t capWidth, uint32_t capHeight);
	~SourceRecorder();

	bool start(std::string *error);
	// Writes packets from encoders that are already running, such as the stream's,
	// so the recording adds no encode of its own.
	bool startShared(obs_encoder_t *video, obs_encoder_t *audio, std::string *error);
	void stop();
	void setSource(obs_source_t *next);
	// The source no longer matches the size its private view was built for, so the
	// picture is being cropped or padded.
	bool sizeChanged() const;

	bool active() const { return started_; }
	bool failed() const { return failed_; }
	// OBS calls the stop and PCM callbacks on its own threads, so the state they set
	// is atomic and the message is read under a lock.
	std::string error() const
	{
		std::lock_guard<std::mutex> lock(errorMutex_);
		return error_;
	}
	const std::string &codec() const { return codec_; }
	uint64_t startOffsetNs() const;
	double durationSeconds() const;
	// Video packets the output actually delivered; final once stop() has run.
	uint32_t totalFrames() const { return totalFrames_; }

	void noteStart(uint64_t epochNs, bool atSessionStart = false);

private:
	static void onPcm(void *param, size_t mixIdx, struct audio_data *data);
	static void onOutputStopped(void *param, calldata_t *calldata);
	static void onPacket(obs_output_t *output, struct encoder_packet *pkt, struct encoder_packet_time *pktTime,
			     void *param);
	static bool onAudioInput(void *param, uint64_t startTs, uint64_t endTs, uint64_t *newTs,
				 uint32_t activeMixers, struct audio_output_data *mixes);
	bool fillAudio(uint64_t startTs, uint64_t endTs, uint64_t *newTs, uint32_t activeMixers,
		       struct audio_output_data *mixes);
	void mixComposite(struct audio_output_data *mixes, size_t mixIdx, uint64_t minTs);
	void mixPlain(struct audio_output_data *mixes, size_t mixIdx);
	// Marks the recorder failed with msg and copies it to *error; always false.
	void setError(std::string msg);
	bool fail(std::string msg, std::string *error);
	bool openAudio(uint32_t samplesPerSec);
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
	bool hasVideo_ = false;
	bool hasAudio_ = false;
	std::string videoEncoderId_;
	obs_data_t *videoSettings_ = nullptr; // owned copy
	obs_data_t *audioSettings_ = nullptr; // owned copy
	uint32_t capWidth_ = 0;               // recording-size cap; 0 means the source's own size
	uint32_t capHeight_ = 0;
	uint32_t baseWidth_ = 0; // the even-rounded source size the view was built for
	uint32_t baseHeight_ = 0;

	obs_view_t *view_ = nullptr;
	video_t *video_ = nullptr;
	audio_t *audio_ = nullptr;
	obs_source_t *background_ = nullptr; // strong ref, released in stop()
	WavWriter wav_;
	obs_encoder_t *videoEnc_ = nullptr;
	obs_encoder_t *audioEnc_ = nullptr;
	obs_output_t *output_ = nullptr;
	bool outputStarted_ = false;
	bool shared_ = false;
	// A shared encoder is mid-stream, so the file begins at its next keyframe, not at start().
	std::atomic<uint64_t> firstFrameNs_{0};
	os_event_t *stopEvent_ = nullptr;

	uint64_t startNs_ = 0;
	std::atomic<uint64_t> stopNs_{0};
	uint64_t epochNs_ = 0;
	uint64_t startOffsetNs_ = 0;
	uint32_t totalFrames_ = 0;
	std::atomic<bool> started_{false};
	std::atomic<bool> failed_{false};
	bool showingInced_ = false;
	bool bgShowingInced_ = false;
	mutable std::mutex errorMutex_;
	std::string error_;
};

} // namespace iso
