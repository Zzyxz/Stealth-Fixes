#include "Fixes.h"

#include "GameSettings.h"
#include "Hooks.h"
#include "Settings.h"

namespace StealthFixes::Fixes
{
	using namespace Hooks;

	namespace
	{
		RE::TESFaction* CrimeFaction(const void* a_crime)
		{
			return *reinterpret_cast<RE::TESFaction* const*>(static_cast<const std::uint8_t*>(a_crime) + 0x60);
		}

		RE::NiPointer<RE::Actor> Criminal(const void* a_crime)
		{
			RE::RefHandle handle;
			std::memcpy(&handle, static_cast<const std::uint8_t*>(a_crime) + 0x0C, sizeof(handle));
			return ActorFrom(handle);
		}
	}

	namespace
	{
		struct CrimeNode
		{
			const void*      item;
			const CrimeNode* next;
		};

		bool HasCrime(RE::BSSimpleList<RE::Crime*>* a_list, RE::TESFaction* a_faction, const void* a_exclude, bool a_known)
		{
			for (auto* node = reinterpret_cast<const CrimeNode*>(a_list); node; node = node->next) {
				if (!node->item || node->item == a_exclude || CrimeFaction(node->item) != a_faction) {
					continue;
				}
				const auto*   crime = static_cast<const std::uint8_t*>(node->item);
				std::uint32_t witnesses;
				std::int32_t  bounty;
				std::memcpy(&witnesses, crime + 0x38, sizeof(witnesses));
				std::memcpy(&bounty, crime + 0x58, sizeof(bounty));
				if (!a_known || witnesses > 0 || bounty > 0) {
					return true;
				}
			}
			return false;
		}

		bool HasOtherViolentCrime(RE::ProcessLists* a_lists, RE::TESFaction* a_faction, const void* a_exclude)
		{
			if (!a_lists) {
				return false;
			}
			if (HasCrime(a_lists->globalCrimes[RE::PackageNS::CRIME_TYPES::kAttack], a_faction, a_exclude, false)) {
				return true;
			}
			return GetSettings()->openMurderKeepsEnemy && HasCrime(a_lists->globalCrimes[RE::PackageNS::CRIME_TYPES::kMurder], a_faction, a_exclude, true);
		}

		void InstallOpenMurder()
		{
			constexpr std::array<std::uint8_t, 5> kPrologue{ 0x48, 0x89, 0x5C, 0x24, 0x08 };
			const auto                            function = REL::CallsiteID{ REL::RelocationID(40344, 41357), 0x701A30 }.rva();
			const bool                            active = function && JumpAt(REL::Module::get().base() + *function, kPrologue,
																		   reinterpret_cast<const void*>(&HasOtherViolentCrime));
			logs::info("An open murder keeps the faction hostile: {}", active ? "active" : "not active - another mod probably changes the same place");
		}
	}

	namespace
	{
		using MurderCheck_t = void(RE::Actor* a_victim, RE::Actor* a_killer, std::uintptr_t a_r8, std::uintptr_t a_r9);
		using GuardInAlarm_t = bool(RE::ProcessLists* a_lists);

		std::uintptr_t g_originalMurderCheck{ 0 };
		std::uintptr_t g_originalGuardInAlarm{ 0 };

		thread_local RE::Actor* t_murderVictim{ nullptr };

		void MurderCheck(RE::Actor* a_victim, RE::Actor* a_killer, std::uintptr_t a_r8, std::uintptr_t a_r9)
		{
			auto* const previous = std::exchange(t_murderVictim, a_victim);
			reinterpret_cast<MurderCheck_t*>(g_originalMurderCheck)(a_victim, a_killer, a_r8, a_r9);
			t_murderVictim = previous;
		}

		bool InAlarm(const RE::Actor* a_guard)
		{
			const auto* process = a_guard->GetActorRuntimeData().currentProcess;
			const auto* package = process ? *reinterpret_cast<const std::uint8_t* const*>(reinterpret_cast<const std::uint8_t*>(process) + 0x20) : nullptr;
			return package && package[0x24] == 21;
		}

		bool SameSpace(RE::Actor* a_one, RE::Actor* a_other)
		{
			auto* const space = a_one->GetWorldspace();
			return space ? space == a_other->GetWorldspace() : a_one->GetParentCell() == a_other->GetParentCell();
		}

		bool GuardInAlarm(RE::ProcessLists* a_lists)
		{
			auto* const victim = t_murderVictim;
			if (!victim || !a_lists || !GetSettings()->guardAlarmNearby) {
				return reinterpret_cast<GuardInAlarm_t*>(g_originalGuardInAlarm)(a_lists);
			}
			auto* const faction = victim->GetCrimeFaction();
			if (!faction) {
				return false;
			}
			const auto  distance = static_cast<float>(GameSettings::Int("iCrimeAlarmRecDistance", 4000));
			const auto& position = victim->GetPosition();
			for (const auto* list : a_lists->allProcesses) {
				if (!list) {
					continue;
				}
				for (const auto& handle : *list) {
					const auto guard = handle.get();
					if (guard && guard.get() != victim && guard->IsGuard() && InAlarm(guard.get()) && !guard->IsDead() && SameSpace(guard.get(), victim) &&
						guard->GetPosition().GetSquaredDistance(position) <= distance * distance && InSharedCrimeFaction(guard.get(), faction)) {
						return true;
					}
				}
			}
			return false;
		}

