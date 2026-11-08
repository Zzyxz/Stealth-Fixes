#include "Unconscious.h"

#include "Settings.h"

#include <random>

namespace StealthFixes::Unconscious
{
	namespace
	{
		using Clock = std::chrono::steady_clock;

		constexpr auto kSettleTime = std::chrono::milliseconds(1500);

		struct Snapshot
		{
			float             health{ 0.0f };
			float             maxHealth{ 0.0f };
			Clock::time_point since{};
			bool              hitPending{ false };
		};

		std::mutex                               g_lock;
		std::unordered_map<RE::FormID, Snapshot> g_snapshots;

		RE::SpellItem*  g_sneakToolsKnockout{ nullptr };
		RE::TESFaction* g_sneakToolsUnconscious{ nullptr };

		struct Rescue
		{
			RE::ActorHandle   ally;
			RE::TESPackage*   package{ nullptr };
			Clock::time_point started{};
			Clock::time_point arrived{};
		};
		std::unordered_map<RE::FormID, Rescue>            g_rescues;
		std::unordered_map<RE::FormID, Clock::time_point> g_noRescueUntil;
		Clock::time_point                                 g_lastLook{};

		constexpr float kUnitsPerMetre = 70.0f;
		constexpr float kReachedDistance = 160.0f;
		constexpr auto  kLookStep = std::chrono::seconds(1);
		constexpr auto  kRescueTimeout = std::chrono::seconds(40);
		constexpr auto  kRescueCooldown = std::chrono::seconds(30);
		constexpr auto  kGuardTime = std::chrono::seconds(30);

		std::unordered_map<RE::FormID, Clock::time_point> g_onGuard;
		bool                                              g_installed{ false };

		RE::TESPackage* CreatePackage(RE::PACKAGE_PROCEDURE_TYPE a_type)
		{
			using func_t = RE::TESPackage* (*)(std::int32_t);
			static REL::Relocation<func_t> func{ RELOCATION_ID(28732, 29496) };
			return func(static_cast<std::int32_t>(a_type));
		}

		void SetPackageLocation(RE::TESPackage* a_package, const RE::PackageLocation* a_location)
		{
			using func_t = void            (*)(RE::TESPackage*, const RE::PackageLocation*);
			static REL::Relocation<func_t> func{ RELOCATION_ID(28752, 29526) };
			func(a_package, a_location);
		}

		struct alignas(8) NearReference
		{
			std::uintptr_t vtable;
			std::uint8_t   type{ 0 };
			std::uint8_t   pad09[3]{};
			std::uint32_t  radius;
			std::uint32_t  handle;
			std::uint32_t  pad14{ 0 };
		};
		static_assert(sizeof(NearReference) == sizeof(RE::PackageLocation));

		bool IsUnconscious(RE::Actor* a_actor)
		{
			return a_actor && !a_actor->IsDead() && a_actor->AsActorState()->IsUnconscious();
		}

		std::string_view Name(RE::Actor* a_actor)
		{
			const char* name = a_actor ? a_actor->GetName() : nullptr;
			return name && name[0] != '\0' ? name : "someone";
		}

		bool CallActorScript(RE::Actor* a_actor, const char* a_function, bool a_value)
		{
			auto* vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
			auto* policy = vm ? vm->GetObjectHandlePolicy() : nullptr;
			if (!policy) {
				return false;
			}
			const auto handle = policy->GetHandleForObject(RE::Actor::FORMTYPE, a_actor);
			if (handle == policy->EmptyHandle()) {
				return false;
			}
			RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> result;
			auto*                                                    args = RE::MakeFunctionArguments(std::move(a_value));
			const bool                                               sent = vm->DispatchMethodCall2(handle, "Actor", a_function, args, result);
			delete args;
			return sent;
		}

		bool SetAlerted(RE::Actor* a_actor, bool a_alerted)
		{
			auto* process = a_actor ? a_actor->GetActorRuntimeData().currentProcess : nullptr;
			if (!process) {
				return false;
			}
			if (a_alerted) {
				process->lowProcessFlags.set(RE::AIProcess::LowProcessFlags::kAlert);
			} else {
				process->lowProcessFlags.reset(RE::AIProcess::LowProcessFlags::kAlert);
			}
			return true;
		}

		void PutOnGuard(RE::Actor* a_actor)
		{
			if (a_actor && !a_actor->IsInCombat() && SetAlerted(a_actor, true)) {
				g_onGuard[a_actor->GetFormID()] = Clock::now() + kGuardTime;
			}
		}

		void EndGuard(RE::FormID a_id)
		{
			if (auto* actor = RE::TESForm::LookupByID<RE::Actor>(a_id); actor && !actor->IsDead() && !actor->IsInCombat()) {
				SetAlerted(actor, false);
			}
		}

