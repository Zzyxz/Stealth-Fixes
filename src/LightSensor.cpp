#include "LightSensor.h"

#include "Fixes.h"

namespace StealthFixes::LightSensor
{
	namespace
	{
		bool ShadowLightReaches(RE::BSLight* a_light, RE::Actor* a_actor, RE::NiPoint3& a_point)
		{
			using func_t = bool(void*, RE::BSLight*, RE::Actor*, RE::NiPoint3*);
			static REL::Relocation<func_t> func{ RELOCATION_ID(41661, 42744) };
			static REL::Relocation<void**> manager{ RELOCATION_ID(518074, 404596) };
			return func(*manager, a_light, a_actor, &a_point);
		}

		bool LampReaches(RE::BSLight* a_light, RE::Actor* a_actor, const RE::NiPoint3& a_point)
		{
			auto* niLight = a_light ? a_light->light.get() : nullptr;
			auto* tes = RE::TES::GetSingleton();
			if (!niLight || !tes) {
				return true;
			}
			const float     scale = RE::bhkWorld::GetWorldScale();
			const auto&     from = niLight->world.translate;
			RE::bhkPickData pick;
			pick.rayInput.from = RE::hkVector4(from.x * scale, from.y * scale, from.z * scale, 0.0f);
			pick.rayInput.to = RE::hkVector4(a_point.x * scale, a_point.y * scale, a_point.z * scale, 0.0f);
			std::uint32_t filter = 0;
			a_actor->GetCollisionFilterInfo(filter);
			pick.rayInput.filterInfo = (filter & 0xFFFF0000) | static_cast<std::uint32_t>(RE::COL_LAYER::kLOS);
			auto* hit = tes->Pick(pick);
			if (!hit) {
				return true;
			}
			return RE::TESObjectREFR::FindReferenceFor3D(hit) == a_actor;
		}

		bool LightReaches(RE::BSLight* a_light, bool a_shadowLight, RE::Actor* a_actor, RE::NiPoint3& a_point)
		{
			return a_shadowLight ? ShadowLightReaches(a_light, a_actor, a_point) : LampReaches(a_light, a_actor, a_point);
		}

		float Average(const RE::NiColor& a_color)
		{
			return (a_color.red + a_color.green + a_color.blue) * (1.0f / 3.0f);
		}

		float LampAt(RE::BSLight* a_light, const RE::NiPoint3& a_point)
		{
			auto* niLight = a_light ? a_light->light.get() : nullptr;
			if (!niLight) {
				return 0.0f;
			}
			const auto& data = niLight->GetLightRuntimeData();
			float       lum = Average(data.diffuse) * data.fade;
			if (a_light->pointLight) {
				const float radius = data.radius.x;
				if (radius <= 0.0f) {
					return 0.0f;
				}
				const float t = std::clamp(niLight->world.translate.GetDistance(a_point) / radius, 0.0f, 1.0f);
				lum *= 1.0f - t * t;
			}
			return Fixes::CorrectLampLight(a_light, lum);
		}

		bool LitLikeInterior(RE::PlayerCharacter* a_player)
		{
			const auto* cell = a_player->GetParentCell();
			return cell && cell->IsInteriorCell();
		}

		void SceneLight(RE::ShadowSceneNode::RUNTIME_DATA& a_scene, float& a_ambient, float& a_sunMoon)
		{
			a_ambient = 0.0f;
			a_sunMoon = 0.0f;
			if (auto* sun = a_scene.sunLight; sun && sun->light) {
				const auto& data = sun->light->GetLightRuntimeData();
				a_ambient = Average(data.ambient) * 100.0f;
				float factor = 1.0f;
				if (auto* imageSpace = RE::ImageSpaceManager::GetSingleton()) {
					const std::ptrdiff_t at = REL::Module::IsVR() ? 0xF0 : 0xE0;
					factor = *reinterpret_cast<const float*>(reinterpret_cast<const std::uint8_t*>(imageSpace) + at);
				}
				a_sunMoon = Average(data.diffuse) * data.fade * factor * 100.0f;
			}
		}

