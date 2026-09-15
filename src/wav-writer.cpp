#include "wav-writer.hpp"

#include <cmath>

namespace iso {

static void putU32(std::vector<uint8_t> &v, uint32_t x)
{
	v.push_back((uint8_t)(x & 0xff));
	v.push_back((uint8_t)((x >> 8) & 0xff));
	v.push_back((uint8_t)((x >> 16) & 0xff));
	v.push_back((uint8_t)((x >> 24) & 0xff));
}

static void putU16(std::vector<uint8_t> &v, uint16_t x)
{
	v.push_back((uint8_t)(x & 0xff));
	v.push_back((uint8_t)((x >> 8) & 0xff));
}

static void putTag(std::vector<uint8_t> &v, const char *tag)
{
	v.insert(v.end(), tag, tag + 4);
}

std::vector<uint8_t> buildWavHeader(uint32_t dataBytes, uint32_t sampleRate, uint16_t channels,
				    uint16_t bitsPerSample, bool streaming)
{
	const uint16_t blockAlign = (uint16_t)(channels * bitsPerSample / 8);
	std::vector<uint8_t> v;
	v.reserve(44);
	// 0xFFFFFFFF is the RIFF "unknown length" convention, so a force-killed file still opens.
	putTag(v, "RIFF");
	putU32(v, streaming ? 0xFFFFFFFFu : 36u + dataBytes);
	putTag(v, "WAVE");
	putTag(v, "fmt ");
	putU32(v, 16);
	putU16(v, 1);
	putU16(v, channels);
	putU32(v, sampleRate);
	putU32(v, sampleRate * blockAlign);
	putU16(v, blockAlign);
	putU16(v, bitsPerSample);
	putTag(v, "data");
	putU32(v, streaming ? 0xFFFFFFFFu : dataBytes);
	return v;
}

bool WavWriter::open(const std::string &path, uint32_t sampleRate, uint16_t channels,
		     uint16_t bitsPerSample)
{
	sampleRate_ = sampleRate;
	channels_ = channels;
	bits_ = bitsPerSample;
	dataBytes_ = 0;
	failed_ = false;
	file_ = std::fopen(path.c_str(), "wb");
	if (!file_)
		return false;
	const auto header = buildWavHeader(0, sampleRate, channels, bits_, true);
	if (std::fwrite(header.data(), 1, header.size(), file_) != header.size()) {
		std::fclose(file_);
		file_ = nullptr;
		return false;
	}
	return true;
}

bool WavWriter::write(const float *const *planar, size_t frames)
{
	if (!file_)
		return false;
	const uint64_t bytes = (uint64_t)frames * channels_ * 3u;
	// Keep both the data field and the 36-byte-preceded RIFF field inside uint32.
	if (bytes + dataBytes_ + 36u > 0xFFFFFFFFull) {
		failed_ = true;
		return false;
	}
	/* interleave to 24-bit little-endian PCM */
	std::vector<uint8_t> out((size_t)bytes);
	size_t k = 0;
	for (size_t f = 0; f < frames; ++f) {
		for (uint16_t c = 0; c < channels_; ++c, k += 3) {
			int32_t s = (int32_t)std::lrint(std::fmin(1.0, std::fmax(-1.0, planar[c][f])) *
						       8388607.0);
			out[k] = (uint8_t)(s & 0xff);
			out[k + 1] = (uint8_t)((s >> 8) & 0xff);
			out[k + 2] = (uint8_t)((s >> 16) & 0xff);
		}
	}
	const size_t written = std::fwrite(out.data(), 1, out.size(), file_);
	dataBytes_ += written;
	if (written != out.size()) {
		failed_ = true;
		return false;
	}
	return true;
}

bool WavWriter::close()
{
	if (!file_)
		return false;
	const auto header = buildWavHeader((uint32_t)dataBytes_, sampleRate_, channels_, bits_);
	bool ok = !failed_;
	if (std::fseek(file_, 0, SEEK_SET) != 0)
		ok = false;
	if (std::fwrite(header.data(), 1, header.size(), file_) != header.size())
		ok = false;
	if (std::fclose(file_) != 0)
		ok = false;
	file_ = nullptr;
	return ok;
}

} // namespace iso
