#include "wav-writer.hpp"

#include <cstdio>
#include <cstring>

using namespace iso;

static int failures = 0;

#define CHECK(cond)                                                                      \
	do {                                                                             \
		if (!(cond)) {                                                           \
			std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
			++failures;                                                      \
		}                                                                        \
	} while (0)

// The header carries a 36-byte placeholder chunk between "WAVE" and "fmt ", so every
// offset below sits 36 bytes later than in a plain 44-byte WAV header.
int main()
{
	const auto h = buildWavHeader(96000, 48000, 2, 24);
	CHECK(h.size() == 80);
	CHECK(std::memcmp(h.data(), "RIFF", 4) == 0);
	CHECK(std::memcmp(h.data() + 8, "WAVE", 4) == 0);
	CHECK(std::memcmp(h.data() + 12, "JUNK", 4) == 0);
	CHECK(std::memcmp(h.data() + 48, "fmt ", 4) == 0);
	CHECK(std::memcmp(h.data() + 72, "data", 4) == 0);

	auto u32 = [&](size_t at) {
		return (uint32_t)h[at] | ((uint32_t)h[at + 1] << 8) | ((uint32_t)h[at + 2] << 16) |
		       ((uint32_t)h[at + 3] << 24);
	};
	auto u16 = [&](size_t at) {
		return (uint16_t)((uint16_t)h[at] | ((uint16_t)h[at + 1] << 8));
	};
	CHECK(u32(4) == 72u + 96000u);      // RIFF chunk size
	CHECK(u32(16) == 28u);              // placeholder chunk size
	CHECK(u32(20) == 0u);               // unused while the sizes still fit in 32 bits
	CHECK(u16(56) == 1);                // PCM
	CHECK(u16(58) == 2);                // channels
	CHECK(u32(60) == 48000u);           // sample rate
	CHECK(u32(64) == 48000u * 2u * 3u); // byte rate
	CHECK(u16(68) == 2u * 3u);          // block align
	CHECK(u16(70) == 24);               // bits per sample
	CHECK(u32(76) == 96000u);           // data size

	{
		// While recording, both sizes are the "unknown length" convention so a
		// force-killed file still opens.
		const auto s = buildWavHeader(0, 48000, 2, 24, true);
		CHECK(s.size() == 80);
		auto su32 = [&](size_t at) {
			return (uint32_t)s[at] | ((uint32_t)s[at + 1] << 8) | ((uint32_t)s[at + 2] << 16) |
			       ((uint32_t)s[at + 3] << 24);
		};
		CHECK(std::memcmp(s.data(), "RIFF", 4) == 0);
		CHECK(su32(4) == 0xFFFFFFFFu);
		CHECK(su32(76) == 0xFFFFFFFFu);
	}

	{
		// Past 4 GiB the file becomes RF64 and the placeholder becomes "ds64",
		// which carries the real sizes as 64-bit numbers. 24-bit stereo at 48 kHz
		// reaches this in about 4 hours 8 minutes, which a long stream passes.
		const uint64_t data = 5ull * 1024 * 1024 * 1024;
		const auto b = buildWavHeader(data, 48000, 2, 24);
		CHECK(b.size() == 80);
		CHECK(std::memcmp(b.data(), "RF64", 4) == 0);
		CHECK(std::memcmp(b.data() + 12, "ds64", 4) == 0);
		auto bu32 = [&](size_t at) {
			return (uint32_t)b[at] | ((uint32_t)b[at + 1] << 8) | ((uint32_t)b[at + 2] << 16) |
			       ((uint32_t)b[at + 3] << 24);
		};
		auto bu64 = [&](size_t at) {
			return (uint64_t)bu32(at) | ((uint64_t)bu32(at + 4) << 32);
		};
		CHECK(bu32(4) == 0xFFFFFFFFu);   // RIFF size says "look in ds64"
		CHECK(bu32(76) == 0xFFFFFFFFu);  // and so does the data chunk size
		CHECK(bu64(20) == 72ull + data); // ds64 riff size
		CHECK(bu64(28) == data);         // ds64 data size
		CHECK(bu64(36) == data / 6ull);  // ds64 sample count, 6 bytes a frame
		CHECK(bu32(44) == 0u);           // no table entries
	}

	const char *tmpPath = "test_wav_writer_tmp.wav";
	{
		WavWriter w;
		CHECK(w.open(tmpPath, 48000, 2, 24));
		const float left[2] = {0.0f, 0.5f};
		const float right[2] = {-0.5f, 1.0f};
		const float *planar[2] = {left, right};
		CHECK(w.write(planar, 2));
		CHECK(w.close());
	}
	{
		std::FILE *f = std::fopen(tmpPath, "rb");
		CHECK(f != nullptr);
		if (f) {
			uint8_t buf[256];
			const size_t n = std::fread(buf, 1, sizeof(buf), f);
			CHECK(n == 80 + 12);
			auto ru32 = [&](size_t at) {
				return (uint32_t)buf[at] | ((uint32_t)buf[at + 1] << 8) |
				       ((uint32_t)buf[at + 2] << 16) | ((uint32_t)buf[at + 3] << 24);
			};
			CHECK(std::memcmp(buf, "RIFF", 4) == 0);
			CHECK(ru32(4) == 72u + 12u);
			CHECK(ru32(76) == 12u);
			std::fclose(f);
		}
		std::remove(tmpPath);
	}
	{
		WavWriter w;
		CHECK(!w.open("/no/such/dir/iso_wav_missing/out.wav", 48000, 2, 24));
		const float l[1] = {0.0f};
		const float *planar[1] = {l};
		CHECK(!w.write(planar, 1));
		CHECK(!w.close());
	}

	if (failures == 0)
		std::printf("wav-writer: all checks passed\n");
	return failures == 0 ? 0 : 1;
}
