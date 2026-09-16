#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace iso {

// The header is always 80 bytes: a plain WAV one with a 36-byte placeholder chunk after
// "WAVE". A WAV's sizes are 32-bit, which 24-bit stereo at 48 kHz fills in 4 hours 8
// minutes — inside one stream. Past that the placeholder becomes the "ds64" chunk of an
// RF64 file, which carries 64-bit sizes and is what Resolve, Reaper and ffmpeg read.
// While recording, the sizes are the "unknown length" 0xFFFFFFFF so a force-killed file
// still opens.
std::vector<uint8_t> buildWavHeader(uint64_t dataBytes, uint32_t sampleRate, uint16_t channels, uint16_t bitsPerSample,
				    bool streaming = false);

constexpr size_t kWavHeaderBytes = 80;

class WavWriter {
public:
	bool open(const std::string &path, uint32_t sampleRate, uint16_t channels,
		  uint16_t bitsPerSample);
	bool write(const float *const *planar, size_t frames);
	bool close();

private:
	std::FILE *file_ = nullptr;
	uint64_t dataBytes_ = 0;
	uint32_t sampleRate_ = 48000;
	uint16_t channels_ = 2;
	uint16_t bits_ = 24;
	bool failed_ = false;
};

} // namespace iso
