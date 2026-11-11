#include "VirusEditor.h"

#include "ArpUserPattern.h"
#include "ControllerLinks.h"

#include "ParameterNames.h"
#include "PatchManager.h"
#include "SettingsDspAudioOsTIrus.h"
#include "SettingsGuiOsTIrus.h"
#include "VirusProcessor.h"
#include "VirusController.h"
#include "virusLib/device.h"

#include "jucePluginLib/filetype.h"
#include "jucePluginLib/pluginVersion.h"

#include "jucePluginEditorLib/patchmanager/savepatchdesc.h"
#include "jucePluginEditorLib/pluginDataModel.h"
#include "jucePluginEditorLib/settingsDeviceSpecific.h"

#include "juceRmlPlugin/skinConverter/skinConverterOptions.h"

#include "juceRmlUi/rmlElemButton.h"
#include "juceRmlUi/rmlElemComboBox.h"
#include "juceRmlUi/rmlEventListener.h"
#include "juceRmlUi/rmlHelper.h"
#include "juceRmlUi/rmlInplaceEditor.h"
#include "juceRmlUi/juceRmlComponent.h"

#include "juceUiLib/messageBox.h"

#include "RmlUi/Core/ElementDocument.h"
#include "RmlUi/Core/CoreInstance.h"
#include "RmlUi/Lua/IncludeLua.h"

