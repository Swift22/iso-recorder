#pragma once

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace iso {

struct AudioCounts {
	uint64_t ticks = 0;       // ticks that carried the source's own audio
	uint64_t idleTicks = 0;   // ticks where the source had nothing, so the file got silence
	uint64_t silentTicks = 0; // real ticks whose samples were all exactly zero
	uint64_t longestSilentTicks = 0;
	uint64_t clipped = 0; // samples at or past full scale
	uint64_t jumps = 0;   // blocks repeated, skipped or out of step with the clock
	float peak = 0.0f;    // linear, 1.0 is full scale
	double maxGapMs = 0.0;
};

// What the recorded audio looked like, counted once per OBS audio tick (1024 frames, about
// 21 ms): one pass over a block that was just written, a few thousand adds. Only the
// OBS audio thread writes; any thread may read counts().
class AudioStats {
public:
	void idle()
	{
		idle_.fetch_add(1, std::memory_order_relaxed);
		last_ = 0; // a source coming back after a pause is not a jump
	}

	// A block the recorder had to make up (nothing queued) or throw away (queue full).
	void slip() { jumps_.fetch_add(1, std::memory_order_relaxed); }

	void block(const float *const *planes, size_t channels, size_t frames)
	{
		float peak = 0.0f;
		uint64_t clipped = 0;
		for (size_t ch = 0; ch < channels; ++ch) {
			const float *p = planes[ch];
			if (!p)
				continue;
			for (size_t f = 0; f < frames; ++f) {
				const float a = std::fabs(p[f]);
				peak = a > peak ? a : peak;
				clipped += a >= 0.999f;
			}
		}
		ticks_.fetch_add(1, std::memory_order_relaxed);
		clipped_.fetch_add(clipped, std::memory_order_relaxed);
		if (peak > peak_.load(std::memory_order_relaxed))
			peak_.store(peak, std::memory_order_relaxed);
		if (peak == 0.0f) {
			silent_.fetch_add(1, std::memory_order_relaxed);
			if (++run_ > longest_.load(std::memory_order_relaxed))
				longest_.store(run_, std::memory_order_relaxed);
		} else {
			run_ = 0;
		}
	}

	// A source's own audio clock moves exactly one tick per tick: no move is a repeated block,
	// two ticks a skipped one.
	void timestamp(uint64_t ts, uint64_t tickNs)
	{
		if (last_ != 0) {
			const uint64_t gap = ts > last_ ? ts - last_ : last_ - ts;
			if (ts <= last_ || gap < tickNs / 2 || gap > tickNs + tickNs / 2) {
				jumps_.fetch_add(1, std::memory_order_relaxed);
				const double ms = (double)gap / 1e6;
				if (ms > maxGapMs_.load(std::memory_order_relaxed))
					maxGapMs_.store(ms, std::memory_order_relaxed);
			}
		}
		last_ = ts;
	}

	AudioCounts counts() const
	{
		AudioCounts c;
		c.ticks = ticks_.load(std::memory_order_relaxed);
		c.idleTicks = idle_.load(std::memory_order_relaxed);
		c.silentTicks = silent_.load(std::memory_order_relaxed);
		c.longestSilentTicks = longest_.load(std::memory_order_relaxed);
		c.clipped = clipped_.load(std::memory_order_relaxed);
		c.jumps = jumps_.load(std::memory_order_relaxed);
		c.peak = peak_.load(std::memory_order_relaxed);
		c.maxGapMs = maxGapMs_.load(std::memory_order_relaxed);
		return c;
	}

private:
	std::atomic<uint64_t> ticks_{0}, idle_{0}, silent_{0}, longest_{0}, clipped_{0}, jumps_{0};
	std::atomic<float> peak_{0.0f};
	std::atomic<double> maxGapMs_{0.0};
	uint64_t run_ = 0;  // audio thread only
	uint64_t last_ = 0; // audio thread only
};

} // namespace iso
