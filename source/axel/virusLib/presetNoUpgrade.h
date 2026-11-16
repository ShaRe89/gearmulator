#pragma once

#include <cstdint>

namespace virusLib
{
// A no-upgrade reply belongs only to the outstanding, explicitly armed load.
class PresetNoUpgrade
{
public:
	void arm(uint64_t _load) { m_pending = _load; m_confirmed = 0; }
	void acknowledge(bool _waiting)
	{
		m_confirmed = _waiting ? m_pending : 0;
		m_pending = 0;
	}
	void upgraded() { arm(0); }
	uint64_t take()
	{
		const auto load = m_confirmed;
		m_confirmed = 0;
		return load;
	}
private:
	uint64_t m_pending = 0;
	uint64_t m_confirmed = 0;
};
}
