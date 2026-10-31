#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>

namespace iso {

// Hands audio from OBS's audio thread to the recorder's own audio thread, one OBS tick at a time.
// Both threads tick at the same rate but on unrelated schedules, so reading OBS's per-source buffer
// straight from the recorder's thread sometimes read one block twice or missed one (a click every
// 21 ms for a whole session). Queued, every block is taken exactly once; the queue only absorbs
// the jitter between the two ticks.
class BlockQueue {
public:
	static constexpr size_t kChannels = 2;
	static constexpr size_t kFrames = 1024; // AUDIO_OUTPUT_FRAMES
	static constexpr size_t kCapacity = 8;  // about 170 ms
	// Blocks held back before the first pop, so a recorder tick that lands just before OBS's
	// tick still finds one waiting.
	static constexpr size_t kPrime = 2;

	struct Block {
		uint64_t ts = 0; // the source's audio timestamp; 0 while the source had nothing
		float data[kChannels][kFrames];
	};

	enum class Pop { Priming, Empty, Ok };

	// False when the queue was full and its oldest block was dropped to make room.
	bool push(const Block &b)
	{
		std::lock_guard<std::mutex> lock(mutex_);
		bool kept = true;
		if (size_ == kCapacity) {
			head_ = (head_ + 1) % kCapacity;
			--size_;
			kept = false;
		}
		ring_[(head_ + size_) % kCapacity] = b;
		++size_;
		if (size_ >= kPrime)
			primed_ = true;
		return kept;
	}

	Pop pop(Block &out)
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (!primed_)
			return Pop::Priming;
		if (size_ == 0)
			return Pop::Empty;
		out = ring_[head_];
		head_ = (head_ + 1) % kCapacity;
		--size_;
		return Pop::Ok;
	}

	void clear()
	{
		std::lock_guard<std::mutex> lock(mutex_);
		head_ = size_ = 0;
		primed_ = false;
	}

private:
	std::mutex mutex_;
	Block ring_[kCapacity];
	size_t head_ = 0;
	size_t size_ = 0;
	bool primed_ = false;
};

} // namespace iso
