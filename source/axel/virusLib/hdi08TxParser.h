#pragma once

#include "synthLib/midiTypes.h"
#include "dsp56kEmu/types.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace virusLib
{
	class Microcontroller;

	struct BpmLedDiagnostics
	{
		enum Counter : size_t
		{
			StatusMessages, CompletedStatusMessages, RisingEdges, FallingEdges, ProcessSamples,
			LastIntervalTransitions, MaxIntervalTransitions, MultipleTransitionIntervals,
			CoveredOnPulses, SampledRisingEdges, SampledFallingEdges, Count
		};
		bool enabled = false;
		std::array<uint64_t, Count> counters{};
	};

	class Hdi08TxParser
	{
	public:
		enum class State
		{
			Default,
			Sysex,
			Preset,
			StatusReport
		};

		enum class PatternType
		{
			DspBoot,

			Count
		};

		Hdi08TxParser(Microcontroller& _mc) : m_mc(_mc)
		{
			m_patternPositions.fill(0);
		}

		bool append(dsp56k::TWord _data);
		const std::vector<synthLib::SMidiEvent>& getMidiData() const { return m_midiData; }
		void clearMidiData() { m_midiData.clear(); }
		bool getBpmLedOn() const { return m_bpmLedOn; }
		// Called under the existing Microcontroller mutex, never directly from the GUI.
		void setBpmLedDiagnosticsEnabled(bool _enabled);
		void sampleBpmLedDiagnostics();
		const BpmLedDiagnostics& getBpmLedDiagnostics() const { return m_bpmDiagnostics; }
		void waitForPreset(uint32_t _byteCount);

		bool waitingForPreset() const
		{
			return m_remainingPresetBytes != 0;
		}

		bool hasDspBooted() const
		{
			return m_dspHasBooted;
		}

		void getPresetData(std::vector<uint8_t>& _data);

	private:
		bool beginPreset(dsp56k::TWord _data);
		bool appendUsbMidi(dsp56k::TWord _data);
		bool processUsbMidiPacket();
		bool finishSysex();
		void recordBpmLedStatus(bool _on);

		Microcontroller& m_mc;

		std::vector<synthLib::SMidiEvent> m_midiData;
		synthLib::SysexBuffer m_sysexData;
		std::vector<uint8_t> m_presetData;
		std::vector<dsp56k::TWord> m_dspStatus;

		// The A, B and C: MIDI out with running status
		uint8_t m_midiStatus = 0;
		std::array<uint8_t, 2> m_midiBytes{};
		uint32_t m_midiCount = 0;

		uint32_t m_remainingPresetBytes = 0;
		uint32_t m_remainingStatusBytes = 0;

		std::array<uint8_t, 4> m_usbPacket{};
		uint32_t m_usbPacketSize = 0;

		State m_state = State::Default;
		State m_stateAfterPreset = State::Default;

		std::vector<PatternType> m_matchedPatterns;
		std::vector<dsp56k::TWord> m_nonPatternWords;

		std::array<size_t, static_cast<size_t>(PatternType::Count)> m_patternPositions = {};

		bool m_dspHasBooted = false;
		bool m_bpmLedOn = false;
		BpmLedDiagnostics m_bpmDiagnostics;
		uint64_t m_bpmIntervalTransitions = 0;
		bool m_bpmUnobservedOn = false;
		bool m_bpmSampledOn = false;
	};
}