namespace genericVirusUI
{
	VirusEditor::VirusEditor(virus::VirusProcessor& _processorRef, const jucePluginEditorLib::Skin& _skin) :
		Editor(_processorRef, _skin),
		m_processor(_processorRef),
		m_romChangedListener(_processorRef.evFirmwareChanged),
		m_playModeSetListener(static_cast<virus::Controller&>(_processorRef.getController()).evPlayModeSet)
	{
	}
	void VirusEditor::create()
	{
		Editor::create();

		m_parts.reset(new Parts(*this));
		m_leds.reset(new Leds(*this, m_processor));

		// Virus-specific Lua access; the controller outlives this editor's Lua context.
		{
			const juceRmlUi::RmlInterfaces::ScopedAccess access(*getRmlComponent());
			if(auto* L = getDocument()->GetCoreInstance().lua_state)
			{
				lua_getglobal(L, "params");
				if(lua_istable(L, -1))
				{
					lua_pushlightuserdata(L, &getController());
					lua_pushcclosure(L, [](lua_State* state) -> int
					{
						auto& controller = *static_cast<virus::Controller*>(lua_touserdata(state, lua_upvalueindex(1)));
						const auto direction = luaL_checkinteger(state, 1);
						const auto* modeParam = controller.getParameter(virus::g_paramPlayMode, 0);
						const auto mode = modeParam ? modeParam->getUnnormalizedValue() : -1;
						const auto part = controller.getCurrentPart();
						const auto* bankParam = controller.getParameter(mode == 0 ? "C_Undefined101" : "Part Bank Select", mode == 0 ? 0 : part);
						const auto* programParam = controller.getParameter(mode == 0 ? "C_Undefined102" : "Part Program Change", mode == 0 ? 0 : part);
						bool changed = false;
						if((direction == -1 || direction == 1) && (mode == 0 || mode == 1 || mode == 2)
							&& part < controller.getPartCount() && (mode != 0 || part == 0) && bankParam && programParam)
						{
							const auto bank = bankParam->getUnnormalizedValue();
							if(bank >= 0 && static_cast<uint32_t>(bank) < controller.getBankCount())
							{
								// OS6.6 4652/4685: adjacent bank, same program, no wrap.
								const auto target = static_cast<int>(bank) + static_cast<int>(direction);
								const auto program = programParam->getUnnormalizedValue();
								if(target >= 0 && static_cast<uint32_t>(target) < controller.getBankCount()
									&& program >= 0 && static_cast<size_t>(program) < controller.getSinglePresets()[target].size())
								{
									controller.setCurrentPartPreset(part, virusLib::fromArrayIndex(static_cast<uint8_t>(target)), static_cast<uint8_t>(program));
									changed = true;
								}
							}
						}
						lua_pushboolean(state, changed);
						return 1;
					}, 1);
					lua_setfield(L, -2, "stepSingleBank");
					lua_pushlightuserdata(L, &getController());
					lua_pushcclosure(L, [](lua_State* state) -> int
					{
						auto& controller = *static_cast<virus::Controller*>(lua_touserdata(state, lua_upvalueindex(1)));
						const auto direction = luaL_checkinteger(state, 1);
						const auto* mode = controller.getParameter(virus::g_paramPlayMode, 0);
						auto* program = controller.getParameter("Multi Program Change", 0);
						if((direction != -1 && direction != 1) || !mode || mode->getUnnormalizedValue() != 2 || !program)
						{
							lua_pushboolean(state, false);
							return 1;
						}
						const auto target = program->getUnnormalizedValue() + static_cast<int>(direction);
						const bool changed = target >= 0 && target <= 127
							&& controller.sendParameterChange(virusLib::PARAM_CHANGE_C, 0, virusLib::MULTI_PROGRAM_CHANGE, static_cast<uint8_t>(target));
						if(changed)
						{
							program->setValueFromSynth(target, pluginLib::Parameter::Origin::PresetChange);
							// A new native MULTI replaces all part buffers, including browser patches.
							for(uint8_t part = 0; part < controller.getPartCount(); ++part)
								controller.setCurrentPartPresetSource(part, virus::Controller::PresetSource::Unknown);
							controller.requestArrangement();
						}
						lua_pushboolean(state, changed);
						return 1;
					}, 1);
					lua_setfield(L, -2, "stepMultiProgram");
					lua_pushlightuserdata(L, &getController());
					lua_pushcclosure(L, [](lua_State* state) -> int
					{
						auto& controller = *static_cast<virus::Controller*>(lua_touserdata(state, lua_upvalueindex(1)));
						const auto bank = luaL_checkinteger(state, 1);
						const auto program = luaL_checkinteger(state, 2);
						size_t length = 0;
						const auto* name = luaL_checklstring(state, 3, &length);
						const bool hasSourcePart = !lua_isnoneornil(state, 4);
						const auto sourcePart = hasSourcePart ? luaL_checkinteger(state, 4) : 0;
						// Validate before narrowing Lua integers. The controller validates again.
						const bool validDraft = bank >= 0 && bank <= 1 && program >= 0 && program <= 127 && length == 10
							&& (!hasSourcePart || (sourcePart >= 0 && sourcePart <= 15));
						const auto token = validDraft
							? controller.storeSingle(static_cast<int>(bank), static_cast<int>(program), std::string(name, length),
								hasSourcePart ? std::optional<int>(static_cast<int>(sourcePart)) : std::nullopt) : 0;
						lua_pushinteger(state, token);
						lua_pushstring(state, token ? "pending" : validDraft ? controller.singleStoreStatus(0) : "INVALID DRAFT");
						return 2;
					}, 1);
					lua_setfield(L, -2, "storeSingle");
					lua_pushlightuserdata(L, &getController());
					lua_pushcclosure(L, [](lua_State* state) -> int
					{
						auto& controller = *static_cast<virus::Controller*>(lua_touserdata(state, lua_upvalueindex(1)));
						const auto token = luaL_checkinteger(state, 1);
						lua_pushstring(state, token > 0 && token <= 0x7fffffff
							? controller.singleStoreStatus(static_cast<uint32_t>(token), lua_toboolean(state, 2) != 0) : "failed");
						return 1;
					}, 1);
					lua_setfield(L, -2, "singleStoreStatus");
                    lua_pushlightuserdata(L, &getController());
                    lua_pushcclosure(L, [](lua_State* state) -> int
                    {
                        auto& controller = *static_cast<virus::Controller*>(lua_touserdata(state, lua_upvalueindex(1)));
                        const auto program = luaL_checkinteger(state, 1);
                        size_t length = 0;
                        const auto* name = luaL_checklstring(state, 2, &length);
                        const bool validDraft = program >= 0 && program <= 127 && length == 10;
                        const auto token = validDraft
                            ? controller.storeMulti(static_cast<int>(program), std::string(name, length)) : 0;
                        lua_pushinteger(state, token);
                        lua_pushstring(state, token ? "pending" : validDraft ? controller.multiStoreStatus(0) : "INVALID DRAFT");
                        return 2;
                    }, 1);
                    lua_setfield(L, -2, "storeMulti");
                    lua_pushlightuserdata(L, &getController());
                    lua_pushcclosure(L, [](lua_State* state) -> int
                    {
                        auto& controller = *static_cast<virus::Controller*>(lua_touserdata(state, lua_upvalueindex(1)));
                        const auto token = luaL_checkinteger(state, 1);
                        lua_pushstring(state, token > 0 && token <= 0x7fffffff
                            ? controller.multiStoreStatus(static_cast<uint32_t>(token), lua_toboolean(state, 2) != 0) : "failed");
                        return 1;
                    }, 1);
                    lua_setfield(L, -2, "multiStoreStatus");
                    lua_pushlightuserdata(L, &getController());
                    lua_pushcclosure(L, [](lua_State* state) -> int
                    {
                        const auto& controller = *static_cast<virus::Controller*>(lua_touserdata(state, lua_upvalueindex(1)));
                        lua_pushboolean(state, controller.hasHostTempo());
                        return 1;
                    }, 1);
                    lua_setfield(L, -2, "hasHostTempo");
                    // 138s1 BEGIN: isolated SINGLE dump request using the existing MIDI routing.
                    lua_pushlightuserdata(L, this);
                    lua_pushcclosure(L, [](lua_State* state) -> int
                    {
                        auto& editor = *static_cast<VirusEditor*>(lua_touserdata(state, lua_upvalueindex(1)));
                        auto& controller = editor.getController();
                        const auto* mode = controller.getParameter(virus::g_paramPlayMode, 0);
                        const auto& routing = editor.m_processor.getMidiRoutingMatrix();
                        const auto playMode = mode ? mode->getUnnormalizedValue() : -1;
                        const auto part = controller.getCurrentPart();
                        const char* result = "Requires Virus C SINGLE or MULTI SINGLE";
                        if (editor.m_processor.getModel() == virusLib::DeviceModel::C
                            && (playMode == virusLib::PlayModeSingle
                                || (playMode == virusLib::PlayModeMultiSingle && part < 16)))
                        {
                            using Source = synthLib::MidiEventSource;
                            using Type = synthLib::MidiRoutingMatrix::EventType;
                            if (!routing.enabled(Source::Editor, Source::Device, Type::SysEx)
                                || !routing.enabled(Source::Device, Source::Host, Type::SysEx))
                                result = "Enable Editor-Device and Device-Host SysEx";
                            else
                            {
                                // 138s8: 0x40 is the separate SINGLE buffer; 0..15 address part buffers.
                                const auto program = playMode == virusLib::PlayModeSingle ? virusLib::SINGLE : part;
                                const bool requested = controller.requestSingle(
                                    virusLib::toMidiByte(virusLib::BankNumber::EditBuffer), program);
                                result = requested ? "SINGLE dump requested" : "Request failed";
                            }
                        }
                        lua_pushstring(state, result);
                        return 1;
                    }, 1);
                    lua_setfield(L, -2, "requestSingleDump138s1");
                    // 138s1 END
                    // 138j: transmit only the two ROM E45 DSP words (F4F472, part/0/0).
                    // The Microcontroller handles this exact panel action without changing C0.
                    lua_pushlightuserdata(L, &getController());
                    lua_pushcclosure(L, [](lua_State* state) -> int
                    {
                        auto& controller = *static_cast<virus::Controller*>(lua_touserdata(state, lua_upvalueindex(1)));
                        const auto* playMode = controller.getParameter(virus::g_paramPlayMode, 0);
                        const int mode = playMode ? playMode->getUnnormalizedValue() : -1;
                        const auto selectedPart = controller.getCurrentPart();
                        const bool valid = mode >= 0 && mode <= 2 && (mode == 0 || selectedPart < controller.getPartCount());
                        const auto part = static_cast<uint8_t>(mode == 0 ? virusLib::SINGLE : selectedPart);
                        const bool sent = valid && controller.sendParameterChange(virusLib::PARAM_CHANGE_C, part, 0, 0);
                        lua_pushboolean(state, sent);
                        return 1;
                    }, 1);
                    lua_setfield(L, -2, "panicOnce");
                    // 138k: controller reset following a second SHIFT+TRANSPOSE click.
                    // Unlike panicOnce, MIDI controller reset is a distinct action.
                    lua_pushlightuserdata(L, &getController());
                    lua_pushcclosure(L, [](lua_State* state) -> int
                    {
                        auto& controller = *static_cast<virus::Controller*>(lua_touserdata(state, lua_upvalueindex(1)));
                        const auto* playMode = controller.getParameter(virus::g_paramPlayMode, 0);
                        const int mode = playMode ? playMode->getUnnormalizedValue() : -1;
                        const bool valid = mode >= 0 && mode <= 2;
                        if(valid)
                            controller.panicResetControllers();
                        lua_pushboolean(state, valid);
                        return 1;
                    }, 1);
                    lua_setfield(L, -2, "panicReset");
                    // 138l AUDITION: editor-only C0 sentinels. Microcontroller
                    // maps them to the ROM E37/E59 F4F472 + part/01/7F or 01/00.
                    // No patch/global parameter is modified.
                    lua_pushlightuserdata(L, this);
                    lua_pushcclosure(L, [](lua_State* state) -> int
                    {
                        auto& editor = *static_cast<VirusEditor*>(lua_touserdata(state, lua_upvalueindex(1)));
                        lua_pushboolean(state, editor.startAudition());
                        return 1;
                    }, 1);
                    lua_setfield(L, -2, "auditionStart");
                    lua_pushlightuserdata(L, this);
                    lua_pushcclosure(L, [](lua_State* state) -> int
                    {
                        auto& editor = *static_cast<VirusEditor*>(lua_touserdata(state, lua_upvalueindex(1)));
                        lua_pushboolean(state, editor.stopAudition());
                        return 1;
                    }, 1);
                    lua_setfield(L, -2, "auditionStop");
                    lua_pushlightuserdata(L, this);
                    lua_pushcclosure(L, [](lua_State* state) -> int
                    {
                        auto& editor = *static_cast<VirusEditor*>(lua_touserdata(state, lua_upvalueindex(1)));
                        lua_pushboolean(state, editor.releaseAudition());
                        return 1;
                    }, 1);
                    lua_setfield(L, -2, "auditionRelease");
                    lua_pushlightuserdata(L, this);
                    lua_pushcclosure(L, [](lua_State* state) -> int
                    {
                        auto& editor = *static_cast<VirusEditor*>(lua_touserdata(state, lua_upvalueindex(1)));
                        lua_pushinteger(state, static_cast<int>(editor.refreshAudition()));
                        return 1;
                    }, 1);
                    lua_setfield(L, -2, "auditionState");
				}
				lua_pop(L, 1);
			}
		}

		m_presetName = findChild("PatchName");

		m_romSelector = std::make_unique<jucePluginEditorLib::FirmwareSelector>(*this, findChild<juceRmlUi::ElemComboBox>("RomSelector"));

		m_playModeSingle = findChild<juceRmlUi::ElemButton>("PlayModeSingle", false);
		m_playModeMulti = findChild<juceRmlUi::ElemButton>("PlayModeMulti", false);

		if(m_playModeSingle && m_playModeMulti)
		{
			juceRmlUi::EventListener::AddClick(m_playModeSingle, [this] { setPlayMode(virusLib::PlayMode::PlayModeSingle); });
			juceRmlUi::EventListener::AddClick(m_playModeMulti, [this] { setPlayMode(virusLib::PlayMode::PlayModeMulti); });
		}
		else
		{
			m_playModeToggle = findChild<juceRmlUi::ElemButton>("PlayModeToggle");
			juceRmlUi::EventListener::AddClick(m_playModeToggle, [this] { setPlayMode(m_playModeToggle->isChecked() ? virusLib::PlayMode::PlayModeMulti : virusLib::PlayMode::PlayModeSingle); });
		}

		getController().onProgramChange = [this](int _part)
		{
			const juceRmlUi::RmlInterfaces::ScopedAccess access(*getRmlComponent());
			onProgramChange(_part);
		};

		if(auto* versionInfo = findChild("VersionInfo", false))
		{
		    const std::string message = "DSP 56300 Emulator Version " + pluginLib::Version::getVersionString() + " - " + pluginLib::Version::getVersionDateTime();
			versionInfo->SetInnerRML(message);
		}

		if(auto* versionNumber = findChild("VersionNumber", false))
		{
			versionNumber->SetInnerRML(pluginLib::Version::getVersionString());
		}

		m_deviceModel = findChild("DeviceModel", false);

		auto* presetSave = findChild("PresetSave", false);
		if(presetSave)
			juceRmlUi::EventListener::Add(presetSave, Rml::EventId::Click, [this](Rml::Event& _event) { savePreset(_event); });

		auto* presetLoad = findChild("PresetLoad", false);
		if(presetLoad)
			juceRmlUi::EventListener::AddClick(presetLoad, [this] { loadPreset(); });

		juceRmlUi::EventListener::Add(m_presetName, Rml::EventId::Mousedown, [this](Rml::Event& _event)
		{
			if (_event.GetTargetElement() != _event.GetCurrentElement())
				return;
			if (!juceRmlUi::helper::isContextMenu(_event))
				return;
			// Swallow the right-click so the global context menu doesn't also open
			_event.StopPropagation();
			// Only toggle in Multi mode; Single mode has only one name to show
			if (!getController().isMultiMode())
				return;
			m_showMultiName = !m_showMultiName;
			updatePresetName();
		});

		juceRmlUi::EventListener::Add(m_presetName, Rml::EventId::Dblclick, [this](const Rml::Event& _event)
		{
			if (_event.GetTargetElement() != _event.GetCurrentElement())
				return;

			const bool editMulti = getController().isMultiMode() && m_showMultiName;

			new juceRmlUi::InplaceEditor(m_presetName, Rml::StringUtilities::StripWhitespace(m_presetName->GetInnerRML()), [this, editMulti](const std::string& _text)
			{
				auto text = Rml::StringUtilities::StripWhitespace(_text);
				if (text.empty())
					return;
				if (editMulti)
				{
					getController().setMultiPresetName(text);
					updatePresetName();
				}
				else
				{
					getController().setSinglePresetName(getController().getCurrentPart(), text);
					onProgramChange(getController().getCurrentPart());
				}
			});
		});

/*
		m_presetNameMouseListener = new PartMouseListener(pluginLib::MidiPacket::AnyPart, [this](const juce::MouseEvent&, int)
		{
			startDragging(new jucePluginEditorLib::patchManager::SavePatchDesc(*getPatchManager(), getController().getCurrentPart()), m_presetName);
		});
		m_presetName->addMouseListener(m_presetNameMouseListener, false);
*/

		auto* menuButton = findChild("Menu", false);

		if(menuButton)
		{
			juceRmlUi::EventListener::Add(menuButton, Rml::EventId::Click, [this](Rml::Event& _event)
			{
				openMenu(_event);
			});
		}

		updatePresetName();
		updatePlayModeButtons();

		// the patch manager switches the play mode when it loads a multi, with or without an editor
		m_playModeSetListener = [this](const uint8_t _playMode)
		{
			if (_playMode == virusLib::PlayModeSingle && getController().getCurrentPart() != 0)
				setPart(0);

			onPlayModeChanged();
		};

		m_romChangedListener = [this](auto)
		{
			updateDeviceModel();
			getPluginDataModel()->set("deviceModel", virusLib::getModelName(m_processor.getModel()));
			m_parts->onPlayModeChanged();
		};

		if (auto* arpUserGraphics = findChild("ArpUserGraphics", false))
			m_arpUserPattern.reset(new ArpUserPattern(*this, arpUserGraphics));

		ControllerLinks::create(*this);
	}

