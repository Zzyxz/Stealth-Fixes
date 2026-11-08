#include "Fixes.h"

#include "Hooks.h"
#include "Settings.h"

namespace StealthFixes::Fixes
{
	using namespace Hooks;

	namespace
	{
		using ProcessReactions_t = bool(RE::ActorKnowledge* a_knowledge);
		using Reaction_t = bool(RE::Actor* a_observer, RE::Actor* a_target);

		enum class Reaction
		{
			kArrest,
			kAggression,
			kOwnedCell,
			kTrespasser,
			kFlee,
			kTotal
		};

		std::uintptr_t                                                         g_originalProcessReactions{ 0 };
		std::array<std::uintptr_t, static_cast<std::size_t>(Reaction::kTotal)> g_originalReaction{};

		thread_local std::optional<std::int32_t> t_reactionLevel;

		bool ProcessReactions(RE::ActorKnowledge* a_knowledge)
		{
			std::optional<std::int32_t> level;
			if (a_knowledge && a_knowledge->detectionState) {
				level = a_knowledge->detectionState->level;
			}
			const auto previous = std::exchange(t_reactionLevel, level);
			const bool result = reinterpret_cast<ProcessReactions_t*>(g_originalProcessReactions)(a_knowledge);
			t_reactionLevel = previous;
			return result;
		}

		bool OnlyHeard()
		{
			return t_reactionLevel && *t_reactionLevel == 0 && GetSettings()->reactionsNeedSight;
		}

		template <Reaction Which>
		bool ReactionTest(RE::Actor* a_observer, RE::Actor* a_target)
		{
			const bool result = reinterpret_cast<Reaction_t*>(g_originalReaction[static_cast<std::size_t>(Which)])(a_observer, a_target);
			if (!result || !a_target || !a_target->IsPlayerRef() || !OnlyHeard()) {
				return result;
			}
			if constexpr (Which == Reaction::kAggression) {
				return a_observer && a_observer->IsInCombat();
			} else {
				return false;
			}
		}

		void InstallReactionsNeedSight()
		{
			auto&                 trampoline = SKSE::GetTrampoline();
			const REL::CallsiteID process{ REL::RelocationID(38739, 39765), 0x681350 };
			const auto            listener = CallAt(REL::CallsiteID{ REL::RelocationID(38733, 39761), 0x680F20 }, REL::VariantOffset(0x39, 0x49, 0x39), process);
			struct Test
			{
				Reaction           which;
				REL::CallsiteID    target;
				REL::VariantOffset offset;
				Reaction_t*        function;
			};
			const std::array<Test, 5>     tests{ {
                { Reaction::kArrest, REL::CallsiteID{ REL::RelocationID(38716, 39748), 0x6803D0 }, REL::VariantOffset(0xB0, 0xB9, 0xB0),
						ReactionTest<Reaction::kArrest> },
                { Reaction::kAggression, REL::CallsiteID{ REL::RelocationID(38714, 39746), 0x680230 }, REL::VariantOffset(0x15A, 0x163, 0x15A),
						ReactionTest<Reaction::kAggression> },
                { Reaction::kOwnedCell, REL::CallsiteID{ REL::RelocationID(38723, 39754), 0x680A10 }, REL::VariantOffset(0x228, 0x231, 0x228),
						ReactionTest<Reaction::kOwnedCell> },
                { Reaction::kTrespasser, REL::CallsiteID{ REL::RelocationID(38718, 39750), 0x680680 }, REL::VariantOffset(0x2CB, 0x324, 0x2CB),
						ReactionTest<Reaction::kTrespasser> },
                { Reaction::kFlee, REL::CallsiteID{ REL::RelocationID(38717, 39749), 0x680550 }, REL::VariantOffset(0x2F7, 0x350, 0x2F7),
						ReactionTest<Reaction::kFlee> },
            } };
			std::array<std::uintptr_t, 5> sites{};
			bool                          all = listener != 0;
			for (std::size_t i = 0; i < tests.size(); ++i) {
				sites[i] = CallAt(process, tests[i].offset, tests[i].target);
				all = all && sites[i] != 0;
			}
			if (!all) {
				logs::info("Noticing is not detecting: not active - another mod probably changes the same place");
				return;
			}
			g_originalProcessReactions = trampoline.write_call<5>(listener, ProcessReactions);
			for (std::size_t i = 0; i < tests.size(); ++i) {
				g_originalReaction[static_cast<std::size_t>(tests[i].which)] = trampoline.write_call<5>(sites[i], tests[i].function);
			}
			logs::info("Noticing is not detecting: active");
		}
	}

	namespace
	{
		using IsTalking_t = bool(RE::Actor* a_actor);
		using IsGreeting_t = bool(RE::AIProcess* a_process);
		using Detection_t = std::int32_t(RE::Actor* a_observer, RE::Actor* a_target, std::uint32_t a_priority);

		std::uintptr_t g_originalBowTalking{ 0 };
		std::uintptr_t g_originalBowGreeting{ 0 };
		std::uintptr_t g_originalBowDetection{ 0 };

		thread_local bool t_bowTalking{ false };
		thread_local bool t_bowGreeting{ false };

		bool BowTalking(RE::Actor* a_actor)
		{
			t_bowTalking = reinterpret_cast<IsTalking_t*>(g_originalBowTalking)(a_actor);
			t_bowGreeting = false;
			return GetSettings()->bowCommentNeedsSight || t_bowTalking;
		}

		bool BowGreeting(RE::AIProcess* a_process)
		{
			t_bowGreeting = a_process && reinterpret_cast<IsGreeting_t*>(g_originalBowGreeting)(a_process);
			return GetSettings()->bowCommentNeedsSight || t_bowGreeting;
		}

		std::int32_t BowDetection(RE::Actor* a_observer, RE::Actor* a_target, std::uint32_t a_priority)
		{
			const auto level = reinterpret_cast<Detection_t*>(g_originalBowDetection)(a_observer, a_target, a_priority);
			if (GetSettings()->bowCommentNeedsSight && (t_bowTalking || t_bowGreeting)) {
				return 0;
			}
			return level;
		}

		void InstallBowComment()
		{
			const REL::CallsiteID owner{ REL::RelocationID(40427, 41442), 0x707E30 };
			const std::size_t     calls = REL::Module::IsAE() ? 2 : 1;
			const auto            talking = CallAt(owner, REL::VariantOffset(0x100, 0x101, 0x100), REL::CallsiteID{ REL::RelocationID(36277, 37266), 0x5DA8F0 }, 0, calls);
			const auto            greeting = CallAt(owner, REL::VariantOffset(0x110, 0x111, 0x110), REL::CallsiteID{ REL::RelocationID(38370, 39353), 0x658CD0 }, 0, calls);
			const auto            detection = CallAt(owner, REL::VariantOffset(0x127, 0x128, 0x127), REL::CallsiteID{ REL::RelocationID(36748, 37764) }, 0, calls);
			if (!talking || !greeting || !detection) {
				logs::info("Comments on your bow need a sighting: not active - another mod probably changes the same place");
				return;
			}
			auto& trampoline = SKSE::GetTrampoline();
			g_originalBowTalking = trampoline.write_call<5>(talking, BowTalking);
			g_originalBowGreeting = trampoline.write_call<5>(greeting, BowGreeting);
			g_originalBowDetection = trampoline.write_call<5>(detection, BowDetection);
			logs::info("Comments on your bow need a sighting: active");
		}
	}

	void InstallReactions()
	{
		InstallReactionsNeedSight();
		InstallBowComment();
	}
}
