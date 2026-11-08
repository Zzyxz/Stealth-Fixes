#include "GameSettings.h"

namespace StealthFixes::GameSettings
{
	namespace
	{
		constexpr std::array kNames{
			"fSneakBaseValue"sv,
			"fSneakSkillMult"sv,
			"fSneakPerceptionSkillMin"sv,
			"fSneakPerceptionSkillMax"sv,
			"fPlayerDetectionSneakBase"sv,
			"fPlayerDetectionSneakMult"sv,
			"fSneakMaxDistance"sv,
			"fSneakExteriorDistanceMult"sv,
			"fSneakFlyingDistanceMult"sv,
			"fSneakDistanceAttenuationExponent"sv,
			"fDetectionSneakLightMod"sv,
			"fSneakLightMult"sv,
			"fSneakLightExteriorMult"sv,
			"fSneakLightMoveMult"sv,
			"fSneakLightRunMult"sv,
			"iLightLevelInteriorMod"sv,
			"iLightLevelExteriorMod"sv,
			"iLightLevelMax"sv,
			"fDetectionNightEyeBonus"sv,
			"fDetectionViewCone"sv,
			"fSneakSoundsMult"sv,
			"fSneakSoundLosMult"sv,
			"fSneakEquippedWeightBase"sv,
			"fSneakEquippedWeightMult"sv,
			"fSneakRunningMult"sv,
			"fSneakActionMult"sv,
			"iSoundLevelVeryLoud"sv,
			"iSoundLevelNormal"sv,
			"iSoundLevelLoud"sv,
			"iSoundLevelSilent"sv,
			"fDetectEventSneakDistanceVeryLoud"sv,
			"fSneakAlertMod"sv,
			"fSneakCombatMod"sv,
			"fSneakSleepMod"sv,
			"fSneakSleepBonus"sv,
			"fSneakAmbushTargetMod"sv,
			"fSneakAmbushNonTargetMod"sv,
			"fDetectionLargeActorSizeMult"sv,
			"fLightRecalcTimerPlayer"sv,
			"fJogInterpolationBetweenWalkAndRun"sv,
			"iCrimeAlarmRecDistance"sv,
		};

		std::array<RE::Setting*, kNames.size()> g_settings{};

		RE::Setting* Find(std::string_view a_name)
		{
			for (std::size_t i = 0; i < kNames.size(); ++i) {
				if (kNames[i] == a_name) {
					return g_settings[i];
				}
			}
			return nullptr;
		}
	}

	void Cache()
	{
		auto* collection = RE::GameSettingCollection::GetSingleton();
		if (!collection) {
			return;
		}
		std::size_t missing = 0;
		for (std::size_t i = 0; i < kNames.size(); ++i) {
			const std::string name{ kNames[i] };
			g_settings[i] = collection->GetSetting(name.c_str());
			if (!g_settings[i]) {
				++missing;
			}
		}
		if (missing) {
			logs::warn("{} detection game settings were not found; the fixes use the base game's values for them.", missing);
		}
	}

	float Float(std::string_view a_name, float a_fallback)
	{
		const auto* setting = Find(a_name);
		return setting ? setting->GetFloat() : a_fallback;
	}

	std::int32_t Int(std::string_view a_name, std::int32_t a_fallback)
	{
		const auto* setting = Find(a_name);
		return setting ? setting->GetSInt() : a_fallback;
	}

	std::vector<std::string> Describe()
	{
		std::vector<std::string> out;
		for (std::size_t i = 0; i < kNames.size(); ++i) {
			const auto* setting = g_settings[i];
			if (!setting) {
				out.push_back(std::format("{}=missing", kNames[i]));
			} else if (kNames[i].front() == 'i') {
				out.push_back(std::format("{}={}", kNames[i], setting->GetSInt()));
			} else {
				out.push_back(std::format("{}={:g}", kNames[i], setting->GetFloat()));
			}
		}
		return out;
	}
}