	VirusEditor::AuditionState VirusEditor::refreshAudition()
	{
		auto* device = dynamic_cast<virusLib::Device*>(m_processor.getPlugin().getDevice());
		const auto outcome = device ? device->getAuditionOutcome() : nullptr;
		if(outcome != m_auditionOutcome)
		{
			m_auditionOutcome = outcome;
			m_auditionPart = -1;
			m_auditionState = AuditionState::Idle;
			m_auditionReleaseRequested = m_auditionStopRequested = false;
		}
		if(!outcome)
			return AuditionState::Unavailable;
		const auto observed = outcome->load(std::memory_order_acquire);
		if(!observed)
			return AuditionState::Unavailable;
		if(m_auditionState == AuditionState::Pending && (observed >> 2) != m_auditionSerial)
		{
			switch(observed & 3)
			{
			case 1: m_auditionState = AuditionState::Held; break;
			case 2: m_auditionState = AuditionState::Latched; break;
			case 3:
				m_auditionState = AuditionState::Idle;
				m_auditionPart = -1;
				m_auditionReleaseRequested = m_auditionStopRequested = false;
				break;
			default: break;
			}
		}
		// Follow-up commands are conditional on a completed native branch, never on elapsed UI time.
		if(m_auditionStopRequested && (m_auditionState == AuditionState::Held || m_auditionState == AuditionState::Latched))
		{
			if(!sendAudition(m_auditionState == AuditionState::Latched ? 127 : 1))
				m_auditionStopRequested = false;
		}
		else if(m_auditionReleaseRequested && m_auditionState == AuditionState::Held)
		{
			m_auditionReleaseRequested = false;
			sendAudition(1);
		}
		return m_auditionState;
	}