		struct LightLists
		{
			std::vector<RE::NiPointer<RE::BSLight>> plain;
			std::vector<RE::NiPointer<RE::BSLight>> shadow;
		};

		const LightLists& Lights(RE::ShadowSceneNode::RUNTIME_DATA& a_scene)
		{
			using Clock = std::chrono::steady_clock;
			static LightLists        lists;
			static Clock::time_point copied{};
			static const void*       from = nullptr;
			const auto               now = Clock::now();
			if (from != &a_scene || now - copied > std::chrono::milliseconds(50)) {
				lists.plain.assign(a_scene.activeLights.begin(), a_scene.activeLights.end());
				lists.shadow.assign(a_scene.unk148.begin(), a_scene.unk148.end());
				copied = now;
				from = &a_scene;
			}
			return lists;
		}

		constexpr float kNudge = 8.0f;

		RE::NiPoint3 TowardLamp(RE::BSLight* a_light, const RE::NiPoint3& a_point)
		{
			auto* niLight = a_light ? a_light->light.get() : nullptr;
			if (!niLight) {
				return a_point;
			}
			auto       direction = niLight->world.translate - a_point;
			const auto length = direction.Length();
			return length > kNudge ? a_point + direction * (kNudge / length) : a_point;
		}

		RE::NiPoint3 TowardSun(RE::BSLight* a_light, const RE::NiPoint3& a_point)
		{
			auto* niLight = a_light ? a_light->light.get() : nullptr;
			if (!niLight) {
				return a_point;
			}
			const auto& rotate = niLight->world.rotate;
			return a_point - RE::NiPoint3{ rotate.entry[0][0], rotate.entry[1][0], rotate.entry[2][0] } * kNudge;
		}
	}

	float BaseLight()
	{
		auto* player = RE::PlayerCharacter::GetSingleton();
		auto& state = RE::BSShaderManager::State::GetSingleton();
		auto* ssn = state.shadowSceneNode[0];
		if (!player || !player->GetParentCell() || !ssn) {
			return -1.0f;
		}
		float ambient = 0.0f;
		float sunMoon = 0.0f;
		SceneLight(ssn->GetRuntimeData(), ambient, sunMoon);
		return ambient + (LitLikeInterior(player) ? sunMoon : 0.0f);
	}

	float MeasureAt(const RE::NiPoint3& a_point, RE::Actor* a_ignore)
	{
		auto* player = RE::PlayerCharacter::GetSingleton();
		auto& state = RE::BSShaderManager::State::GetSingleton();
		auto* ssn = state.shadowSceneNode[0];
		if (!player || !player->GetParentCell() || !ssn) {
			return -1.0f;
		}
		auto&      scene = ssn->GetRuntimeData();
		RE::Actor* ignore = a_ignore ? a_ignore : player;

		const auto* sunShadow = reinterpret_cast<RE::BSLight*>(scene.shadowDirLight);
		float       lamps = 0.0f;
		const auto  addLamp = [&](RE::BSLight* a_light, bool a_shadowCaster) {
            const float lum = LampAt(a_light, a_point);
            if (lum <= 0.0f) {
                return;
            }
            auto point = TowardLamp(a_light, a_point);
            if (LightReaches(a_light, a_shadowCaster, ignore, point)) {
                lamps += lum * 100.0f;
            }
		};
		const auto& lists = Lights(scene);
		for (const auto& light : lists.plain) {
			if (light.get() != sunShadow) {
				addLamp(light.get(), false);
			}
		}
		for (const auto& light : lists.shadow) {
			if (light.get() != sunShadow) {
				addLamp(light.get(), true);
			}
		}

		float ambient = 0.0f;
		float sunMoon = 0.0f;
		SceneLight(scene, ambient, sunMoon);
		float share = 1.0f;
		if (!LitLikeInterior(player)) {
			if (auto* dirLight = reinterpret_cast<RE::BSLight*>(scene.shadowDirLight)) {
				auto point = TowardSun(dirLight, a_point);
				share = LightReaches(dirLight, true, ignore, point) ? 1.0f : 0.0f;
			}
		}
		return lamps + ambient + sunMoon * share;
	}
}
