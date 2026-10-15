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

static void putU64(std::vector<uint8_t> &v, uint64_t x)
{
	putU32(v, (uint32_t)(x & 0xffffffffull));
	putU32(v, (uint32_t)((x >> 32) & 0xffffffffull));
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

// Everything in the file that is not sample data: the two 12-byte chunk headers, the
// placeholder chunk, and the fmt chunk. The RIFF size field counts all of it bar the
// first 8 bytes.
static constexpr uint64_t kNonDataBytes = kWavHeaderBytes - 8;

std::vector<uint8_t> buildWavHeader(uint64_t dataBytes, uint32_t sampleRate, uint16_t channels, uint16_t bitsPerSample,
				    bool streaming)
{
	const uint16_t blockAlign = (uint16_t)(channels * bitsPerSample / 8);
	const uint64_t riffBytes = kNonDataBytes + dataBytes;
	const bool big = riffBytes > 0xFFFFFFFFull;

	std::vector<uint8_t> v;
	v.reserve(kWavHeaderBytes);
	putTag(v, big ? "RF64" : "RIFF");
	putU32(v, (streaming || big) ? 0xFFFFFFFFu : (uint32_t)riffBytes);
	putTag(v, "WAVE");

	// 28 bytes either way: "ds64" once the file outgrew 32-bit sizes, otherwise "JUNK",
	// which every reader skips. Reserving it while recording is what lets a file that
	// turns out to be huge become RF64 without rewriting the samples.
	putTag(v, big ? "ds64" : "JUNK");
	putU32(v, 28);
	putU64(v, big ? riffBytes : 0);
	putU64(v, big ? dataBytes : 0);
	putU64(v, big && blockAlign ? dataBytes / blockAlign : 0);
	putU32(v, 0); // table length: no chunks beyond data need 64-bit sizes

	putTag(v, "fmt ");
	putU32(v, 16);
	putU16(v, 1);
	putU16(v, channels);
	putU32(v, sampleRate);
	putU32(v, sampleRate * blockAlign);
	putU16(v, blockAlign);
	putU16(v, bitsPerSample);

	putTag(v, "data");
	putU32(v, (streaming || big) ? 0xFFFFFFFFu : (uint32_t)dataBytes);
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
	// A recording long enough to overflow this is longer than any computer's uptime;
	// the 4 GiB ceiling that used to stop a stream at four hours is gone with RF64.
	if (bytes > (uint64_t)SIZE_MAX || dataBytes_ + bytes < dataBytes_) {
		failed_ = true;
		return false;
	}
	/* interleave to 24-bit little-endian PCM; the buffer is reused so the audio thread does not allocate */
	std::vector<uint8_t> &out = buffer_;
	out.resize((size_t)bytes);
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
	const auto header = buildWavHeader(dataBytes_, sampleRate_, channels_, bits_);
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