	bool VirusEditor::sendAudition(const uint8_t _value)
	{
		if(m_auditionPart < 0 || !m_auditionOutcome)
			return false;
		const auto observed = m_auditionOutcome->load(std::memory_order_acquire);
		if(!observed)
			return false;
		if(!getController().sendParameterChange(virusLib::PARAM_CHANGE_C,
			static_cast<uint8_t>(m_auditionPart), 0, _value))
			return false;
		m_auditionSerial = observed >> 2;
		m_auditionState = AuditionState::Pending;
		return true;
	}

	bool VirusEditor::startAudition()
	{
		if(refreshAudition() != AuditionState::Idle || m_processor.getModel() != virusLib::DeviceModel::C)
			return false;
		auto& controller = getController();
		const auto* modeParam = controller.getParameter(virus::g_paramPlayMode, 0);
		const int mode = modeParam ? modeParam->getUnnormalizedValue() : -1;
		const auto part = controller.getCurrentPart();
		if(mode < 0 || mode > 2 || (mode != 0 && part >= controller.getPartCount()))
			return false;
		m_auditionPart = mode == 0 ? virusLib::SINGLE : part;
		if(sendAudition(127))
			return true;
		m_auditionPart = -1;
		return false;
	}

	bool VirusEditor::releaseAudition()
	{
		const auto state = refreshAudition();
		if(state == AuditionState::Pending)
		{
			m_auditionReleaseRequested = true; // mouseup before START acknowledgement
			return true;
		}
		return state == AuditionState::Held && sendAudition(1);
	}

