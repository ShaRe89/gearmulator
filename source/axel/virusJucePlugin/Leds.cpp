#include "Leds.h"

#include "VirusController.h"
#include "VirusEditor.h"

#include "VirusProcessor.h"

#include "juceRmlUi/juceRmlComponent.h"
#include "RmlUi/Core/ComputedValues.h"
#include "RmlUi/Core/Element.h"

namespace genericVirusUI
{
	constexpr const char* const g_logoAnimKey = "logoAnimation";

	constexpr const char* g_lfoNames[3] = {"Lfo1LedOn", "Lfo2LedOn", "Lfo3LedOn"};

	Leds::Leds(const VirusEditor& _editor, virus::VirusProcessor& _processor) : m_processor(_processor), m_rmlComponent(*_editor.getRmlComponent()), m_logoAnimationEnabled(_processor.getConfig().getBoolValue(g_logoAnimKey, true))
	{
		m_onFrontPanelStateChanged.set(_editor.getController().onFrontPanelStateChanged, [this](const virusLib::FrontpanelState& _frontpanelState)
		{
			if(m_processor.getModel() == virusLib::DeviceModel::C)
			{
				// Controller delivery is on the message thread. Serialize LED mutation and
				// observation with RmlUi generation, including GL's fallback render.
				const juceRmlUi::RmlInterfaces::ScopedAccess access(m_rmlComponent);
				onFrontPanelStateChanged(_frontpanelState);
				return;
			}
			onFrontPanelStateChanged(_frontpanelState);
		});

		for(size_t i=0; i<m_lfos.size(); ++i)
		{
			if(auto* comp = _editor.findChild(g_lfoNames[i], false))
			{
				m_lfos[i].reset(new jucePluginEditorLib::Led(_editor, comp));
			}
		}

		if(m_processor.getModel() == virusLib::DeviceModel::C)
		{
			if(auto* bpm = _editor.findChild("HC_R_LED_0070", false))
			{
				m_bpmElement = bpm;
				m_bpmLed.reset(new jucePluginEditorLib::Led(_editor, bpm));
				m_bpmLed->setValue(0.0f);
			}
		}

		if(auto* logoAnim = _editor.findChild("logolight", false))
		{
			m_logoAnim = logoAnim;

			m_logoLed.reset(new jucePluginEditorLib::Led(_editor, logoAnim));

			addLogoClickListener(logoAnim);

			m_logo = _editor.findChild("logo", false);

			if(m_logo)
			{
				addLogoClickListener(m_logo);
			}
		}

		const juceRmlUi::RmlInterfaces::ScopedAccess access(m_rmlComponent);
		if(m_processor.getModel() == virusLib::DeviceModel::C)
		{
			m_onBpmFrameGenerated.set(m_rmlComponent.evFrameGenerated, [this](juceRmlUi::RmlComponent*)
			{
				onBpmFrameGenerated();
			});
		}
		onFrontPanelStateChanged(_editor.getController().getFrontpanelState());
	}

	Leds::~Leds()
	{
		const juceRmlUi::RmlInterfaces::ScopedAccess access(m_rmlComponent);
		m_onBpmFrameGenerated.reset();
		m_onFrontPanelStateChanged.reset();
		for (auto& led : m_lfos)
			led.reset();
	}

	void Leds::setBpmDiagnosticsEnabled(const bool _enabled)
	{
		const juceRmlUi::RmlInterfaces::ScopedAccess access(m_rmlComponent);
		if(m_processor.getModel() != virusLib::DeviceModel::C)
			return;

		if(_enabled)
		{
			m_bpmDiagnostics = {};
			m_bpmDiagnostics.initialOn = m_bpmDisplayState;
		}
		m_bpmDiagnostics.enabled = _enabled;
	}

	void Leds::onBpmFrameGenerated()
	{
		// Called after Render()/finishFrame(), under the same RML access lock as
		// controller delivery. Only a resolved frame can acknowledge a transition.
		const bool visible = m_bpmElement && m_bpmElement->IsVisible(true);
		const auto opacity = visible ? m_bpmElement->GetComputedValues().opacity() : -1.0f;
		auto& d = m_bpmDiagnostics;
		if(d.enabled)
		{
			const auto now = std::chrono::steady_clock::now();
			if(d.frames)
			{
				const auto ms = std::chrono::duration<double, std::milli>(now - d.lastFrame).count();
				if(!d.intervalCount || ms < d.minIntervalMs)
					d.minIntervalMs = ms;
				d.maxIntervalMs = std::max(d.maxIntervalMs, ms);
				d.sumIntervalMs += ms;
				++d.intervalCount;
			}
			d.lastFrame = now;
			++d.frames;
			if(opacity == 1.0f)
			{
				++d.onFrames;
				if(d.pulseOpen)
					d.pulseHadOnFrame = true;
			}
			else if(opacity == 0.0f)
				++d.offFrames;
			else
			{
				++d.unknownFrames;
				if(d.pulseOpen)
					d.pulseHadUnknownFrame = true;
			}
		}

		if(!m_bpmFramePending || opacity != (m_bpmDisplayState ? 1.0f : 0.0f))
			return;
		m_bpmFramePending = false;
		if(!m_bpmTransitions.empty())
		{
			const bool next = m_bpmTransitions.front();
			m_bpmTransitions.pop_front();
			setBpmDisplayState(next);
		}
	}

