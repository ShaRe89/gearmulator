#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace virusLib
{
// Part 1 only. All calls are protected by Microcontroller::m_mutex.
class Part1DirtyState
{
public:
	enum class Status { Unknown, Clean, Dirty };
	static constexpr size_t PayloadSize = 256;

	void invalidate()
	{
		m_valid = false;
		m_pendingLoad = 0;
		if (++m_generation == 0)
			++m_generation;
	}

	uint64_t beginLoad()
	{
		invalidate();
		m_pendingLoad = m_generation;
		return m_pendingLoad;
	}

	void completeLoad(const uint64_t _load, const uint8_t* _normalized, const size_t _size)
	{
		if (!_load || _load != m_pendingLoad)
			return;
		m_pendingLoad = 0;
		if (!_normalized || _size != PayloadSize)
			return;
		std::copy_n(_normalized, PayloadSize, m_reference.begin());
		m_valid = true;
	}

	Status status(const uint8_t* _current, const size_t _size, const bool _uncertain) const
	{
		if (!m_valid || m_pendingLoad || _uncertain || !_current || _size < PayloadSize)
			return Status::Unknown;
		return std::equal(m_reference.begin(), m_reference.end(), _current) ? Status::Clean : Status::Dirty;
	}

private:
	std::array<uint8_t, PayloadSize> m_reference{};
	bool m_valid = false;
	uint64_t m_generation = 0;
	uint64_t m_pendingLoad = 0;
};
}