		void UpdateGuards(Clock::time_point a_now)
		{
			for (auto it = g_onGuard.begin(); it != g_onGuard.end();) {
				if (a_now < it->second) {
					++it;
					continue;
				}
				EndGuard(it->first);
				it = g_onGuard.erase(it);
			}
		}

		bool EndKnockoutEffect(RE::Actor* a_actor)
		{
			if (!g_sneakToolsKnockout) {
				return false;
			}
			auto* target = a_actor->AsMagicTarget();
			auto* list = target ? target->GetActiveEffectList() : nullptr;
			if (!list) {
				return false;
			}
			std::vector<RE::ActiveEffect*> found;
			for (auto* effect : *list) {
				if (effect && effect->spell == g_sneakToolsKnockout && effect->flags.none(RE::ActiveEffect::Flag::kDispelled)) {
					found.push_back(effect);
				}
			}
			for (auto* effect : found) {
				effect->Dispel(true);
			}
			return !found.empty();
		}

		void Wake(RE::Actor* a_actor)
		{
			if (!EndKnockoutEffect(a_actor)) {
				if (!CallActorScript(a_actor, "SetUnconscious", false)) {
					return;
				}
				a_actor->NotifyAnimationGraph("GetUpBegin");
			}
			PutOnGuard(a_actor);
		}

		void Judge(RE::ActorHandle a_handle)
		{
			const auto actor = a_handle.get();
			if (!actor) {
				return;
			}
			Snapshot before;
			{
				std::scoped_lock lock{ g_lock };
				const auto       it = g_snapshots.find(actor->GetFormID());
				if (it == g_snapshots.end()) {
					return;
				}
				before = it->second;
				it->second.hitPending = false;
			}
			if (!IsUnconscious(actor.get())) {
				return;
			}
			const auto                            settings = GetSettings();
			auto*                                 values = actor->AsActorValueOwner();
			const float                           health = values->GetActorValue(RE::ActorValue::kHealth);
			const float                           maxHealth = (std::max)(before.maxHealth, 1.0f);
			const float                           damage = (std::max)(before.health - health, 0.0f);
			const float                           chance = std::clamp((std::max)(settings->wakeChanceMin, damage / (maxHealth * settings->wakeDamage)), 0.0f, 1.0f);
			static std::mt19937                   rng{ std::random_device{}() };
			std::uniform_real_distribution<float> roll{ 0.0f, 1.0f };
			const bool                            wakes = roll(rng) < chance;
			logs::debug("Wake on hit: {} lost {:.0f} of {:.0f} health, chance {:.0f}% -> {}", Name(actor.get()), damage, maxHealth,
				chance * 100.0f, wakes ? "wakes up" : "stays down");
			{
				std::scoped_lock lock{ g_lock };
				if (const auto it = g_snapshots.find(actor->GetFormID()); it != g_snapshots.end()) {
					it->second.health = health;
				}
			}
			if (wakes) {
				Wake(actor.get());
			}
		}

		RE::TESPackage* SendOver(RE::Actor* a_ally, RE::Actor* a_target)
		{
			auto* package = CreatePackage(RE::PACKAGE_PROCEDURE_TYPE::kTravel);
			if (!package) {
				return nullptr;
			}
			using Flag = RE::PACKAGE_DATA::GeneralFlag;
			package->packData.packFlags.reset(static_cast<Flag>(6));
			package->packData.packFlags.set(Flag::kPreferredSpeed);
			package->packData.maxSpeed = RE::PACKAGE_DATA::PreferredSpeed::kFastWalk;

			NearReference location{};
			location.vtable = RE::VTABLE_PackageLocation[0].address();
			location.radius = 100;
			location.handle = a_target->CreateRefHandle().native_handle();
			SetPackageLocation(package, reinterpret_cast<const RE::PackageLocation*>(&location));

			a_ally->PutCreatedPackage(package, true, true, false);
			return package;
		}

		void EndRescuePackage(RE::Actor* a_ally, const Rescue& a_rescue)
		{
			auto* process = a_ally ? a_ally->GetActorRuntimeData().currentProcess : nullptr;
			if (process && a_rescue.package && process->GetRunningPackage() == a_rescue.package) {
				a_ally->EndInterruptPackage(false);
			}
		}

