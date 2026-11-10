#pragma once

#include <atomic>

#include "jucePluginEditorLib/firmwareSelector.h"
#include "jucePluginEditorLib/pluginEditor.h"

#include "baseLib/event.h"

#include "Parts.h"
#include "PatchManager.h"
#include "Leds.h"

namespace juceRmlUi
{
	class ElemComboBox;
}

namespace virusLib
{
	class ROMFile;
}

namespace pluginLib
{
	class Parameter;
}

namespace virus
{
	class VirusProcessor;
}

namespace genericVirusUI
{
	class ArpUserPattern;

	class VirusEditor : public jucePluginEditorLib::Editor
	{
	public:
		enum class SaveType
		{
			CurrentSingle,
			Bank,
			Arrangement
		};

		VirusEditor(virus::VirusProcessor& _processorRef, const jucePluginEditorLib::Skin& _skin);
		~VirusEditor() override;

		void create() override;

		void setPart(size_t _part);

		virus::Controller& getController() const;
		virus::VirusProcessor& getProcessor() const { return m_processor; }

		const auto& getLeds() const { return m_leds; }

		void initSkinConverterOptions(rmlPlugin::skinConverter::SkinConverterOptions&) override;
		void initPluginDataModel(jucePluginEditorLib::PluginDataModel& _model) override;


		std::unique_ptr<jucePluginEditorLib::SettingsDeviceSpecific> createDeviceSpecificSettings(const std::string& _templateName, Rml::Element* _root) override;

		void setPlayMode(uint8_t _playMode);

	private:
		void onProgramChange(int _part);
		void onPlayModeChanged();
		void onCurrentPartChanged(uint8_t _part) override;

		void updatePresetName() const;
		void updatePlayModeButtons() const;

		void updateDeviceModel();

		void savePreset(const Rml::Event& _event);
		void loadPreset();

		void savePresets(SaveType _saveType, const pluginLib::FileType& _fileType, uint8_t _bankNumber = 0);
		bool savePresets(const std::string& _pathName, SaveType _saveType, const pluginLib::FileType& _fileType, uint8_t _bankNumber = 0) const;

		virus::VirusProcessor& m_processor;

		std::unique_ptr<Parts> m_parts;
		std::unique_ptr<Leds> m_leds;

		Rml::Element* m_presetName = nullptr;
//		PartMouseListener* m_presetNameMouseListener  = nullptr;

		// When in Multi mode, right-click toggles the patch name display between
		// the current part's Single name (default) and the Multi name.
		bool m_showMultiName = false;

		std::unique_ptr<jucePluginEditorLib::FirmwareSelector> m_romSelector;

		juceRmlUi::ElemButton* m_playModeSingle = nullptr;
		juceRmlUi::ElemButton* m_playModeMulti = nullptr;
		juceRmlUi::ElemButton* m_playModeToggle = nullptr;

		Rml::Element* m_deviceModel = nullptr;

		// 138l: part address of an audition voice started by this editor (-1 = idle).
		enum class AuditionState { Idle = 0, Held = 1, Latched = 2, Pending = 3, Unavailable = 4 };
		AuditionState refreshAudition();
		bool sendAudition(uint8_t _value);
		bool startAudition();
		bool releaseAudition();
		bool stopAudition();
		int m_auditionPart = -1;
		AuditionState m_auditionState = AuditionState::Idle;
		std::shared_ptr<std::atomic<uint64_t>> m_auditionOutcome;
		uint64_t m_auditionSerial = 0;
		bool m_auditionReleaseRequested = false;
		bool m_auditionStopRequested = false;

		std::unique_ptr<ArpUserPattern> m_arpUserPattern;

		baseLib::EventListener<uint32_t> m_romChangedListener;
		baseLib::EventListener<uint8_t> m_playModeSetListener;
	};
}
