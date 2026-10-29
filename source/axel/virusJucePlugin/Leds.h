#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>

#include "baseLib/event.h"

#include "jucePluginEditorLib/led.h"

#include "virusLib/frontpanelState.h"

namespace juce
{
	class MouseListener;
}

namespace virus
{
	class VirusProcessor;
}

namespace genericUI
{
	class Editor;
}

namespace genericVirusUI
{
	class VirusEditor;

	class Leds
	{
	public:
		Leds(const VirusEditor& _editor, virus::VirusProcessor& _processor);
		~Leds();

		void toggleLogoAnimation();

		bool supportsLogoAnimation() const { return m_logoLed.get(); }
		bool isLogoAnimationEnabled() const { return m_logoAnimationEnabled; }

		void setBpmDiagnosticsEnabled(bool _enabled);
		std::string getBpmDiagnosticsReport() const;

	private:
		void onFrontPanelStateChanged(const virusLib::FrontpanelState& _frontpanelState);
		void onBpmFrameGenerated();
		void setBpmDisplayState(bool _on);
		void addLogoClickListener(Rml::Element* _logo);

		virus::VirusProcessor& m_processor;
		juceRmlUi::RmlComponent& m_rmlComponent;
		Rml::Element* m_bpmElement = nullptr;
		bool m_bpmState = false;
		bool m_bpmDisplayState = false;
		bool m_bpmFramePending = false;
		std::deque<bool> m_bpmTransitions;
		// Access is serialized by RmlInterfaces::ScopedAccess; never used by the audio thread.
		struct BpmDiagnostics
		{
			bool enabled = false;
			bool initialOn = false;
			bool pulseOpen = false;
			bool pulseHadOnFrame = false;
			bool pulseHadUnknownFrame = false;
			uint64_t onEdges = 0, offEdges = 0;
			uint64_t frames = 0, onFrames = 0, offFrames = 0, unknownFrames = 0;
			uint64_t pulsesWithoutOnFrame = 0;
			uint64_t unclassifiedPulses = 0;
			uint64_t intervalCount = 0;
			double minIntervalMs = 0, maxIntervalMs = 0, sumIntervalMs = 0;
			std::chrono::steady_clock::time_point lastFrame;
		} m_bpmDiagnostics;
		baseLib::EventListener<juceRmlUi::RmlComponent*> m_onBpmFrameGenerated;
		bool m_logoAnimationEnabled = true;

		std::array<std::unique_ptr<jucePluginEditorLib::Led>, 3> m_lfos;

		std::unique_ptr<jucePluginEditorLib::Led> m_logoLed;
		std::unique_ptr<jucePluginEditorLib::Led> m_bpmLed;

		Rml::Element* m_logo = nullptr;
		Rml::Element* m_logoAnim = nullptr;

		baseLib::EventListener<virusLib::FrontpanelState> m_onFrontPanelStateChanged;
	};
}
