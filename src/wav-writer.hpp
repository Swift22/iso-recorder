#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace iso {

std::vector<uint8_t> buildWavHeader(uint32_t dataBytes, uint32_t sampleRate, uint16_t channels,
				    uint16_t bitsPerSample, bool streaming = false);

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