	void Leds::setBpmDisplayState(const bool _on)
	{
		auto& d = m_bpmDiagnostics;
		// Pulse outcomes follow display handoff; listener edge counters stay raw.
		if(d.enabled)
		{
			if(_on)
			{
				d.pulseOpen = true;
				d.pulseHadOnFrame = false;
				d.pulseHadUnknownFrame = false;
			}
			else
			{
				if(d.pulseOpen && !d.pulseHadOnFrame)
				{
					if(d.pulseHadUnknownFrame)
						++d.unclassifiedPulses;
					else
						++d.pulsesWithoutOnFrame;
				}
				d.pulseOpen = false;
			}
		}
		m_bpmDisplayState = _on;
		m_bpmFramePending = true;
		m_bpmLed->setValue(_on ? 1.0f : 0.0f);
	}

	std::string Leds::getBpmDiagnosticsReport() const
	{
		const juceRmlUi::RmlInterfaces::ScopedAccess access(m_rmlComponent);
		const auto& d = m_bpmDiagnostics;
		std::string report = "gui_measurement=generated_RML_command_frames_not_GPU_or_screen_presentation\n";
		report += "gui_frame_state=resolved_LED_opacity_and_RML_visibility_no_pixel_or_occlusion_check\n";
		report += "gui_pulse_outcomes=display_handoff_listener_edges_remain_raw\n";
		report += "gui_queued_transitions=" + std::to_string(m_bpmTransitions.size()) + "\n";
		report += "gui_window=message_thread_start_stop_device_window_is_audio_callback_applied\n";
		report += std::string("gui_enabled=") + (d.enabled ? "1\n" : "0\n");
		report += std::string("gui_bpm_element_available=") + (m_bpmElement ? "1\n" : "0\n");
		auto add = [&report](const char* _name, uint64_t _value)
		{
			report += std::string(_name) + "=" + std::to_string(_value) + "\n";
		};
		add("gui_listener_on_edges", d.onEdges);
		add("gui_listener_off_edges", d.offEdges);
		add("gui_initial_on_excluded_from_pulses", d.initialOn ? 1 : 0);
		add("gui_generated_frames", d.frames);
		add("gui_frames_led_unknown", d.unknownFrames);
		if(m_bpmElement)
		{
			add("gui_frames_led_on", d.onFrames);
			add("gui_frames_led_off", d.offFrames);
			add("gui_completed_on_pulses_without_on_frame", d.pulsesWithoutOnFrame);
			add("gui_completed_on_pulses_unclassified", d.unclassifiedPulses);
		}
		else
			report += "gui_frames_led_on=n/a\ngui_frames_led_off=n/a\ngui_completed_on_pulses_without_on_frame=n/a\ngui_completed_on_pulses_unclassified=n/a\n";
		add("gui_pending_on_pulse", d.pulseOpen ? 1 : 0);
		add("gui_pending_on_pulse_had_on_frame", d.pulseOpen && d.pulseHadOnFrame ? 1 : 0);
		add("gui_frame_interval_count", d.intervalCount);
		if(d.intervalCount)
		{
			report += "gui_frame_interval_min_ms=" + std::to_string(d.minIntervalMs) + "\n";
			report += "gui_frame_interval_max_ms=" + std::to_string(d.maxIntervalMs) + "\n";
			report += "gui_frame_interval_avg_ms=" + std::to_string(d.sumIntervalMs / static_cast<double>(d.intervalCount)) + "\n";
		}
		else
			report += "gui_frame_interval_min_ms=n/a\ngui_frame_interval_max_ms=n/a\ngui_frame_interval_avg_ms=n/a\n";
		return report;
	}

	void Leds::toggleLogoAnimation()
	{
		m_logoAnimationEnabled = !m_logoAnimationEnabled;

		m_processor.getConfig().setValue(g_logoAnimKey, m_logoAnimationEnabled);
		m_processor.getConfig().saveIfNeeded();
	}

	void Leds::onFrontPanelStateChanged(const virusLib::FrontpanelState& _frontpanelState)
	{
		const bool bpmOn = m_processor.getModel() == virusLib::DeviceModel::C && _frontpanelState.m_bpm == 1.0f;
		auto& d = m_bpmDiagnostics;
		if(bpmOn != m_bpmState)
		{
			if(d.enabled)
			{
				if(bpmOn)
					++d.onEdges;
				else
					++d.offEdges;
			}
			m_bpmState = bpmOn;
			if(m_bpmLed)
			{
				// Preserve every edge, including OFF between two queued pulses.
				if(m_bpmFramePending)
					m_bpmTransitions.push_back(bpmOn);
				else
					setBpmDisplayState(bpmOn);
			}
		}

		for(size_t i=0; i<_frontpanelState.m_lfoPhases.size(); ++i)
		{
			const auto v = std::clamp(_frontpanelState.m_lfoPhases[i], 0.0f, 1.0f);
			if(!m_lfos[i])
				continue;
			m_lfos[i]->setValue(std::pow(1.0f - v, 0.2f));
		}

		if(m_logoLed)
		{
			if(!m_logoAnimationEnabled)
			{
				m_logoLed->setValue(0.0f);
			}
			else
			{
				const auto v = std::clamp(m_processor.getModel() == virusLib::DeviceModel::Snow ? _frontpanelState.m_bpm : _frontpanelState.m_logo, 0.0f, 1.0f);

				m_logoLed->setValue(std::pow(1.0f - v, 0.2f));
			}
		}
	}

	void Leds::addLogoClickListener(Rml::Element* _logo)
	{
		juceRmlUi::EventListener::Add(_logo, Rml::EventId::Click, [this](Rml::Event& _e)
		{
			_e.StopImmediatePropagation();
			toggleLogoAnimation();
		});
	}
}
