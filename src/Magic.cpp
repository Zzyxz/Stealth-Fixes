#include "Fixes.h"

#include "Hooks.h"
#include "Settings.h"

#include <unordered_map>

namespace StealthFixes::Fixes
{
	using namespace Hooks;

	namespace
	{
		using QueueOnAttacked_t = void(void* a_pool, RE::Actor* a_victim, RE::TESObjectREFR* a_caster, RE::Effect* a_effect,
			std::int32_t a_aggression);
		using EffectStage_t = void(RE::ActiveEffect* a_effect);
		using HighestDetection_t = std::int32_t(RE::ProcessLists* a_lists, RE::Actor* a_actor, std::uint32_t* a_losCount);

		std::uintptr_t g_originalQueueOnAttacked{ 0 };
		std::uintptr_t g_originalFrenzyStart{ 0 };
		std::uintptr_t g_originalFrenzyFinish{ 0 };

		bool SomeoneNotices(RE::Actor* a_player)
		{
			static const REL::Relocation<HighestDetection_t*> func{ REL::VariantID(40394, 41408, 0x705850) };
			auto* const                                       lists = RE::ProcessLists::GetSingleton();
			std::uint32_t                                     losCount = 0;
			return !lists || func(lists, a_player, &losCount) > 0;
		}

		void QueueOnAttacked(void* a_pool, RE::Actor* a_victim, RE::TESObjectREFR* a_caster, RE::Effect* a_effect, std::int32_t a_aggression)
		{
			if (a_victim && !a_victim->IsPlayerRef() && a_caster && a_caster->IsPlayerRef() && a_effect && a_effect->baseEffect &&
				a_effect->baseEffect->GetArchetype() == RE::EffectSetting::Archetype::kFrenzy && GetSettings()->frenzyUnseen &&
				!SomeoneNotices(a_caster->As<RE::Actor>())) {
				logs::debug("{} is frenzied while nobody notices you", a_victim->GetDisplayFullName());
				return;
			}
			reinterpret_cast<QueueOnAttacked_t*>(g_originalQueueOnAttacked)(a_pool, a_victim, a_caster, a_effect, a_aggression);
		}

		struct FrenzyVictim
		{
			std::int32_t running{ 0 };
			bool         foughtPlayer{ false };
		};

		std::mutex                                                   g_frenzyLock;
		std::unordered_map<const RE::ActiveEffect*, RE::ActorHandle> g_frenzyEffects;
		std::unordered_map<std::uint32_t, FrenzyVictim>              g_frenzyVictims;

		bool FightsPlayer(RE::Actor* a_actor)
		{
			auto* const player = RE::PlayerCharacter::GetSingleton();
			auto* const group = a_actor->IsInCombat() ? a_actor->GetCombatGroup() : nullptr;
			if (!player || !group) {
				return false;
			}
			const auto                handle = player->GetHandle();
			const RE::BSReadLockGuard guard{ group->lock };
			return std::ranges::any_of(group->targets, [&](const RE::CombatTarget& a_target) { return a_target.targetHandle == handle; });
		}

		std::optional<FrenzyVictim> EndFrenzyEffect(RE::ActorHandle a_victim)
		{
			const auto it = g_frenzyVictims.find(a_victim.native_handle());
			if (it == g_frenzyVictims.end()) {
				return std::nullopt;
			}
			if (--it->second.running > 0) {
				return std::nullopt;
			}
			const auto state = it->second;
			g_frenzyVictims.erase(it);
			return state;
		}

		void FrenzyStart(RE::ActiveEffect* a_effect)
		{
			auto* const victim = a_effect ? a_effect->GetTargetActor() : nullptr;
			const bool  fought = victim && FightsPlayer(victim);
			{
				const std::lock_guard lock{ g_frenzyLock };
				if (const auto it = g_frenzyEffects.find(a_effect); it != g_frenzyEffects.end()) {
					EndFrenzyEffect(it->second);
					g_frenzyEffects.erase(it);
				}
				if (victim) {
					const auto handle = victim->GetHandle();
					g_frenzyEffects[a_effect] = handle;
					auto& state = g_frenzyVictims[handle.native_handle()];
					if (state.running++ == 0) {
						state.foughtPlayer = fought;
					}
				}
			}
			reinterpret_cast<EffectStage_t*>(g_originalFrenzyStart)(a_effect);
		}

		void FrenzyFinish(RE::ActiveEffect* a_effect)
		{
			reinterpret_cast<EffectStage_t*>(g_originalFrenzyFinish)(a_effect);
			RE::ActorHandle             victimHandle;
			std::optional<FrenzyVictim> state;
			{
				const std::lock_guard lock{ g_frenzyLock };
				const auto            it = g_frenzyEffects.find(a_effect);
				if (it == g_frenzyEffects.end()) {
					return;
				}
				victimHandle = it->second;
				g_frenzyEffects.erase(it);
				state = EndFrenzyEffect(victimHandle);
			}
			auto* const player = RE::PlayerCharacter::GetSingleton();
			const auto  victim = victimHandle.get();
			if (!state || !state->foughtPlayer || !GetSettings()->frenzyKeepsEnemies || !victim || !player || victim.get() != a_effect->GetTargetActor() ||
				victim->IsDead() || player->IsDead() || FightsPlayer(victim.get())) {
				return;
			}
			auto* const process = victim->GetActorRuntimeData().currentProcess;
			if (!process || victim->RequestDetectionLevel(player, RE::DETECTION_PRIORITY::kNormal) <= 0) {
				return;
			}
			using StartCombat_t = void(RE::AIProcess*, RE::Actor*, RE::Actor*, RE::CombatGroup*);
			static const REL::Relocation<StartCombat_t*> startCombat{ REL::VariantID(38375, 39359, 0x658FA0) };
			startCombat(process, victim.get(), player, nullptr);
			logs::debug("{} fights on after the frenzy", victim->GetDisplayFullName());
		}

		void InstallFrenzy()
		{
			const auto queue = CallAt(REL::CallsiteID{ REL::RelocationID(37832, 38786) }, REL::VariantOffset(0x27C, 0x373, 0x27C),
				REL::CallsiteID{ REL::RelocationID(36013, 36988), 0x5CF250 });
			if (queue) {
				g_originalQueueOnAttacked = SKSE::GetTrampoline().write_call<5>(queue, QueueOnAttacked);
			}
			logs::info("Frenzy from hiding: {}", queue ? "active" : "not active - another mod probably changes the same place");
			REL::Relocation<std::uintptr_t> vtable{ RE::VTABLE_FrenzyEffect[0] };
			g_originalFrenzyStart = vtable.write_vfunc(0x14, FrenzyStart);
			g_originalFrenzyFinish = vtable.write_vfunc(0x15, FrenzyFinish);
			logs::info("Enemies stay enemies after a frenzy: active");
		}
	}

	void ResetMagic()
	{
		const std::lock_guard lock{ g_frenzyLock };
		g_frenzyEffects.clear();
		g_frenzyVictims.clear();
	}

	void InstallMagic()
	{
		InstallFrenzy();
	}
}