	bool VirusEditor::stopAudition()
	{
		const auto state = refreshAudition();
		if(state == AuditionState::Idle)
			return true;
		if(state != AuditionState::Held && state != AuditionState::Latched)
			return false;
		const bool sent = sendAudition(state == AuditionState::Latched ? 127 : 1);
		m_auditionStopRequested = sent;
		return sent;
	}

	VirusEditor::~VirusEditor()
	{
		// Avoid a latched AUDITION note when closing the editor.
		const auto audition = refreshAudition();
		if(m_auditionPart >= 0 && (audition == AuditionState::Held || audition == AuditionState::Latched))
		{
			getController().sendParameterChange(virusLib::PARAM_CHANGE_C,
				static_cast<uint8_t>(m_auditionPart), 0, audition == AuditionState::Latched ? 127 : 1);
			m_auditionPart = -1;
		}
		m_arpUserPattern.reset();

		getController().onProgramChange = nullptr;
	}

	virus::Controller& VirusEditor::getController() const
	{
		return static_cast<virus::Controller&>(m_processor.getController());
	}

	/*
	juce::Component* VirusEditor::createJuceComponent(juce::Component* _component, genericUI::UiObject& _object)
	{
		if(_object.getName() == "ArpUserGraphics")
		{
			assert(m_arpUserPattern == nullptr);
			m_arpUserPattern = new ArpUserPattern(*this);
			return m_arpUserPattern;
		}

		return Editor::createJuceComponent(_component, _object);
	}
	*/
	void VirusEditor::initSkinConverterOptions(rmlPlugin::skinConverter::SkinConverterOptions& _skinConverterOptions)
	{
		Editor::initSkinConverterOptions(_skinConverterOptions);

		_skinConverterOptions.idReplacements.insert({"page_presets", "container-patchmanager"});
		_skinConverterOptions.idReplacements.insert({"page_2_browser", "container-patchmanager"});
	}

