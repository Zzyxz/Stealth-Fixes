#include "Fixes.h"

#include "Hooks.h"
#include "Settings.h"

namespace StealthFixes::Fixes
{
	using namespace Hooks;

	namespace
	{
		using Luminance_t = float(RE::BSLight* a_light, const RE::NiPoint3* a_point, RE::NiLight* a_exclude);

		std::uintptr_t g_originalLuminanceAll{ 0 };
		std::uintptr_t g_originalLuminanceShadow{ 0 };

		std::optional<float> MeanDimmer(const RE::TESObjectLIGH* a_light)
		{
			const auto  flags = a_light->data.flags;
			const float amplitude = a_light->data.flickerIntensityAmplitude;
			if (flags.any(RE::TES_LIGHT_FLAGS::kFlicker)) {
				return a_light->fade * (1.0f - amplitude * 11.0f / 24.0f);
			}
			if (flags.any(RE::TES_LIGHT_FLAGS::kPulse, RE::TES_LIGHT_FLAGS::kPulseSlow) && amplitude <= 2.0f) {
				return a_light->fade * (1.0f - amplitude * 0.5f);
			}
			return std::nullopt;
		}

		const RE::TESObjectLIGH* PlayerTorch(RE::PlayerCharacter* a_player)
		{
			for (const bool left : { true, false }) {
				if (auto* const object = a_player->GetEquippedObject(left); object && object->Is(RE::FormType::Light)) {
					return object->As<RE::TESObjectLIGH>();
				}
			}
			return nullptr;
		}

		float Correct(RE::BSLight* a_light, float a_luminance)
		{
			if (a_luminance <= 0.0f || !a_light) {
				return a_luminance;
			}
			auto* const light = a_light->light.get();
			if (!light) {
				return a_luminance;
			}
			const auto settings = GetSettings();
			if (settings->hiddenLightsOff && light->GetFlags().any(RE::NiAVObject::Flag::kHidden)) {
				return 0.0f;
			}
			if (!settings->steadyTorch) {
				return a_luminance;
			}
			auto* const player = RE::PlayerCharacter::GetSingleton();
			if (!player) {
				return a_luminance;
			}
			const auto& info = player->GetInfoRuntimeData();
			if (a_light != info.firstPersonLight.get() && a_light != info.thirdPersonLight.get()) {
				return a_luminance;
			}
			const auto* torch = PlayerTorch(player);
			const auto  mean = torch ? MeanDimmer(torch) : std::nullopt;
			const float dimmer = light->GetLightRuntimeData().fade;
			if (!mean || *mean <= 0.0f || dimmer <= 1e-4f) {
				return a_luminance;
			}
			return a_luminance * *mean / dimmer;
		}

		float LuminanceAll(RE::BSLight* a_light, const RE::NiPoint3* a_point, RE::NiLight* a_exclude)
		{
			return Correct(a_light, reinterpret_cast<Luminance_t*>(g_originalLuminanceAll)(a_light, a_point, a_exclude));
		}

		float LuminanceShadow(RE::BSLight* a_light, const RE::NiPoint3* a_point, RE::NiLight* a_exclude)
		{
			return Correct(a_light, reinterpret_cast<Luminance_t*>(g_originalLuminanceShadow)(a_light, a_point, a_exclude));
		}
	}

	float CorrectLampLight(RE::BSLight* a_light, float a_luminance)
	{
		return Correct(a_light, a_luminance);
	}

	void InstallLights()
	{
		const REL::CallsiteID sum{ REL::RelocationID(99725, 106362) };
		const REL::CallsiteID luminance{ REL::RelocationID(101303, 108292) };
		const auto            all = CallAt(sum, REL::VariantOffset(0x6F, 0x83, 0x6F), luminance, 0, 2);
		const auto            shadow = CallAt(sum, REL::VariantOffset(0xF3, 0xF4, 0xF3), luminance, 1, 2);
		if (!all || !shadow) {
			logs::info("Your torch counts once and steadily: not active - another mod probably changes the same place");
			return;
		}
		auto& trampoline = SKSE::GetTrampoline();
		g_originalLuminanceAll = trampoline.write_call<5>(all, LuminanceAll);
		g_originalLuminanceShadow = trampoline.write_call<5>(shadow, LuminanceShadow);
		logs::info("Your torch counts once and steadily: active");
	}
}
