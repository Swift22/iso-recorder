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

int main()
{
	const auto h = buildWavHeader(96000, 48000, 2, 24);
	CHECK(h.size() == 44);
	CHECK(std::memcmp(h.data(), "RIFF", 4) == 0);
	CHECK(std::memcmp(h.data() + 8, "WAVE", 4) == 0);
	CHECK(std::memcmp(h.data() + 12, "fmt ", 4) == 0);
	CHECK(std::memcmp(h.data() + 36, "data", 4) == 0);

	auto u32 = [&](size_t at) {
		return (uint32_t)h[at] | ((uint32_t)h[at + 1] << 8) | ((uint32_t)h[at + 2] << 16) |
		       ((uint32_t)h[at + 3] << 24);
	};
	auto u16 = [&](size_t at) {
		return (uint16_t)((uint16_t)h[at] | ((uint16_t)h[at + 1] << 8));
	};
	CHECK(u32(4) == 36u + 96000u);  // RIFF chunk size
	CHECK(u16(20) == 1);            // PCM
	CHECK(u16(22) == 2);            // channels
	CHECK(u32(24) == 48000u);       // sample rate
	CHECK(u32(28) == 48000u * 2u * 3u); // byte rate
	CHECK(u16(32) == 2u * 3u);      // block align
	CHECK(u16(34) == 24);           // bits per sample
	CHECK(u32(40) == 96000u);       // data size

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
			CHECK(n == 44 + 12);
			auto ru32 = [&](size_t at) {
				return (uint32_t)buf[at] | ((uint32_t)buf[at + 1] << 8) |
				       ((uint32_t)buf[at + 2] << 16) | ((uint32_t)buf[at + 3] << 24);
			};
			CHECK(ru32(4) == 36u + 12u);
			CHECK(ru32(40) == 12u);
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
	{
		const char *overflowPath = "test_wav_writer_overflow_tmp.wav";
		WavWriter w;
		CHECK(w.open(overflowPath, 48000, 1, 24));
		// 2^32 frames * 1 channel * 3 bytes exceeds the 4 GiB RIFF field; the
		// guard must fail before allocating, and close must then report failure.
		CHECK(!w.write(nullptr, (size_t)0x100000000ull));
		CHECK(!w.close());
		std::remove(overflowPath);
	}

	if (failures == 0)
		std::printf("wav-writer: all checks passed\n");
	return failures == 0 ? 0 : 1;
}
