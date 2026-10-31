#include "audio-stats.hpp"
#include "block-queue.hpp"
#include "session-writer.hpp"

#include <cstdio>
#include <vector>

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
	const uint64_t tick = 21333333; // 1024 frames at 48 kHz
	std::vector<float> quiet(1024, 0.0f), loud(1024, 0.5f), hot(1024, 1.0f);
	const float *silentPlanes[2] = {quiet.data(), quiet.data()};
	const float *loudPlanes[2] = {loud.data(), loud.data()};
	const float *hotPlanes[2] = {hot.data(), nullptr};

	AudioStats s;
	s.block(silentPlanes, 2, 1024);
	s.block(silentPlanes, 2, 1024);
	s.block(loudPlanes, 2, 1024);
	s.block(silentPlanes, 2, 1024);
	s.block(hotPlanes, 2, 1024);
	s.idle();
	AudioCounts c = s.counts();
	CHECK(c.ticks == 5);
	CHECK(c.idleTicks == 1);
	CHECK(c.silentTicks == 3);
	CHECK(c.longestSilentTicks == 2);
	CHECK(c.clipped == 1024); // the null plane is skipped, not counted
	CHECK(c.peak == 1.0f);

	AudioStats t;
	t.timestamp(1000 * tick, tick);
	t.timestamp(1001 * tick, tick);
	t.timestamp(1002 * tick + tick / 4, tick); // rounding inside half a tick is normal
	CHECK(t.counts().jumps == 0);
	t.timestamp(1002 * tick + tick / 4, tick); // the same block again
	t.timestamp(1004 * tick + tick / 4, tick); // one block skipped
	CHECK(t.counts().jumps == 2);
	t.idle();
	t.timestamp(1010 * tick, tick); // back after a pause: not a jump
	t.timestamp(1005 * tick, tick); // went backwards
	CHECK(t.counts().jumps == 3);
	CHECK(t.counts().maxGapMs > 100.0);
	t.slip();
	CHECK(t.counts().jumps == 4);

	// Every block pushed comes out once, in order; the first pops wait for two blocks.
	BlockQueue q;
	BlockQueue::Block in{}, out{};
	CHECK(q.pop(out) == BlockQueue::Pop::Priming);
	in.ts = 1;
	CHECK(q.push(in));
	CHECK(q.pop(out) == BlockQueue::Pop::Priming);
	in.ts = 2;
	q.push(in);
	CHECK(q.pop(out) == BlockQueue::Pop::Ok && out.ts == 1);
	CHECK(q.pop(out) == BlockQueue::Pop::Ok && out.ts == 2);
	CHECK(q.pop(out) == BlockQueue::Pop::Empty);
	bool dropped = false;
	for (uint64_t i = 10; i < 10 + BlockQueue::kCapacity + 1; ++i) {
		in.ts = i;
		dropped |= !q.push(in);
	}
	CHECK(dropped);
	CHECK(q.pop(out) == BlockQueue::Pop::Ok && out.ts == 11); // the oldest went

	Recording r;
	r.source = "Mic/Aux";
	r.kind = Kind::Audio;
	AudioHealth a;
	a.sampleRate = 48000;
	a.device = "default";
	a.filters = {"Noise Suppression [noise_suppress_filter_v2]"};
	a.flags = {"follows the system's default input, which can change under it"};
	r.audio = a;
	const std::string json = buildSessionJson(SessionInfo{}, {r});
	CHECK(json.find("\"audio\": { \"sampleRate\": 48000") != std::string::npos);
	CHECK(json.find("\"device\": \"default\"") != std::string::npos);
	CHECK(json.find("\"peakDb\": null") != std::string::npos);
	CHECK(json.find("Noise Suppression [noise_suppress_filter_v2]") != std::string::npos);

	Recording plain;
	CHECK(buildSessionJson(SessionInfo{}, {plain}).find("\"audio\"") == std::string::npos);

	return failures ? 1 : 0;
}