		bool SharesFaction(RE::Actor* a_a, RE::Actor* a_b)
		{
			std::vector<RE::TESFaction*> mine;
			a_a->VisitFactions([&](RE::TESFaction* a_faction, std::int8_t a_rank) {
				if (a_faction && a_rank >= 0 && a_faction != g_sneakToolsUnconscious) {
					mine.push_back(a_faction);
				}
				return false;
			});
			return !mine.empty() && a_b->VisitFactions([&](RE::TESFaction* a_faction, std::int8_t a_rank) {
				return a_rank >= 0 && std::ranges::find(mine, a_faction) != mine.end();
			});
		}

		bool CanRescue(RE::Actor* a_ally, RE::Actor* a_target, float a_range)
		{
			if (!a_ally || a_ally == a_target || a_ally->IsPlayerRef() || a_ally->IsDead() || a_ally->AsActorState()->IsUnconscious() ||
				a_ally->IsInCombat() || a_ally->IsPlayerTeammate() || a_ally->GetCurrentScene() || a_ally->IsInKillMove()) {
				return false;
			}
			const auto* process = a_ally->GetActorRuntimeData().currentProcess;
			if (!process || !process->middleHigh || process->middleHigh->runOncePackage.package) {
				return false;
			}
			if (a_ally->GetPosition().GetDistance(a_target->GetPosition()) > a_range || a_ally->IsHostileToActor(a_target) ||
				!SharesFaction(a_ally, a_target)) {
				return false;
			}
			bool unused = false;
			return a_ally->HasLineOfSight(a_target, unused);
		}

		bool Busy(RE::Actor* a_ally)
		{
			return std::ranges::any_of(g_rescues, [&](const auto& a_entry) { return a_entry.second.ally.get().get() == a_ally; });
		}

		void LookForRescuers(const Settings& a_settings, Clock::time_point a_now)
		{
			auto* lists = RE::ProcessLists::GetSingleton();
			if (!lists) {
				return;
			}
			const float             range = a_settings.allyRange * kUnitsPerMetre;
			std::vector<RE::FormID> lying;
			{
				std::scoped_lock lock{ g_lock };
				for (const auto& [id, snap] : g_snapshots) {
					if (a_now - snap.since >= kSettleTime) {
						lying.push_back(id);
					}
				}
			}
			for (const auto id : lying) {
				if (g_rescues.contains(id)) {
					continue;
				}
				if (const auto it = g_noRescueUntil.find(id); it != g_noRescueUntil.end() && a_now < it->second) {
					continue;
				}
				auto* target = RE::TESForm::LookupByID<RE::Actor>(id);
				if (!IsUnconscious(target) || target->GetCurrentScene()) {
					continue;
				}
				for (const auto& handle : lists->highActorHandles) {
					const auto ally = handle.get();
					if (!ally || Busy(ally.get()) || !CanRescue(ally.get(), target, range)) {
						continue;
					}
					auto* package = SendOver(ally.get(), target);
					if (!package) {
						break;
					}
					g_rescues[id] = Rescue{ ally->CreateRefHandle(), package, a_now, {} };
					logs::debug("Unconscious: {} sees {} lying there and goes over", Name(ally.get()), Name(target));
					break;
				}
			}
		}

		void UpdateRescues(const Settings& a_settings, Clock::time_point a_now)
		{
			for (auto it = g_rescues.begin(); it != g_rescues.end();) {
				auto&      rescue = it->second;
				auto*      target = RE::TESForm::LookupByID<RE::Actor>(it->first);
				const auto ally = rescue.ally.get();
				const auto finish = [&](const char* a_why, auto a_cooldown) {
					if (ally) {
						EndRescuePackage(ally.get(), rescue);
					}
					logs::debug("Unconscious: {} {}", ally ? Name(ally.get()) : "the ally", a_why);
					g_noRescueUntil[it->first] = a_now + a_cooldown;
					it = g_rescues.erase(it);
				};
				if (!IsUnconscious(target)) {
					finish("stops: the NPC is awake or gone", kRescueCooldown);
					continue;
				}
				if (!ally || ally->IsDead() || ally->AsActorState()->IsUnconscious() || ally->IsInCombat() || !a_settings.alliesWake) {
					finish("is no longer coming", kRescueCooldown);
					continue;
				}
				if (a_now - rescue.started > kRescueTimeout) {
					finish("did not get there in time", kRescueCooldown * 2);
					continue;
				}
				if (ally->GetPosition().GetDistance(target->GetPosition()) > kReachedDistance) {
					++it;
					continue;
				}
				if (rescue.arrived == Clock::time_point{}) {
					rescue.arrived = a_now;
				}
				if (a_now - rescue.arrived < std::chrono::duration<float>(a_settings.allyWakeTime)) {
					++it;
					continue;
				}
				logs::debug("Unconscious: {} wakes {}", Name(ally.get()), Name(target));
				Wake(target);
				PutOnGuard(ally.get());
				finish("is done", std::chrono::seconds(0));
			}
		}