	void VirusEditor::initPluginDataModel(jucePluginEditorLib::PluginDataModel& _model)
	{
		Editor::initPluginDataModel(_model);

		_model.set("deviceModel", virusLib::getModelName(m_processor.getModel()));
		_model.setFunc("multiMode", [this]
		{
			return getController().isMultiMode() ? "1" : "0";
		}, [this](const std::string& _multiMode)
		{
			try
			{
				auto mode = std::stoi(_multiMode);
				setPlayMode(mode ? virusLib::PlayModeMulti : virusLib::PlayModeSingle);
			}
			catch (std::exception&)
			{
				// ignore invalid input
			}
		});
	}

	std::unique_ptr<jucePluginEditorLib::SettingsDeviceSpecific> VirusEditor::createDeviceSpecificSettings(const std::string& _templateName, Rml::Element* _root)
	{
		if (_templateName == "tus_settings_gui_OsTIrus")
			return std::make_unique<SettingsGuiOsTIrus>(this, _root);

		if (_templateName == "tus_settings_dspaudio_OsTIrus")
			return std::make_unique<SettingsDspAudioOsTIrus>(this, _root);

		return Editor::createDeviceSpecificSettings(_templateName, _root);
	}

	void VirusEditor::onProgramChange(int _part)
	{
		m_parts->onProgramChange();
		updatePresetName();
		updatePlayModeButtons();
		if(getPatchManager())
			getPatchManager()->onProgramChanged(_part);
	}