		void InstallGuardAlarm()
		{
			const REL::CallsiteID murder{ REL::RelocationID(36431, 37426), 0x5E7570 };
			const auto            kill = CallAt(REL::CallsiteID{ REL::RelocationID(36723, 37735) }, REL::VariantOffset(0x8F, 0xA2, 0x8F), murder);
			const auto            alarm = CallAt(murder, REL::VariantOffset(0xE4, 0xE4, 0xE4), REL::CallsiteID{ REL::RelocationID(40332, 41342), 0x7004B0 });
			if (!kill || !alarm) {
				logs::info("Murders reported by alarmed guards nearby only: not active - another mod probably changes the same place");
				return;
			}
			auto& trampoline = SKSE::GetTrampoline();
			g_originalMurderCheck = trampoline.write_call<5>(kill, MurderCheck);
			g_originalGuardInAlarm = trampoline.write_call<5>(alarm, GuardInAlarm);
			logs::info("Murders reported by alarmed guards nearby only: active");
		}
	}

	namespace
	{
		using AlarmReact_t = std::uintptr_t(RE::Actor* a_receiver, void* a_crime, std::uintptr_t a_unknown, bool a_flag, void* a_data);
		using AddKnownActor_t = void(void* a_crime, const RE::ActorHandle* a_actor);

		std::uintptr_t g_originalAlarmReact{ 0 };
		std::uintptr_t g_originalReactWitness{ 0 };
		std::uintptr_t g_originalDispatchWitness{ 0 };

		thread_local bool t_alarmReaction{ false };

		std::uintptr_t AlarmReact(RE::Actor* a_receiver, void* a_crime, std::uintptr_t a_unknown, bool a_flag, void* a_data)
		{
			const bool previous = std::exchange(t_alarmReaction, true);
			const auto result = reinterpret_cast<AlarmReact_t*>(g_originalAlarmReact)(a_receiver, a_crime, a_unknown, a_flag, a_data);
			t_alarmReaction = previous;
			return result;
		}

		bool SeesCriminal(void* a_crime, const RE::ActorHandle* a_actor)
		{
			if (!a_crime || !a_actor || !GetSettings()->alarmWitnessesSee) {
				return true;
			}
			const auto receiver = a_actor->get();
			const auto criminal = Criminal(a_crime);
			if (!receiver || !criminal) {
				return true;
			}
			return receiver->RequestDetectionLevel(criminal.get(), RE::DETECTION_PRIORITY::kNormal) > 0;
		}

		void ReactWitness(void* a_crime, const RE::ActorHandle* a_actor)
		{
			if (!t_alarmReaction || SeesCriminal(a_crime, a_actor)) {
				reinterpret_cast<AddKnownActor_t*>(g_originalReactWitness)(a_crime, a_actor);
			}
		}

		void DispatchWitness(void* a_crime, const RE::ActorHandle* a_actor)
		{
			if (SeesCriminal(a_crime, a_actor)) {
				reinterpret_cast<AddKnownActor_t*>(g_originalDispatchWitness)(a_crime, a_actor);
			}
		}

		void InstallAlarmWitnesses()
		{
			const REL::CallsiteID dispatcher{ REL::RelocationID(38667, 39695), 0x67D8B0 };
			const REL::CallsiteID react{ REL::RelocationID(36553, 37554), 0x5F3450 };
			const REL::CallsiteID addKnown{ REL::RelocationID(41437, 42506), 0x737180 };
			const auto            reaction = CallAt(dispatcher, REL::VariantOffset(0x325, 0x3B7, 0x325), react);
			const auto            reactWitness = CallAt(react, REL::VariantOffset(0x810, 0x827, 0x810), addKnown);
			const auto            dispatchWitness = CallAt(dispatcher, REL::VariantOffset(0x338, 0x3CA, 0x338), addKnown);
			if (!reaction || !reactWitness || !dispatchWitness) {
				logs::info("Alarmed NPCs as witnesses only when they see you: not active - another mod probably changes the same place");
				return;
			}
			auto& trampoline = SKSE::GetTrampoline();
			g_originalAlarmReact = trampoline.write_call<5>(reaction, AlarmReact);
			g_originalReactWitness = trampoline.write_call<5>(reactWitness, ReactWitness);
			g_originalDispatchWitness = trampoline.write_call<5>(dispatchWitness, DispatchWitness);
			logs::info("Alarmed NPCs as witnesses only when they see you: {}", GetSettings()->alarmWitnessesSee ? "active" : "ready (off in the settings)");
		}
	}

	void InstallCrimes()
	{
		InstallOpenMurder();
		InstallGuardAlarm();
		InstallAlarmWitnesses();
	}
}