		class HitSink final : public RE::BSTEventSink<RE::TESHitEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const RE::TESHitEvent* a_event, RE::BSTEventSource<RE::TESHitEvent>*) override
			{
				if (!a_event || !a_event->target || !GetSettings()->wakeOnHit) {
					return RE::BSEventNotifyControl::kContinue;
				}
				auto* actor = a_event->target->As<RE::Actor>();
				if (!IsUnconscious(actor) || actor->IsPlayerRef() || actor->GetCurrentScene()) {
					return RE::BSEventNotifyControl::kContinue;
				}
				{
					std::scoped_lock lock{ g_lock };
					const auto       it = g_snapshots.find(actor->GetFormID());
					if (it == g_snapshots.end() || Clock::now() - it->second.since < kSettleTime || it->second.hitPending) {
						return RE::BSEventNotifyControl::kContinue;
					}
					it->second.hitPending = true;
				}
				if (auto* tasks = SKSE::GetTaskInterface()) {
					tasks->AddTask([handle = actor->CreateRefHandle()]() { Judge(handle); });
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};
		HitSink g_sink;
	}

	void Install()
	{
		if (REL::Module::IsVR()) {
			logs::info("Unconscious NPCs wake up: not available in VR.");
			return;
		}
		if (auto* data = RE::TESDataHandler::GetSingleton()) {
			g_sneakToolsKnockout = data->LookupForm<RE::SpellItem>(0x017B74, "Sneak Tools.esp");
			g_sneakToolsUnconscious = data->LookupForm<RE::TESFaction>(0x017B72, "Sneak Tools.esp");
		}
		auto* events = RE::ScriptEventSourceHolder::GetSingleton();
		if (!events) {
			logs::info("Wake on hit: not active - the game's hit events are not available.");
			return;
		}
		events->AddEventSink<RE::TESHitEvent>(&g_sink);
		g_installed = true;
		const auto settings = GetSettings();
		logs::info("Unconscious NPCs: wake on a hit {}, allies wake them {}{}", settings->wakeOnHit ? "on" : "off",
			settings->alliesWake ? "on" : "off", g_sneakToolsKnockout ? " (Sneak Tools knockouts included)" : "");
	}

	void Update()
	{
		if (!g_installed) {
			return;
		}
		const auto settings = GetSettings();
		if (!settings->wakeOnHit && !settings->alliesWake) {
			std::scoped_lock lock{ g_lock };
			g_snapshots.clear();
			return;
		}
		auto* lists = RE::ProcessLists::GetSingleton();
		if (!lists) {
			return;
		}
		if (auto* ui = RE::UI::GetSingleton(); ui && ui->GameIsPaused()) {
			return;
		}
		const auto now = Clock::now();
		{
			std::scoped_lock lock{ g_lock };
			for (auto& [id, snap] : g_snapshots) {
				snap.maxHealth = -1.0f;
			}
			for (const auto& handle : lists->highActorHandles) {
				const auto actor = handle.get();
				if (!actor || !IsUnconscious(actor.get())) {
					continue;
				}
				auto* values = actor->AsActorValueOwner();
				const auto [it, added] = g_snapshots.try_emplace(actor->GetFormID());
				auto& snap = it->second;
				if (added) {
					snap.since = now;
				}
				snap.maxHealth = (std::max)(values->GetPermanentActorValue(RE::ActorValue::kHealth), 1.0f);
				if (!snap.hitPending) {
					snap.health = values->GetActorValue(RE::ActorValue::kHealth);
				}
			}
			std::erase_if(g_snapshots, [](const auto& a_entry) { return a_entry.second.maxHealth < 0.0f; });
		}

		UpdateGuards(now);
		UpdateRescues(*settings, now);
		if (settings->alliesWake && now - g_lastLook >= kLookStep) {
			g_lastLook = now;
			LookForRescuers(*settings, now);
		}
		std::erase_if(g_noRescueUntil, [&](const auto& a_entry) { return a_entry.second <= now; });
	}

	void Reset()
	{
		std::scoped_lock lock{ g_lock };
		g_snapshots.clear();
		g_rescues.clear();
		g_noRescueUntil.clear();
		g_onGuard.clear();
	}

	void BeforeSave()
	{
		if (!g_installed) {
			return;
		}
		for (const auto& [id, until] : g_onGuard) {
			EndGuard(id);
		}
		g_onGuard.clear();
		for (const auto& [id, rescue] : g_rescues) {
			if (const auto ally = rescue.ally.get()) {
				EndRescuePackage(ally.get(), rescue);
			}
		}
		g_rescues.clear();
	}
}