	void VirusEditor::onPlayModeChanged()
	{
		m_parts->onPlayModeChanged();
		if (!getController().isMultiMode())
			m_showMultiName = false;
		updatePresetName();
		updatePlayModeButtons();
	}

	void VirusEditor::onCurrentPartChanged(const uint8_t _part)
	{
		Editor::onCurrentPartChanged(_part);

		// the base subscribes before create() has built the panel
		if(!m_parts)
			return;

		m_parts->onCurrentPartChanged();
		if(m_arpUserPattern)
			m_arpUserPattern->onCurrentPartChanged();
		updatePresetName();
	}

	void VirusEditor::updatePresetName() const
	{
		const auto& c = getController();
		const auto name = (c.isMultiMode() && m_showMultiName)
			? c.getMultiPresetName()
			: c.getCurrentPartPresetName(c.getCurrentPart());
		m_presetName->SetInnerRML(name);
	}

	void VirusEditor::updatePlayModeButtons() const
	{
		if(m_playModeSingle)
			m_playModeSingle->setChecked(!getController().isMultiMode());
		if(m_playModeMulti)
			m_playModeMulti->setChecked(getController().isMultiMode());
		if(m_playModeToggle)
			m_playModeToggle->setChecked(getController().isMultiMode());
	}

	void VirusEditor::updateDeviceModel()
	{
		if(!m_deviceModel)
			return;

		std::string m;

		switch(m_processor.getModel())
		{
		case virusLib::DeviceModel::Invalid:
			return;
		case virusLib::DeviceModel::ABC:
			{
				auto* rom = m_processor.getSelectedRom();
				if(!rom)
					return;

				virusLib::ROMFile::TPreset data;
				if(!rom->getSingle(0, 0, data))
					return;

				switch(virusLib::Microcontroller::getPresetVersion(data.front()))
				{
				case virusLib::A:	m = "A";	break;
				case virusLib::B:	m = "B";	break;
				case virusLib::C:	m = "C";	break;
				case virusLib::D:	m = "TI";	break;
				case virusLib::D2:	m = "TI2";	break;
				default:			m = "?";	break;
				}
			}
			break;
		case virusLib::DeviceModel::Snow:	m = "Snow";	break;
		case virusLib::DeviceModel::TI:		m = "TI";	break;
		case virusLib::DeviceModel::TI2:	m = "TI2";	break;
		}

		m_deviceModel->SetInnerRML(m);
	}


	void VirusEditor::savePreset(const Rml::Event& _event)
	{
		// Pre-request the multi edit buffer so it's cached by the time the user
		// selects "Add arrangement to...". The request goes to the emulated DSP
		// and the response typically arrives within one audio callback, while
		// the user takes hundreds of ms to navigate the menu.
		if (getController().isMultiMode())
			getController().requestMulti(virusLib::toMidiByte(virusLib::BankNumber::EditBuffer), 0);

		juceRmlUi::Menu menu;

		uint32_t countAdded = getPatchManager()->createSaveMenuEntries(menu, "Single");

		if (getController().isMultiMode())
			countAdded += getPatchManager()->createSaveMenuEntries(menu, getController().getCurrentPart(), "Arrangement", PatchManager::g_userDataArrangement);

		if(countAdded)
			menu.addSeparator();

		auto addEntry = [&](juceRmlUi::Menu& _menu, const std::string& _name, const std::function<void(pluginLib::FileType)>& _callback)
		{
			juceRmlUi::Menu subMenu;
			createExportFileTypeMenu(subMenu, _callback);

			_menu.addSubMenu(_name, std::move(subMenu));
		};

		addEntry(menu, "Export Current Single (Edit Buffer)", [this](const pluginLib::FileType& _type)
		{
			savePresets(SaveType::CurrentSingle, _type);
		});

		if(getController().isMultiMode())
		{
			addEntry(menu, "Export Arrangement (Multi + 16 Singles)", [this](const pluginLib::FileType& _type)
			{
				savePresets(SaveType::Arrangement, _type);
			});
		}

		juceRmlUi::Menu banksMenu;
		for(uint8_t b=0; b<static_cast<uint8_t>(getController().getBankCount()); ++b)
		{
			addEntry(banksMenu, getController().getBankName(b), [this, b](const pluginLib::FileType& _type)
			{
				savePresets(SaveType::Bank, _type, b);
			});
		}

		menu.addSubMenu("Export Bank", std::move(banksMenu));

		menu.runModal(_event);
	}

	void VirusEditor::loadPreset()
	{
		Editor::loadPreset([this](const juce::File& _result)
		{
			auto* pm = getPatchManager();

			const auto patches = pm->loadPatchesFromFiles(std::vector<std::string>{_result.getFullPathName().toStdString()});

			if (patches.empty())
				return;

			// A single patch may be a Single, Multi or Arrangement — the PatchManager
			// dispatches by type. Arrangements received from a file used to be 17
			// separate dumps; parseFileData now merges them into a single compound
			// patch, so this branch covers them too.
			if (patches.size() == 1)
			{
				pm->activatePatch(patches.front(), getController().getCurrentPart());
				return;
			}

			genericUI::MessageBox::showOk(genericUI::MessageBox::Icon::Info, "Information",
				"The selected file contains more than one patch. Please add this file as a data source in the Patch Manager instead.\n\n"
				"Go to the Patch Manager, right click the 'Data Sources' node and select 'Add File...' to import it."
			);
		});
	}

	void VirusEditor::setPlayMode(const uint8_t _playMode)
	{
		getController().setPlayMode(_playMode);
	}

	void VirusEditor::savePresets(SaveType _saveType, const pluginLib::FileType& _fileType, uint8_t _bankNumber/* = 0*/)
	{
		Editor::savePreset(_fileType, [this, _saveType, _bankNumber, _fileType](const juce::File& _result)
		{
			pluginLib::FileType fileType = _fileType;
			const auto file = createValidFilename(fileType, _result);
			savePresets(file, _saveType, fileType, _bankNumber);
		});
	}

	bool VirusEditor::savePresets(const std::string& _pathName, SaveType _saveType, const pluginLib::FileType& _fileType, uint8_t _bankNumber/* = 0*/) const
	{
		synthLib::SysexBufferList messages;
		
		switch (_saveType)
		{
		case SaveType::CurrentSingle:
			{
				const auto dump = getController().createSingleDump(getController().getCurrentPart(), toMidiByte(virusLib::BankNumber::A), 0);
				messages.push_back(dump);
			}
			break;
		case SaveType::Bank:
			{
				const auto& presets = getController().getSinglePresets();
				if(_bankNumber < presets.size())
				{
					const auto& bankPresets = presets[_bankNumber];
					for (const auto& bankPreset : bankPresets)
						messages.push_back(bankPreset.data);
				}
			}
			break;
		case SaveType::Arrangement:
			{
				getController().onMultiReceived = [this, _fileType, _pathName]
				{
					synthLib::SysexBufferList messages;
					messages.push_back(getController().getMultiEditBuffer().data);

					for(uint8_t i=0; i<16; ++i)
					{
						const auto dump = getController().createSingleDump(i, toMidiByte(virusLib::BankNumber::EditBuffer), i);
						messages.push_back(dump);
						Editor::savePresets(_fileType, _pathName, messages);
					}

					getController().onMultiReceived = {};
				};
				getController().requestMulti(0, 0);
			}
			return true;
		default:
			return false;
		}

		return Editor::savePresets(_fileType, _pathName, messages);
	}

	void VirusEditor::setPart(const size_t _part)
	{
		setCurrentPart(static_cast<uint8_t>(_part));
	}
}
