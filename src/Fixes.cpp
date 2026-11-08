#include "Fixes.h"

#include "GameSettings.h"
#include "Hooks.h"
#include "LightSensor.h"
#include "Settings.h"
#include "Unconscious.h"

#include <random>
#include <span>
#include <thread>
#include <unordered_set>

namespace StealthFixes::Fixes
{
	using namespace Hooks;

	namespace
	{
		using Clock = std::chrono::steady_clock;

		using Formula_t = std::int32_t(RE::Actor* a_observer, RE::Actor* a_target,
			float a_observerSkill, float a_targetSkill,
			std::int32_t a_sizeClass, std::int32_t a_lineOfSight, std::int32_t a_inViewCone, float a_distance,
			std::int32_t a_light, std::int32_t a_blindness, bool a_invisible, float a_equippedWeight,
			bool a_moving, bool a_sneaking, float a_noiseMult, bool a_running, bool a_sleeping, bool a_inCombat,
			std::int32_t a_action, bool a_exterior, bool a_fightsOther, std::int32_t a_ambush,
			float* a_sound, float* a_visual, float* a_skill);

		std::uintptr_t          g_originalFormula{ 0 };
		std::atomic<bool>       g_formulaActive{ false };
		std::atomic<Formula_t*> g_filter{ nullptr };

		constexpr std::int32_t kUnconsciousLevel = -100;

		bool UnconsciousObserver(const RE::Actor* a_observer)
		{
			return a_observer && GetSettings()->unconsciousNoticeNothing && a_observer->AsActorState()->IsUnconscious();
		}

		float HeadingAngleTo(RE::Actor* a_actor, const RE::NiPoint3& a_point)
		{
			using func_t = float(RE::Actor*, const RE::NiPoint3*);
			static REL::Relocation<func_t> func{ REL::VariantID(36757, 37773, 0x605FD0) };
			return func(a_actor, &a_point);
		}

		float FormulaLight(float a_raw, bool a_exterior, bool a_nightEye, bool a_wholeBonus = false)
		{
			std::int32_t light = static_cast<std::int32_t>(a_raw);
			if (a_nightEye) {
				const float bonus = GameSettings::Float("fDetectionNightEyeBonus", 1.0f);
				if (a_wholeBonus || !GetSettings()->nightEyeBonusFix) {
					light *= static_cast<std::int32_t>(bonus);
				} else {
					light = static_cast<std::int32_t>(static_cast<float>(light) * bonus);
				}
			}
			if (a_exterior) {
				light -= GameSettings::Int("iLightLevelExteriorMod");
			} else {
				std::int32_t ambient = 0;
				if (const auto* sky = RE::Sky::GetSingleton()) {
					const auto& color = sky->skyColor[3];
					ambient = static_cast<std::int32_t>((std::max)({ color.red, color.green, color.blue }) * 100.0f);
				}
				light -= (std::min)(ambient, GameSettings::Int("iLightLevelInteriorMod"));
			}
			return static_cast<float>(std::clamp(light, 0, GameSettings::Int("iLightLevelMax", 100)));
		}

		std::int32_t ToLevel(float a_score)
		{
			if (a_score >= 1.0f) {
				return static_cast<std::int32_t>(a_score);
			}
			if (a_score > 0.0f) {
				return 1;
			}
			return static_cast<std::int32_t>(a_score);
		}

		std::optional<std::int32_t> SoundAmount(RE::SOUND_LEVEL a_level)
		{
			switch (a_level) {
			case RE::SOUND_LEVEL::kLoud:
				return GameSettings::Int("iSoundLevelLoud", 100);
			case RE::SOUND_LEVEL::kNormal:
				return GameSettings::Int("iSoundLevelNormal", 50);
			case RE::SOUND_LEVEL::kSilent:
				return GameSettings::Int("iSoundLevelSilent", 0);
			case RE::SOUND_LEVEL::kVeryLoud:
				return GameSettings::Int("iSoundLevelVeryLoud", 200);
			default:
				return std::nullopt;
			}
		}

		float ActionFraction()
		{
			const float mult = GameSettings::Float("fSneakActionMult", 1.0f);
			return mult > 0.0f ? mult - static_cast<float>(static_cast<std::int32_t>(mult)) : 0.0f;
		}

		bool Flying(RE::Actor* a_actor)
		{
			const auto* state = a_actor ? a_actor->AsActorState() : nullptr;
			const auto  fly = state ? state->GetFlyState() : RE::FLY_STATE::kNone;
			return fly != RE::FLY_STATE::kNone && fly != RE::FLY_STATE::kPerching;
		}

		float Attenuation(RE::Actor* a_observer, RE::Actor* a_target, bool a_exterior, float a_distance)
		{
			float range = GameSettings::Float("fSneakMaxDistance", 2500.0f);
			if (a_exterior) {
				range *= GameSettings::Float("fSneakExteriorDistanceMult", 1.0f);
			}
			if (Flying(a_observer) || Flying(a_target)) {
				range *= GameSettings::Float("fSneakFlyingDistanceMult", 1.0f);
			}
			if (range <= 0.0f) {
				return 0.0f;
			}
			return std::pow((std::max)((range - a_distance) / range, 0.0f), GameSettings::Float("fSneakDistanceAttenuationExponent", 2.0f));
		}

		std::atomic<float> g_baseLight{ -1.0f };

		float PlayerExposure(const Settings& a_settings)
		{
			auto*       player = RE::PlayerCharacter::GetSingleton();
			const auto* process = player ? player->GetActorRuntimeData().currentProcess : nullptr;
			const float base = g_baseLight.load();
			if (!process || !process->high || base < 0.0f) {
				return -1.0f;
			}
			return std::clamp((process->high->lightLevel - base) / (std::max)(a_settings.exposureSpan, 1.0f), 0.0f, 1.0f);
		}
	}

	namespace
	{
		std::atomic<RE::FormID>   g_touchedID{ 0 };
		std::atomic<Clock::rep>   g_touchedUntil{ 0 };
		std::atomic<std::int32_t> g_touchLoudness{ 0 };

		using SetActionLoudness_t = void(RE::Actor* a_actor, std::int32_t a_loudness);
		std::uintptr_t g_originalContact{ 0 };

		constexpr auto kTouchHold = std::chrono::seconds(5);
		constexpr auto kTouchGap = std::chrono::milliseconds(300);

		RE::FormID        g_contactID{ 0 };
		Clock::time_point g_contactStart{};
		Clock::time_point g_contactLast{};

		std::int32_t TouchNoise(const RE::Actor* a_observer)
		{
			if (!GetSettings()->touchFix || a_observer->GetFormID() != g_touchedID.load(std::memory_order_relaxed)) {
				return 0;
			}
			return Clock::now().time_since_epoch().count() < g_touchedUntil.load(std::memory_order_relaxed) ? g_touchLoudness.load() : 0;
		}

		RE::FormID TouchedActor(RE::Actor* a_player)
		{
			using GetContact_t = void*(RE::bhkCharacterController*);
			using GetOwner_t = RE::NiAVObject*(void*, std::uint32_t);
			static REL::Relocation<GetContact_t> getContact{ REL::VariantID(76451, 78291, 0xE15BB0) };
			static REL::Relocation<GetOwner_t>   getOwner{ REL::VariantID(75979, 77805, 0xDF8CC0) };
			auto*                                controller = a_player->GetCharController();
			void*                                contact = controller ? getContact(controller) : nullptr;
			if (!contact) {
				return 0;
			}
			using Body_t = void*(void*);
			auto* body = (*reinterpret_cast<Body_t***>(contact))[0x80 / sizeof(void*)](contact);
			auto* object = body ? getOwner(body, 0) : nullptr;
			auto* ref = object ? RE::TESObjectREFR::FindReferenceFor3D(object) : nullptr;
			auto* actor = ref ? ref->As<RE::Actor>() : nullptr;
			return actor && actor != a_player && !actor->IsPlayerTeammate() ? actor->GetFormID() : 0;
		}

		void ContactLoudness(RE::Actor* a_actor, std::int32_t a_loudness)
		{
			const auto settings = GetSettings();
			if (!g_formulaActive || !settings->touchFix || !a_actor) {
				if (a_actor) {
					const auto& data = a_actor->GetActorRuntimeData();
					if (data.actionValue != a_loudness || data.timerOnAction < 4.0f) {
						logs::debug("Noise: touch {} - everyone nearby hears it (base game)", a_loudness);
					}
				}
				reinterpret_cast<SetActionLoudness_t*>(g_originalContact)(a_actor, a_loudness);
				return;
			}
			const auto* state = a_actor->AsActorState();
			const auto  attack = state ? state->GetAttackState() : RE::ATTACK_STATE_ENUM::kNone;
			if (attack >= RE::ATTACK_STATE_ENUM::kDraw && attack <= RE::ATTACK_STATE_ENUM::kBash) {
				g_contactID = 0;
				return;
			}
			const auto now = Clock::now();
			if (const auto nearest = TouchedActor(a_actor); nearest != g_contactID || now - g_contactLast > kTouchGap) {
				g_contactID = nearest;
				g_contactStart = now;
			}
			g_contactLast = now;
			const auto grace = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<float>(settings->touchGrace));
			if (g_contactID != 0 && now - g_contactStart >= grace) {
				if (g_touchedID != g_contactID || Clock::now().time_since_epoch().count() >= g_touchedUntil.load()) {
					logs::debug("Noise: touch {} - only the person touched hears it", a_loudness);
				}
				g_touchLoudness = a_loudness;
				g_touchedUntil = (now + kTouchHold).time_since_epoch().count();
				g_touchedID = g_contactID;
			}
		}

		void InstallTouch()
		{
			const REL::CallsiteID owner{ REL::RelocationID(39375, 40447) };
			const REL::CallsiteID setter{ REL::RelocationID(36701, 37711), 0x602C00 };
			const auto            site = REL::try_resolve_callsite(owner,
						   REL::AUTO_CALLSITE(setter).or_offset(0x3EA, REL::Version{ 1, 6, 1170, 0 }));
			if (!site || !g_formulaActive) {
				logs::info("Touch noticed only by the person touched: not active - another mod probably changes the same place.");
				return;
			}
			g_originalContact = SKSE::GetTrampoline().write_call<5>(*site.address, ContactLoudness);
			logs::info("Touch noticed only by the person touched: active");
		}
	}

	namespace
	{
		struct Spot
		{
			RE::NiPoint3  position;
			float         light{ -1.0f };
			std::uint32_t projectile{ 0 };
		};
		struct CorpseSpot
		{
			RE::FormID        corpse{ 0 };
			float             light{ -1.0f };
			RE::NiPoint3      position;
			Clock::time_point measured{};
		};
		std::mutex              g_spotLock;
		std::vector<Spot>       g_projectileSpots;
		Spot                    g_soundSpot;
		std::vector<CorpseSpot> g_corpseSpots;
		constexpr std::size_t   kMaxSpots = 32;
		constexpr std::size_t   kMaxCorpses = 32;
		constexpr float         kSpotMatch = 32.0f;
		constexpr float         kSpotRange = 4096.0f;
		std::atomic<bool>       g_eventHooks{ false };

		const Spot* MatchSpot(std::span<const Spot> a_spots, const RE::NiPoint3& a_observer, float a_distance)
		{
			const Spot* best = nullptr;
			int         fits = 0;
			for (const auto& spot : a_spots) {
				if (spot.light >= 0.0f && std::abs(spot.position.GetDistance(a_observer) - a_distance) < kSpotMatch) {
					best = &spot;
					++fits;
				}
			}
			return fits == 1 ? best : nullptr;
		}
	}

	namespace
	{
		using EventFormula_t = std::int32_t(RE::Actor* a_observer, RE::Actor* a_target, float a_observerSkill,
			std::int32_t a_lineOfSight, float a_distance, std::int32_t a_blindness, std::int32_t a_light, bool a_sleeping,
			bool a_alerted, bool a_inCombat, std::int32_t a_loudness, std::int32_t a_inViewCone, bool a_exterior);
		std::uintptr_t g_originalSoundEvent{ 0 };
		std::uintptr_t g_originalProjectileEvent{ 0 };

		std::optional<RE::NiPoint3> LiveProjectile(RE::Actor* a_observer, RE::Actor* a_target, float a_distance,
			std::optional<std::int32_t>* a_loudness = nullptr, std::uint32_t* a_which = nullptr)
		{
			auto* manager = RE::Projectile::Manager::GetSingleton();
			if (!manager) {
				return std::nullopt;
			}
			constexpr float             kExact = 1.0f;
			const auto                  from = a_observer->GetPosition();
			std::optional<RE::NiPoint3> best;
			int                         fits = 0;
			RE::BSSpinLockGuard         guard{ manager->projectileLock };
			for (const auto& handle : manager->unlimited) {
				const auto projectile = handle.get();
				if (!projectile || projectile->GetProjectileRuntimeData().shooter.get().get() != a_target) {
					continue;
				}
				const auto pos = projectile->GetPosition();
				if (std::abs(pos.GetDistance(from) - a_distance) < kExact) {
					best = pos;
					++fits;
					if (a_which) {
						*a_which = handle.native_handle();
					}
					if (a_loudness) {
						const auto* base = projectile->GetProjectileBase();
						*a_loudness = base ? SoundAmount(base->soundLevel) : std::nullopt;
					}
				}
			}
			if (fits != 1 && a_loudness) {
				*a_loudness = std::nullopt;
			}
			return fits == 1 ? best : std::nullopt;
		}

		void FixEvent(bool a_projectile, RE::Actor* a_observer, RE::Actor* a_target, float a_distance, std::int32_t& a_light,
			std::int32_t& a_blindness, std::int32_t& a_inViewCone, bool a_exterior)
		{
			const auto settings = GetSettings();
			if (!a_observer || !a_target) {
				return;
			}
			const bool player = a_target->IsPlayerRef();
			if (!player && !(settings->eventSightFix && settings->eventSightNPCs)) {
				return;
			}
			std::optional<RE::NiPoint3> where;
			std::uint32_t               which = 0;
			if (a_projectile && (player || a_inViewCone != 0)) {
				where = LiveProjectile(a_observer, a_target, a_distance, nullptr, &which);
			}
			if (player) {
				std::scoped_lock lock{ g_spotLock };
				const Spot*      spot = nullptr;
				if (a_projectile && where) {
					constexpr float kNear = 512.0f;
					float           nearest = kNear * kNear;
					for (const auto& s : g_projectileSpots) {
						const float d = s.position.GetSquaredDistance(*where);
						if (s.light >= 0.0f && which && s.projectile == which && d < kNear * kNear) {
							spot = &s;
							break;
						}
						if (s.light >= 0.0f && d < nearest) {
							nearest = d;
							spot = &s;
						}
					}
				} else if (!a_projectile) {
					spot = MatchSpot(std::span<const Spot>{ &g_soundSpot, 1 }, a_observer->GetPosition(), a_distance);
				}
				if (spot && settings->eventRealLight) {
					const bool nightEye = a_observer->AsActorValueOwner()->GetActorValue(RE::ActorValue::kNightEye) > 0.0f;
					a_light = (std::min)(a_light, static_cast<std::int32_t>(FormulaLight(spot->light, a_exterior, nightEye)));
				}
			}
			if (!settings->eventSightFix) {
				return;
			}
			if (!a_projectile) {
				const auto* process = a_target->GetActorRuntimeData().currentProcess;
				if (const auto* event = process && process->high ? process->high->actorsGeneratedDetectionEvent : nullptr) {
					where = event->location;
				}
			}
			if (a_inViewCone != 0 && where) {
				const float half = GameSettings::Float("fDetectionViewCone", 190.0f) * RE::NI_PI / 180.0f * 0.5f;
				if (HeadingAngleTo(a_observer, *where) >= half) {
					a_inViewCone = 0;
				}
			}
			a_blindness = static_cast<std::int32_t>(a_observer->AsActorValueOwner()->GetActorValue(RE::ActorValue::kBlindness));
		}

		std::int32_t CorrectEvent(std::int32_t a_level, RE::Actor* a_observer, RE::Actor* a_target, float a_observerSkill,
			std::int32_t a_lineOfSight, float a_distance, std::int32_t a_blindness, std::int32_t a_light, bool a_sleeping, bool a_alerted,
			bool a_inCombat, std::int32_t a_loudness, std::int32_t a_inViewCone, bool a_exterior)
		{
			const auto  settings = GetSettings();
			const float fraction = settings->actionNoiseFix && a_loudness > 0 ? ActionFraction() : 0.0f;
			if ((fraction <= 0.0f && !settings->eventPerception) || !a_observer || !a_target || !a_target->IsPlayerRef()) {
				return a_level;
			}
			float range = a_loudness < GameSettings::Int("iSoundLevelVeryLoud", 200) ? GameSettings::Float("fSneakMaxDistance", 2500.0f) :
			                                                                           GameSettings::Float("fDetectEventSneakDistanceVeryLoud", 3072.0f);
			if (a_exterior) {
				range *= GameSettings::Float("fSneakExteriorDistanceMult", 1.0f);
			}
			if (range <= 0.0f) {
				return a_level;
			}
			const float a = std::pow((std::max)((range - a_distance) / range, 0.0f), GameSettings::Float("fSneakDistanceAttenuationExponent", 2.0f));
			const float heard = a_lineOfSight <= 0 ? GameSettings::Float("fSneakSoundLosMult", 1.0f) * a : a;
			const auto  whole = static_cast<std::int32_t>(GameSettings::Float("fSneakActionMult", 1.0f));
			const float soundsMult = GameSettings::Float("fSneakSoundsMult", 1.0f);
			const float sound = (std::max)(static_cast<float>(a_loudness * whole) * (soundsMult * heard), 0.0f);
			const float lightMult = a_exterior ? GameSettings::Float("fSneakLightExteriorMult", 1.0f) : GameSettings::Float("fSneakLightMult", 1.0f);
			const float sight = a_sleeping ? 0.0f : (100.0f - static_cast<float>(a_blindness)) * 0.0099999998f;
			const float visual = (std::max)(static_cast<float>(a_inViewCone) * a *
												(static_cast<float>(a_light) * lightMult + GameSettings::Float("fDetectionSneakLightMod")) * sight,
				0.0f);
			const float score = visual + (sound + GameSettings::Float("fSneakBaseValue"));
			if (ToLevel(score) != a_level) {
				return a_level;
			}

			float extra = 0.0f;
			if (fraction > 0.0f) {
				extra += (std::max)(static_cast<float>(a_loudness) * fraction * soundsMult * heard, 0.0f);
			}
			if (settings->eventPerception) {
				float skill = a_observerSkill;
				if (a_observer->IsPlayerRef()) {
					skill = GameSettings::Float("fPlayerDetectionSneakMult", 1.0f) * skill + GameSettings::Float("fPlayerDetectionSneakBase");
				}
				RE::BGSEntryPoint::HandleEntryPoint(RE::BGSEntryPoint::ENTRY_POINT::kModDetectionSneakSkill, a_observer, a_target, &skill);
				const float factor = 1.0f + (a_alerted || a_inCombat ? GameSettings::Float("fSneakAlertMod") : 0.0f) +
				                     (a_sleeping ? GameSettings::Float("fSneakSleepMod") : 0.0f);
				const float pMin = GameSettings::Float("fSneakPerceptionSkillMin");
				const float pMax = GameSettings::Float("fSneakPerceptionSkillMax", 100.0f);
				extra += a * factor * (pMin + (pMax - pMin) * skill * 0.01f) * GameSettings::Float("fSneakSkillMult", 0.5f);
			}
			return extra != 0.0f ? ToLevel(score + extra) : a_level;
		}

		std::int32_t SoundEvent(RE::Actor* a_observer, RE::Actor* a_target, float a_observerSkill, std::int32_t a_lineOfSight,
			float a_distance, std::int32_t a_blindness, std::int32_t a_light, bool a_sleeping, bool a_alerted, bool a_inCombat,
			std::int32_t a_loudness, std::int32_t a_inViewCone, bool a_exterior)
		{
			if (UnconsciousObserver(a_observer)) {
				return kUnconsciousLevel;
			}
			FixEvent(false, a_observer, a_target, a_distance, a_light, a_blindness, a_inViewCone, a_exterior);
			const auto level = reinterpret_cast<EventFormula_t*>(g_originalSoundEvent)(a_observer, a_target, a_observerSkill, a_lineOfSight,
				a_distance, a_blindness, a_light, a_sleeping, a_alerted, a_inCombat, a_loudness, a_inViewCone, a_exterior);
			return CorrectEvent(level, a_observer, a_target, a_observerSkill, a_lineOfSight, a_distance, a_blindness, a_light, a_sleeping,
				a_alerted, a_inCombat, a_loudness, a_inViewCone, a_exterior);
		}

		std::int32_t ProjectileEvent(RE::Actor* a_observer, RE::Actor* a_target, float a_observerSkill, std::int32_t a_lineOfSight,
			float a_distance, std::int32_t a_blindness, std::int32_t a_light, bool a_sleeping, bool a_alerted, bool a_inCombat,
			std::int32_t a_loudness, std::int32_t a_inViewCone, bool a_exterior)
		{
			if (UnconsciousObserver(a_observer)) {
				return kUnconsciousLevel;
			}
			if (GetSettings()->projectileSound && a_observer && a_target && a_target->IsPlayerRef()) {
				std::optional<std::int32_t> loudness;
				LiveProjectile(a_observer, a_target, a_distance, &loudness);
				if (loudness) {
					a_loudness = *loudness;
				}
			}
			FixEvent(true, a_observer, a_target, a_distance, a_light, a_blindness, a_inViewCone, a_exterior);
			const auto level = reinterpret_cast<EventFormula_t*>(g_originalProjectileEvent)(a_observer, a_target, a_observerSkill,
				a_lineOfSight, a_distance, a_blindness, a_light, a_sleeping, a_alerted, a_inCombat, a_loudness, a_inViewCone, a_exterior);
			return CorrectEvent(level, a_observer, a_target, a_observerSkill, a_lineOfSight, a_distance, a_blindness, a_light, a_sleeping,
				a_alerted, a_inCombat, a_loudness, a_inViewCone, a_exterior);
		}

		void InstallEvents()
		{
			constexpr std::size_t                 kScan = 0x1000;
			const REL::Relocation<std::uintptr_t> owner{ REL::VariantID(36758, 37774, 0x606080) };
			const REL::Relocation<std::uintptr_t> target{ REL::VariantID(25813, 26366, 0x3CC750) };
			const auto*                           code = reinterpret_cast<const std::uint8_t*>(owner.address());
			std::vector<std::uintptr_t>           sites;
			for (std::size_t i = 0; i + 5 <= kScan; ++i) {
				if (code[i] != 0xE8) {
					continue;
				}
				std::int32_t rel = 0;
				std::memcpy(&rel, code + i + 1, sizeof(rel));
				if (owner.address() + i + 5 + rel == target.address()) {
					sites.push_back(owner.address() + i);
				}
			}
			if (sites.size() != 2) {
				logs::info("Arrows and noise spots: not active - another mod probably changes the same place.");
				return;
			}
			g_originalSoundEvent = SKSE::GetTrampoline().write_call<5>(sites[0], SoundEvent);
			g_originalProjectileEvent = SKSE::GetTrampoline().write_call<5>(sites[1], ProjectileEvent);
			g_eventHooks = true;
			logs::info("Arrows and noise spots: active");
		}
	}

	namespace
	{
		float CorpseShadow(const Settings& a_settings, RE::Actor* a_observer, bool a_inCombat, [[maybe_unused]] bool a_fightsOther)
		{
			if (a_inCombat) {
				return a_settings.shadowAlerted;
			}
			if (const auto* process = a_observer->GetActorRuntimeData().currentProcess;
				process && process->lowProcessFlags.any(RE::AIProcess::LowProcessFlags::kAlert)) {
				return a_settings.shadowAlerted;
			}
			return a_settings.shadowUnaware;
		}

		std::int32_t CorpseLevel(RE::Actor* a_observer, RE::Actor* a_corpse, std::int32_t a_light, bool a_exterior, bool a_inCombat,
			bool a_fightsOther, std::int32_t a_level, float a_sound, float* a_visual, float* a_skill)
		{
			const auto  settings = GetSettings();
			const float base = g_baseLight.load();
			if (!settings->corpseLight || base < 0.0f || !a_visual || !a_skill) {
				return a_level;
			}
			float raw = -1.0f;
			{
				std::scoped_lock lock{ g_spotLock };
				const auto       id = a_corpse->GetFormID();
				if (const auto it = std::ranges::find(g_corpseSpots, id, &CorpseSpot::corpse); it != g_corpseSpots.end()) {
					raw = it->light;
				}
			}
			if (raw < 0.0f) {
				return a_level;
			}
			const float before = ((a_sound + GameSettings::Float("fSneakBaseValue")) + *a_visual) + *a_skill;
			if (ToLevel(before) != a_level) {
				return a_level;
			}
			const float span = (std::max)(settings->exposureSpan, 1.0f);
			const float exposure = std::clamp((raw - base) / span, 0.0f, 1.0f);
			const float shadow = CorpseShadow(*settings, a_observer, a_inCombat, a_fightsOther);
			const float lit = shadow + (1.0f - shadow) * exposure;

			float visual = *a_visual;
			if (visual > 0.0f) {
				const float lightMult = a_exterior ? GameSettings::Float("fSneakLightExteriorMult", 1.0f) :
				                                     GameSettings::Float("fSneakLightMult", 1.0f);
				const float lightMod = GameSettings::Float("fDetectionSneakLightMod");
				if (const float engineLight = static_cast<float>(a_light) * lightMult + lightMod; engineLight > 0.0f) {
					const bool  nightEye = a_observer->AsActorValueOwner()->GetActorValue(RE::ActorValue::kNightEye) > 0.0f;
					const float light = FormulaLight(raw, a_exterior, nightEye);
					visual = visual / engineLight * (light * lightMult + lightMod);
				}
				visual = (std::max)(visual * lit, 0.0f);
			}
			*a_visual = visual;
			if (*a_skill > 0.0f) {
				*a_skill *= lit;
			}
			const auto level = ToLevel(GameSettings::Float("fSneakBaseValue") + a_sound + *a_visual + *a_skill);
			if ((level > 0) != (a_level > 0)) {
				logs::debug("{} {} the body of {} (light {:.0f}, base game: {})", a_observer->GetDisplayFullName(),
					level > 0 ? "sees" : "does not see", a_corpse->GetDisplayFullName(), raw, a_level > 0 ? "sees it" : "does not see it");
			}
			return level;
		}
	}

	namespace
	{
		std::int32_t NightEyeLight(RE::Actor* a_observer, RE::Actor* a_target, std::int32_t a_light, bool a_exterior)
		{
			if (!GetSettings()->nightEyeBonusFix) {
				return a_light;
			}
			const float bonus = GameSettings::Float("fDetectionNightEyeBonus", 1.0f);
			if (bonus == static_cast<float>(static_cast<std::int32_t>(bonus)) ||
				a_observer->AsActorValueOwner()->GetActorValue(RE::ActorValue::kNightEye) <= 0.0f) {
				return a_light;
			}
			const auto* process = a_target->GetActorRuntimeData().currentProcess;
			if (!process || !process->high) {
				return a_light;
			}
			const float raw = process->high->lightLevel;
			if (static_cast<std::int32_t>(FormulaLight(raw, a_exterior, true, true)) != a_light) {
				return a_light;
			}
			return static_cast<std::int32_t>(FormulaLight(raw, a_exterior, true, false));
		}

		std::int32_t CallGame(RE::Actor* a_observer, RE::Actor* a_target, float a_observerSkill, float a_targetSkill,
			std::int32_t a_sizeClass, std::int32_t a_lineOfSight, std::int32_t a_inViewCone, float a_distance,
			std::int32_t a_light, std::int32_t a_blindness, bool a_invisible, float a_equippedWeight,
			bool a_moving, bool a_sneaking, float a_noiseMult, bool a_running, bool a_sleeping, bool a_inCombat,
			std::int32_t a_action, bool a_exterior, bool a_fightsOther, std::int32_t a_ambush,
			float* a_sound, float* a_visual, float* a_skill)
		{
			const auto level = reinterpret_cast<Formula_t*>(g_originalFormula)(a_observer, a_target, a_observerSkill, a_targetSkill,
				a_sizeClass, a_lineOfSight, a_inViewCone, a_distance, a_light, a_blindness, a_invisible, a_equippedWeight, a_moving,
				a_sneaking, a_noiseMult, a_running, a_sleeping, a_inCombat, a_action, a_exterior, a_fightsOther, a_ambush, a_sound,
				a_visual, a_skill);
			if (a_action <= 0 || !a_sound || !a_visual || !a_skill || !GetSettings()->actionNoiseFix) {
				return level;
			}
			const float fraction = ActionFraction();
			if (fraction <= 0.0f) {
				return level;
			}
			const float score = ((*a_sound + GameSettings::Float("fSneakBaseValue")) + *a_visual) + *a_skill;
			if (ToLevel(score) != level) {
				return level;
			}
			float heard = Attenuation(a_observer, a_target, a_exterior, a_distance);
			if (a_lineOfSight <= 0 && a_ambush != 1) {
				heard *= GameSettings::Float("fSneakSoundLosMult", 1.0f);
			}
			const float sleep = a_sleeping ? 1.0f + GameSettings::Float("fSneakSleepBonus") : 1.0f;
			const float extra = (std::max)(static_cast<float>(a_action) * fraction * GameSettings::Float("fSneakSoundsMult", 1.0f) * heard * sleep, 0.0f);
			if (extra <= 0.0f) {
				return level;
			}
			*a_sound += extra;
			return ToLevel(score + extra);
		}

		std::optional<float> CurrentSpeed(RE::Actor* a_actor)
		{
			auto* const controller = a_actor->GetActorRuntimeData().movementController.get();
			if (!controller) {
				return std::nullopt;
			}
			static const REL::Relocation<std::uintptr_t> queryDeltas{ REL::VariantID(525807, 412284, 0x31A1770) };
			using GetInterface_t = void*(void* a_controller, const void* a_id);
			using Speed_t = float(void* a_interface);
			auto* const deltas = (*reinterpret_cast<GetInterface_t***>(controller))[3](controller, reinterpret_cast<const void*>(queryDeltas.address()));
			if (!deltas) {
				return std::nullopt;
			}
			return (*reinterpret_cast<Speed_t***>(deltas))[1](deltas);
		}

		bool MovesBackwards(RE::Actor* a_actor)
		{
			using func_t = float(RE::Actor*);
			static const REL::Relocation<func_t*> func{ REL::VariantID(36935, 37960, 0x611900) };
			constexpr float                       kTwoPi = 2.0f * RE::NI_PI;
			float                                 angle = std::fmod(func(a_actor), kTwoPi);
			if (angle < 0.0f) {
				angle += kTwoPi;
			}
			return angle >= 0.75f * RE::NI_PI && angle < 1.25f * RE::NI_PI;
		}

		bool RunsForDetection(RE::Actor* a_actor, bool a_running)
		{
			float ignore = 0.0f;
			RE::BGSEntryPoint::HandleEntryPoint(RE::BGSEntryPoint::ENTRY_POINT::kIgnoreRunningDuringDetection, a_actor, &ignore);
			if (ignore != 0.0f) {
				return a_running;
			}
			auto* const state = a_actor->AsActorState();
			if (state->IsSprinting()) {
				return true;
			}
			using GetSpeeds_t = bool(RE::ActorState * a_state, RE::Movement::MaxSpeeds * a_speeds);
			RE::Movement::MaxSpeeds speeds{};
			const auto              speed = CurrentSpeed(a_actor);
			if (!speed || !(*reinterpret_cast<GetSpeeds_t***>(state))[8](state, &speeds)) {
				return a_running;
			}
			const auto  direction = MovesBackwards(a_actor) ? RE::Movement::SPEED_DIRECTIONS::kBack : RE::Movement::SPEED_DIRECTIONS::kForward;
			const float walk = speeds.speeds[direction][RE::Movement::MaxSpeeds::kWalk];
			const float run = speeds.speeds[direction][RE::Movement::MaxSpeeds::kRun];
			if (std::abs(run - walk) < 0.01f) {
				return false;
			}
			const float k = (GameSettings::Float("fJogInterpolationBetweenWalkAndRun", 0.5f) + 1.0f) * 0.5f;
			return *speed >= (1.0f - k) * walk + k * run;
		}

		std::int32_t Formula(RE::Actor* a_observer, RE::Actor* a_target, float a_observerSkill, float a_targetSkill,
			std::int32_t a_sizeClass, std::int32_t a_lineOfSight, std::int32_t a_inViewCone, float a_distance,
			std::int32_t a_light, std::int32_t a_blindness, bool a_invisible, float a_equippedWeight,
			bool a_moving, bool a_sneaking, float a_noiseMult, bool a_running, bool a_sleeping, bool a_inCombat,
			std::int32_t a_action, bool a_exterior, bool a_fightsOther, std::int32_t a_ambush,
			float* a_sound, float* a_visual, float* a_skill)
		{
			if (UnconsciousObserver(a_observer)) {
				for (auto* const out : { a_sound, a_visual, a_skill }) {
					if (out) {
						*out = 0.0f;
					}
				}
				return kUnconsciousLevel;
			}
			if (a_observer && a_target && a_target->IsPlayerRef()) {
				a_action = (std::max)(a_action, TouchNoise(a_observer));
				a_light = NightEyeLight(a_observer, a_target, a_light, a_exterior);
			}
			if (a_target && a_moving && GetSettings()->sprintCounts) {
				a_running = RunsForDetection(a_target, a_running);
			}
			if (a_target && a_sneaking && (a_moving || a_running) && GetSettings()->quietSneakKillmove &&
				a_target->IsInKillMove()) {
				a_moving = false;
				a_running = false;
			}
			auto* const next = g_filter.load();
			const auto  level = (next ? next : &CallGame)(a_observer, a_target, a_observerSkill, a_targetSkill, a_sizeClass,
                a_lineOfSight, a_inViewCone, a_distance, a_light, a_blindness, a_invisible, a_equippedWeight, a_moving, a_sneaking,
                a_noiseMult, a_running, a_sleeping, a_inCombat, a_action, a_exterior, a_fightsOther, a_ambush, a_sound, a_visual, a_skill);
			if (a_observer && a_target && !a_target->IsPlayerRef() && a_target->IsDead() && a_sound) {
				return CorpseLevel(a_observer, a_target, a_light, a_exterior, a_inCombat, a_fightsOther, level, *a_sound, a_visual, a_skill);
			}
			return level;
		}

		void InstallFormula()
		{
			const REL::CallsiteID owner{ REL::RelocationID(36758, 37774), 0x606080 };
			const REL::CallsiteID formula{ REL::RelocationID(25812, 26365), 0x3CC260 };
			const auto            site = REL::try_resolve_callsite(owner,
						   REL::AUTO_CALLSITE(formula).or_offset(0x5B9, REL::Version{ 1, 6, 1170, 0 }));
			if (!site) {
				logs::info("Detection check (touch, bodies): not active - another mod probably changes the same place.");
				return;
			}
			g_originalFormula = SKSE::GetTrampoline().write_call<5>(*site.address, Formula);
			g_formulaActive = true;
		}
	}

	namespace
	{
		using ActionNoise_t = void(RE::Actor* a_actor, std::int32_t a_loudness);
		using DetectionEvent_t = void(RE::AIProcess* a_process, RE::Actor* a_actor, const RE::NiPoint3* a_position, std::int32_t a_loudness,
			RE::TESObjectREFR* a_source);
		using SummonPlace_t = void(RE::ActiveEffect* a_effect);
		using SummonEvent_t = void(RE::ActiveEffect* a_effect, void* a_event);

		std::uintptr_t                 g_originalShotNoise{ 0 };
		std::uintptr_t                 g_originalSpellNoise{ 0 };
		std::uintptr_t                 g_originalBlowNoise{ 0 };
		std::uintptr_t                 g_originalSpellEvent{ 0 };
		std::uintptr_t                 g_originalSummonEvent{ 0 };
		std::uintptr_t                 g_originalExplosionEvent{ 0 };
		std::uintptr_t                 g_originalImpactEvent{ 0 };
		std::uintptr_t                 g_originalSummonPlace{ 0 };
		std::uintptr_t                 g_originalSummonHandleEvent{ 0 };
		thread_local RE::ActiveEffect* t_summon{ nullptr };

		bool KeepLouderAction(RE::Actor* a_actor, std::int32_t a_loudness)
		{
			if (!a_actor || !GetSettings()->louderStays) {
				return false;
			}
			const auto& data = a_actor->GetActorRuntimeData();
			return data.timerOnAction > 0.0f && data.actionValue > a_loudness;
		}

		constexpr float kLouderEventNear = 700.0f;

		bool KeepLouderEvent(RE::AIProcess* a_process, const RE::NiPoint3* a_position, std::int32_t a_loudness)
		{
			if (!a_process || !a_process->high || !a_position || !GetSettings()->louderStays) {
				return false;
			}
			const auto* event = a_process->high->actorsGeneratedDetectionEvent;
			return event && static_cast<std::int32_t>(event->actionValue) > a_loudness &&
			       event->location.GetSquaredDistance(*a_position) <= kLouderEventNear * kLouderEventNear;
		}

		void LogNoise(const RE::Actor* a_actor, const char* a_source, std::int32_t a_loudness, const char* a_result)
		{
			if (a_actor && a_actor->IsPlayerRef()) {
				logs::debug("Noise: {} {} - {}", a_source, a_loudness, a_result);
			}
		}

		void ShotNoise(RE::Actor* a_actor, std::int32_t a_loudness)
		{
			if (KeepLouderAction(a_actor, a_loudness)) {
				LogNoise(a_actor, "shot", a_loudness, "a louder noise stays");
				return;
			}
			LogNoise(a_actor, "shot", a_loudness, "set");
			reinterpret_cast<ActionNoise_t*>(g_originalShotNoise)(a_actor, a_loudness);
		}

		void SpellNoise(RE::Actor* a_actor, std::int32_t a_loudness)
		{
			if (KeepLouderAction(a_actor, a_loudness)) {
				LogNoise(a_actor, "spell", a_loudness, "a louder noise stays");
				return;
			}
			LogNoise(a_actor, "spell", a_loudness, "set");
			reinterpret_cast<ActionNoise_t*>(g_originalSpellNoise)(a_actor, a_loudness);
		}

		void MeleeNoise(std::uintptr_t a_original, RE::Actor* a_actor, std::int32_t a_loudness)
		{
			const char* source = a_actor && a_actor->IsInKillMove() ? (a_actor->IsSneaking() ? "sneak killmove blow" : "killmove blow") : "blow";
			if (a_actor && a_actor->IsInKillMove() && a_actor->IsSneaking()) {
				if (const auto settings = GetSettings(); settings->quietSneakKillmove) {
					a_loudness = std::clamp(a_loudness, settings->killmoveNoiseMin, (std::max)(settings->killmoveNoiseMin, settings->killmoveNoiseMax));
				}
			}
			if (KeepLouderAction(a_actor, a_loudness)) {
				LogNoise(a_actor, source, a_loudness, "a louder noise stays");
				return;
			}
			LogNoise(a_actor, source, a_loudness, "set");
			reinterpret_cast<ActionNoise_t*>(a_original)(a_actor, a_loudness);
		}

		void BlowNoise(RE::Actor* a_actor, std::int32_t a_loudness) { MeleeNoise(g_originalBlowNoise, a_actor, a_loudness); }

		std::uintptr_t g_originalVRContactNoise{ 0 };
		std::uintptr_t g_originalVRSwingNoise{ 0 };
		void           VRContactNoise(RE::Actor* a_actor, std::int32_t a_loudness) { MeleeNoise(g_originalVRContactNoise, a_actor, a_loudness); }
		void           VRSwingNoise(RE::Actor* a_actor, std::int32_t a_loudness) { MeleeNoise(g_originalVRSwingNoise, a_actor, a_loudness); }

		void EventNoise(std::uintptr_t a_original, const char* a_source, RE::AIProcess* a_process, RE::Actor* a_actor,
			const RE::NiPoint3* a_position, std::int32_t a_loudness, RE::TESObjectREFR* a_ref)
		{
			if (KeepLouderEvent(a_process, a_position, a_loudness)) {
				LogNoise(a_actor, a_source, a_loudness, "a louder noise nearby stays");
				return;
			}
			LogNoise(a_actor, a_source, a_loudness, "set");
			reinterpret_cast<DetectionEvent_t*>(a_original)(a_process, a_actor, a_position, a_loudness, a_ref);
		}

		void SpellEvent(RE::AIProcess* a_process, RE::Actor* a_actor, const RE::NiPoint3* a_position, std::int32_t a_loudness, RE::TESObjectREFR* a_source)
		{
			EventNoise(g_originalSpellEvent, "spell spot", a_process, a_actor, a_position, a_loudness, a_source);
		}

		void ExplosionEvent(RE::AIProcess* a_process, RE::Actor* a_actor, const RE::NiPoint3* a_position, std::int32_t a_loudness, RE::TESObjectREFR* a_source)
		{
			EventNoise(g_originalExplosionEvent, "explosion", a_process, a_actor, a_position, a_loudness, a_source);
		}

		void ImpactEvent(RE::AIProcess* a_process, RE::Actor* a_actor, const RE::NiPoint3* a_position, std::int32_t a_loudness, RE::TESObjectREFR* a_source)
		{
			EventNoise(g_originalImpactEvent, "impact", a_process, a_actor, a_position, a_loudness, a_source);
		}

		void SummonEvent(RE::AIProcess* a_process, RE::Actor* a_actor, const RE::NiPoint3* a_position, std::int32_t a_loudness, RE::TESObjectREFR* a_source)
		{
			if (GetSettings()->summonNoise && t_summon && t_summon->spell && a_actor) {
				const auto caster = t_summon->GetCasterActor();
				float      value = 1.0f;
				RE::BGSEntryPoint::HandleEntryPoint(RE::BGSEntryPoint::ENTRY_POINT::kModSpellCastingSoundEvent, caster ? caster.get() : a_actor, t_summon->spell, &value);
				if (value == 0.0f) {
					return;
				}
				if (const auto* effect = t_summon->spell->GetAVEffect()) {
					if (const auto amount = SoundAmount(effect->data.castingSoundLevel)) {
						a_loudness = *amount;
					}
				}
			}
			EventNoise(g_originalSummonEvent, "summon", a_process, a_actor, a_position, a_loudness, a_source);
		}

		void SummonPlace(RE::ActiveEffect* a_effect)
		{
			auto* const previous = std::exchange(t_summon, a_effect);
			reinterpret_cast<SummonPlace_t*>(g_originalSummonPlace)(a_effect);
			t_summon = previous;
		}

		void SummonHandleEvent(RE::ActiveEffect* a_effect, void* a_event)
		{
			auto* const previous = std::exchange(t_summon, a_effect);
			reinterpret_cast<SummonEvent_t*>(g_originalSummonHandleEvent)(a_effect, a_event);
			t_summon = previous;
		}

		void InstallNoiseSources()
		{
			const REL::CallsiteID action{ REL::RelocationID(36701, 37711), 0x602C00 };
			const REL::CallsiteID event{ REL::RelocationID(38311, 39286) };
			const REL::CallsiteID release{ REL::RelocationID(33668, 34448), 0x553F50 };
			const REL::CallsiteID place{ REL::RelocationID(34197, 34993), 0x56A140 };
			auto&                 trampoline = SKSE::GetTrampoline();

			std::size_t noise = 0;
			std::size_t wanted = 0;
			const auto  hook = [&](REL::CallsiteID a_owner, REL::CallsiteID a_target, auto a_function, std::uintptr_t& a_original) {
                ++wanted;
                if (const auto address = Site(a_owner, a_target)) {
                    a_original = trampoline.write_call<5>(address, a_function);
                    ++noise;
                    return true;
                }
                return false;
			};
			hook(REL::CallsiteID{ REL::RelocationID(17693, 18102) }, action, ShotNoise, g_originalShotNoise);
			hook(release, action, SpellNoise, g_originalSpellNoise);
			bool blow = hook(REL::CallsiteID{ REL::RelocationID(37650, 38603) }, action, BlowNoise, g_originalBlowNoise);
			hook(release, event, SpellEvent, g_originalSpellEvent);
			hook(REL::CallsiteID{ REL::RelocationID(42696, 43869) }, event, ExplosionEvent, g_originalExplosionEvent);
			hook(REL::CallsiteID{ REL::RelocationID(43022, 44213) }, event, ImpactEvent, g_originalImpactEvent);
			if (noise == wanted) {
				logs::info("Louder noise stays: active");
			} else {
				logs::info("Louder noise stays: active for {} of {} kinds of noise - another mod probably changes the other places", noise, wanted);
			}
			logs::info("Quiet sneak killmoves: {}", blow ? "active" : "not active - another mod probably changes the same place");
			if (REL::Module::IsVR()) {
				const REL::CallsiteID contact{ REL::RelocationID(0, 0), 0x6B3200 };
				const REL::CallsiteID swing{ REL::RelocationID(0, 0), 0x6B9130 };
				const bool            melee = hook(contact, action, VRContactNoise, g_originalVRContactNoise);
				const bool            swings = hook(swing, action, VRSwingNoise, g_originalVRSwingNoise);
				logs::info("VR melee noise: {}", melee && swings ? "active" : "not active - another mod probably changes the same place");
			}

			bool summon = hook(place, event, SummonEvent, g_originalSummonEvent);
			if (summon) {
				const auto update = Site(REL::CallsiteID{ REL::RelocationID(34196, 34992), 0x569E70 }, place);
				if (update) {
					g_originalSummonPlace = trampoline.write_call<5>(update, SummonPlace);
					REL::Relocation<std::uintptr_t> vtable{ RE::VTABLE_SummonCreatureEffect[0] };
					g_originalSummonHandleEvent = vtable.write_vfunc(0x0D, SummonHandleEvent);
				} else {
					summon = false;
				}
			}
			logs::info("Summons as loud as their spell: {}", summon ? "active" : "not active - another mod probably changes the same place");
		}
	}

	namespace
	{
		std::uintptr_t                                  g_originalImpactData{ 0 };
		std::uintptr_t                                  g_originalImpactAmount{ 0 };
		std::unordered_set<const RE::BGSImpactDataSet*> g_weaponImpactSets;
		std::unordered_set<const RE::BGSImpactDataSet*> g_spellImpactSets;
		std::atomic<bool>                               g_impactSetsReady{ false };

		struct ImpactPick
		{
			const RE::BGSImpactData* data{ nullptr };
			RE::MATERIAL_ID          material{ RE::MATERIAL_ID::kNone };
			bool                     weapon{ false };
		};
		thread_local ImpactPick t_impact;

		enum class Surface
		{
			kOther,
			kSoft,
			kWood,
			kStone,
			kMetal,
			kWater
		};

		Surface SurfaceOf(RE::MATERIAL_ID a_material)
		{
			using M = RE::MATERIAL_ID;
			switch (a_material) {
			case M::kSnow:
			case M::kSnowStairs:
			case M::kDirt:
			case M::kGrass:
			case M::kSand:
			case M::kMud:
			case M::kAsh:
			case M::kSkin:
			case M::kSkinSmall:
			case M::kSkinLarge:
			case M::kMeat:
			case M::kOrganic:
			case M::kOrganicLarge:
			case M::kCloth:
			case M::kCarpet:
			case M::kBasket:
			case M::kBook:
			case M::kInsect:
			case M::kDLC1DeerSkin:
			case M::kDLC1SabreCatPelt:
			case M::kWeb:
			case M::kBoneActor:
				return Surface::kSoft;
			case M::kWood:
			case M::kWoodLight:
			case M::kWoodHeavy:
			case M::kWoodStairs:
			case M::kWoodAsStairs:
			case M::kBarrel:
			case M::kBowsStaves:
			case M::kBlockBowsStaves:
			case M::kCarriageWheel:
			case M::kDLC1SwingingBridge:
				return Surface::kWood;
			case M::kStone:
			case M::kStoneBroken:
			case M::kStoneStairs:
			case M::kStoneHeavy:
			case M::kStoneAsStairs:
			case M::kStoneStairsBroken:
			case M::kGravel:
			case M::kBoulderSmall:
			case M::kBoulderMedium:
			case M::kBoulderLarge:
			case M::kCeramicMedium:
			case M::kGlass:
			case M::kGlassStairs:
			case M::kBottle:
			case M::kBottleSmall:
			case M::kIce:
			case M::kIceForm:
			case M::kBone:
			case M::kDraugrSkeleton:
			case M::kDragonSkeleton:
			case M::kSkinSkeleton:
				return Surface::kStone;
			case M::kMetalLight:
			case M::kMetalSolid:
			case M::kMetalHeavy:
			case M::kChain:
			case M::kChainMetal:
			case M::kArmorLight:
			case M::kArmorHeavy:
			case M::kShieldLight:
			case M::kShieldHeavy:
			case M::kBlade1Hand:
			case M::kBlade1HandSmall:
			case M::kBlade2Hand:
			case M::kAxe1Hand:
			case M::kBlunt1Hand:
			case M::kBlunt2Hand:
			case M::kBlockBlade1Hand:
			case M::kBlockBlade2Hand:
			case M::kBlockBlunt:
			case M::kBlockBlunt2Hand:
			case M::kBlockAxe:
			case M::kPotsPans:
			case M::kCoin:
			case M::kAmulet:
			case M::kTrap:
			case M::kSkinMetalLarge:
			case M::kSkinMetalSmall:
			case M::kSteelGreatSword:
			case M::kTrailerSteelSword:
			case M::kArrow:
				return Surface::kMetal;
			case M::kWater:
			case M::kWaterPuddle:
				return Surface::kWater;
			default:
				return Surface::kOther;
			}
		}

		RE::BGSImpactData* ImpactData(RE::BGSImpactDataSet* a_set, RE::BGSMaterialType* a_material)
		{
			using func_t = RE::BGSImpactData*(RE::BGSImpactDataSet*, RE::BGSMaterialType*);
			auto*      data = reinterpret_cast<func_t*>(g_originalImpactData)(a_set, a_material);
			const bool weapon = g_impactSetsReady.load(std::memory_order_acquire) && g_weaponImpactSets.contains(a_set) &&
			                    !g_spellImpactSets.contains(a_set);
			t_impact = { data, a_material ? a_material->materialID : RE::MATERIAL_ID::kNone, weapon };
			return data;
		}

		std::int32_t ImpactAmount(RE::BGSImpactData* a_data)
		{
			using func_t = std::int32_t(RE::BGSImpactData*);
			const auto amount = reinterpret_cast<func_t*>(g_originalImpactAmount)(a_data);
			const auto pick = std::exchange(t_impact, {});
			const auto settings = GetSettings();
			if (!settings->impactNoise || !pick.weapon || pick.data != a_data || amount != GameSettings::Int("iSoundLevelNormal", 50)) {
				return amount;
			}
			switch (SurfaceOf(pick.material)) {
			case Surface::kSoft:
				return settings->impactSoft;
			case Surface::kWood:
				return settings->impactWood;
			case Surface::kStone:
				return settings->impactStone;
			case Surface::kMetal:
				return settings->impactMetal;
			case Surface::kWater:
				return settings->impactWater;
			default:
				return amount;
			}
		}

		void InstallImpacts()
		{
			const REL::CallsiteID owner{ REL::RelocationID(43022, 44213) };
			const auto            lookup = REL::try_resolve_callsite(owner,
						   REL::AUTO_CALLSITE(REL::CallsiteID{ REL::RelocationID(20408, 20860), 0x2D4C00 }).or_offset(0xBC, REL::Version{ 1, 6, 1170, 0 }));
			const auto            amount = REL::try_resolve_callsite(owner,
						   REL::AUTO_CALLSITE(REL::CallsiteID{ REL::RelocationID(20390, 20842), 0x2D43D0 }).or_offset(0x11C, REL::Version{ 1, 6, 1170, 0 }));
			if (!lookup || !amount) {
				logs::info("Arrow impacts by surface: not active - another mod probably changes the same place.");
				return;
			}
			g_originalImpactData = SKSE::GetTrampoline().write_call<5>(*lookup.address, ImpactData);
			g_originalImpactAmount = SKSE::GetTrampoline().write_call<5>(*amount.address, ImpactAmount);
			logs::info("Arrow impacts by surface: active");
		}
	}

	namespace
	{
		using OnTargetAttacked_t = void(RE::CombatGroup* a_group, RE::Actor* a_victim, RE::Actor* a_attacker);
		std::uintptr_t  g_originalAttacked{ 0 };
		constexpr float kMeleeReach = 300.0f;

		RE::NiPoint3 GuessFrom(const RE::NiPoint3& a_victim, const RE::NiPoint3& a_attacker, float a_error)
		{
			thread_local std::mt19937             random{ std::random_device{}() };
			std::uniform_real_distribution<float> spread{ -1.0f, 1.0f };
			const float                           factor = std::clamp(1.0f + spread(random) * a_error, 0.3f, 2.0f);
			const float                           angle = spread(random) * a_error * 0.9f;
			const auto                            offset = a_attacker - a_victim;
			const float                           c = std::cos(angle);
			const float                           s = std::sin(angle);
			return { a_victim.x + (offset.x * c - offset.y * s) * factor, a_victim.y + (offset.x * s + offset.y * c) * factor,
				a_victim.z + offset.z * factor };
		}

		void OnTargetAttacked(RE::CombatGroup* a_group, RE::Actor* a_victim, RE::Actor* a_attacker)
		{
			reinterpret_cast<OnTargetAttacked_t*>(g_originalAttacked)(a_group, a_victim, a_attacker);

			const auto settings = GetSettings();
			if (!settings->hitGuess || !a_group || !a_victim || !a_attacker || !a_attacker->IsPlayerRef()) {
				return;
			}
			const auto victimPos = a_victim->GetPosition();
			if (victimPos.GetDistance(a_attacker->GetPosition()) < kMeleeReach) {
				return;
			}
			const auto handle = a_attacker->GetHandle().native_handle();
			a_group->lock.LockForWrite();
			const auto unlock = [&]() { a_group->lock.UnlockForWrite(); };
			for (auto& target : a_group->targets) {
				if (target.targetHandle.native_handle() != handle) {
					continue;
				}
				if (target.detectLevel > 0) {
					unlock();
					return;
				}
				const float exposure = PlayerExposure(*settings);
				const float darkness = exposure >= 0.0f ? 1.0f - exposure : 0.5f;
				const float sneak = std::clamp(a_attacker->AsActorValueOwner()->GetActorValue(RE::ActorValue::kSneak) * 0.01f, 0.0f, 1.0f);
				const float error = settings->hitGuessError * (0.5f + 0.5f * darkness) * (1.0f + 0.5f * sneak);
				const auto  real = target.searchLoc.pos;
				const auto  guess = GuessFrom(victimPos, real, error);
				target.searchLoc.pos = guess;
				for (auto& spot : a_group->searchLocations) {
					if (spot.loc.pos.x == real.x && spot.loc.pos.y == real.y && spot.loc.pos.z == real.z) {
						spot.loc.pos = guess;
					}
				}
				unlock();
				logs::debug("Hit from hiding: guessed {:.0f} units off at {:.0f} units", guess.GetDistance(real), victimPos.GetDistance(real));
				return;
			}
			unlock();
		}

		void InstallHitGuess()
		{
			const REL::CallsiteID owner{ REL::RelocationID(32473, 33220), 0x50DD90 };
			const REL::CallsiteID target{ REL::RelocationID(43513, 44739), 0x797850 };
			const auto            site = REL::try_resolve_callsite(owner,
						   REL::AUTO_CALLSITE(target).or_offset(0x98, REL::Version{ 1, 6, 1170, 0 }));
			if (!site) {
				logs::info("Hit from hiding: not active - another mod probably changes the same place.");
				return;
			}
			g_originalAttacked = SKSE::GetTrampoline().write_call<5>(*site.address, OnTargetAttacked);
			logs::info("Hit from hiding: active");
		}
	}

	namespace
	{
		struct WitnessQuery
		{
			std::uint8_t*      witnesses;
			RE::Actor*         perpetrator;
			RE::TESObjectREFR* victim;
		};
		using CollectWitnesses_t = void(RE::ProcessLists* a_lists, WitnessQuery* a_query);
		std::uintptr_t g_originalWitnesses{ 0 };

		void CollectWitnesses(RE::ProcessLists* a_lists, WitnessQuery* a_query)
		{
			reinterpret_cast<CollectWitnesses_t*>(g_originalWitnesses)(a_lists, a_query);
			if (!GetSettings()->noAnimalWitnesses || !a_query || !a_query->witnesses) {
				return;
			}
			auto*         data = *reinterpret_cast<RE::RefHandle**>(a_query->witnesses + 0x08);
			auto&         size = *reinterpret_cast<std::uint32_t*>(a_query->witnesses + 0x18);
			std::uint32_t kept = 0;
			for (std::uint32_t i = 0; data && i < size; ++i) {
				RE::NiPointer<RE::TESObjectREFR> ref;
				const bool                       animal = RE::LookupReferenceByHandle(data[i], ref) && ref && ref->IsAnimal();
				if (!animal) {
					data[kept++] = data[i];
				}
			}
			if (data && kept != size) {
				logs::debug("Crime witnesses: {} animal(s) left out", size - kept);
				size = kept;
			}
		}

		using SharedCrimeFaction_t = bool(RE::Actor* a_actor, RE::TESFaction* a_faction);
		std::uintptr_t g_originalAlarmFaction{ 0 };

		bool AlarmReceiver(RE::Actor* a_actor, RE::TESFaction* a_faction)
		{
			if (GetSettings()->noAnimalWitnesses && a_actor && a_actor->IsAnimal()) {
				return false;
			}
			return reinterpret_cast<SharedCrimeFaction_t*>(g_originalAlarmFaction)(a_actor, a_faction);
		}

		void InstallWitnesses()
		{
			struct Site
			{
				REL::VariantID owner;
				std::size_t    se;
				std::size_t    ae;
			};
			const std::array<Site, 6>             sites{ {
                { REL::VariantID(36427, 37422, 0x5E5DC0), 0x738, 0x73D },
                { REL::VariantID(36428, 37423, 0x5E67B0), 0x3DD, 0x3DF },
                { REL::VariantID(36430, 37425, 0x5E6EC0), 0x277, 0x25E },
                { REL::VariantID(36431, 37426, 0x5E7570), 0x303, 0x305 },
                { REL::VariantID(36432, 37427, 0x5E7C80), 0x392, 0x372 },
                { REL::VariantID(36433, 37428, 0x5E8180), 0x15F, 0x15F },
            } };
			const REL::Relocation<std::uintptr_t> collector{ REL::VariantID(37085, 38094, 0x61AC80) };
			std::size_t                           hooked = 0;
			for (const auto& site : sites) {
				const auto   address = site.owner.address() + (REL::Module::IsAE() ? site.ae : site.se);
				const auto*  code = reinterpret_cast<const std::uint8_t*>(address);
				std::int32_t rel = 0;
				std::memcpy(&rel, code + 1, sizeof(rel));
				if (code[0] != 0xE8 || address + 5 + rel != collector.address()) {
					continue;
				}
				g_originalWitnesses = SKSE::GetTrampoline().write_call<5>(address, CollectWitnesses);
				++hooked;
			}
			{
				const REL::Relocation<std::uintptr_t> dispatcher{ REL::VariantID(38667, 39695, 0x67D8B0) };
				const REL::Relocation<std::uintptr_t> factionTest{ REL::VariantID(36674, 37682, 0x6009B0) };
				const auto                            address = dispatcher.address() + (REL::Module::IsAE() ? 0x32E : 0x29C);
				const auto*                           code = reinterpret_cast<const std::uint8_t*>(address);
				std::int32_t                          rel = 0;
				std::memcpy(&rel, code + 1, sizeof(rel));
				if (code[0] == 0xE8 && address + 5 + rel == factionTest.address()) {
					g_originalAlarmFaction = SKSE::GetTrampoline().write_call<5>(address, AlarmReceiver);
					++hooked;
				}
			}
			if (hooked == 0) {
				logs::info("Animals are no crime witnesses: not active - another mod probably changes the same place.");
				return;
			}
			logs::info("Animals are no crime witnesses: active{}", hooked < sites.size() + 1 ? " (not for every kind of crime)" : "");
		}
	}

	namespace
	{
		constexpr auto          kSampleStep = std::chrono::milliseconds(250);
		constexpr auto          kFastSampleStep = std::chrono::milliseconds(33);
		std::atomic<bool>       g_fastSampling{ false };
		std::atomic<RE::FormID> g_lastCell{ 0 };
		constexpr auto          kCellSettleTime = std::chrono::seconds(3);
		std::atomic<bool>       g_samplerStarted{ false };
		std::atomic<bool>       g_taskPending{ false };

		void UpdateSpots()
		{
			std::vector<Spot>       projectiles;
			std::vector<CorpseSpot> corpses;
			Spot                    sound;
			const auto              settings = GetSettings();
			auto*                   player = RE::PlayerCharacter::GetSingleton();

			static Clock::time_point cellChanged{};
			const auto*              cell = player ? player->GetParentCell() : nullptr;
			const auto               cellID = cell ? cell->GetFormID() : 0;
			if (cellID != g_lastCell) {
				g_lastCell = cellID;
				cellChanged = Clock::now();
			}
			const bool  settled = cell && player->Get3D() && Clock::now() - cellChanged >= kCellSettleTime;
			const float base = settled ? LightSensor::BaseLight() : -1.0f;
			g_baseLight = base;

			if (base >= 0.0f) {
				const bool                        events = g_eventHooks && settings->eventRealLight;
				std::vector<RE::ProjectileHandle> handles;
				if (auto* manager = events ? RE::Projectile::Manager::GetSingleton() : nullptr) {
					RE::BSSpinLockGuard guard{ manager->projectileLock };
					handles.assign(manager->unlimited.begin(), manager->unlimited.end());
				}
				for (const auto& handle : handles) {
					const auto projectile = handle.get();
					if (!projectile || projectile->GetProjectileRuntimeData().shooter.get().get() != player) {
						continue;
					}
					const auto position = projectile->GetPosition();
					if (position.GetDistance(player->GetPosition()) > kSpotRange) {
						continue;
					}
					if (const float light = LightSensor::MeasureAt(position); light >= 0.0f) {
						projectiles.push_back({ position, light, handle.native_handle() });
					}
					if (projectiles.size() >= kMaxSpots) {
						break;
					}
				}
				if (g_formulaActive && settings->corpseLight) {
					if (auto* lists = RE::ProcessLists::GetSingleton()) {
						const auto from = player->GetPosition();
						for (auto* list : { &lists->highActorHandles, &lists->middleHighActorHandles, &lists->middleLowActorHandles }) {
							for (auto& handle : *list) {
								const auto actor = handle.get();
								if (corpses.size() >= kMaxCorpses) {
									break;
								}
								if (!actor || !actor->IsDead() || actor->GetPosition().GetDistance(from) > kSpotRange) {
									continue;
								}
								const auto position = actor->GetPosition();
								const auto now = Clock::now();
								{
									std::scoped_lock lock{ g_spotLock };
									const auto       it = std::ranges::find(g_corpseSpots, actor->GetFormID(), &CorpseSpot::corpse);
									if (it != g_corpseSpots.end() && it->position.GetDistance(position) < 16.0f &&
										now - it->measured < std::chrono::seconds(5)) {
										corpses.push_back(*it);
										continue;
									}
								}
								auto point = position;
								point.z += 20.0f;
								if (const float light = LightSensor::MeasureAt(point, actor.get()); light >= 0.0f) {
									corpses.push_back({ actor->GetFormID(), light, position, now });
								}
							}
						}
					}
				}
				if (const auto* process = player->GetActorRuntimeData().currentProcess; events && process && process->high) {
					if (const auto* event = process->high->actorsGeneratedDetectionEvent) {
						sound = { event->location, LightSensor::MeasureAt(event->location) };
					}
				}
			}
			g_fastSampling = !projectiles.empty();
			std::scoped_lock lock{ g_spotLock };
			g_projectileSpots = std::move(projectiles);
			g_corpseSpots = std::move(corpses);
			g_soundSpot = sound;
		}

		void OnTick()
		{
			if (auto* ui = RE::UI::GetSingleton(); !ui || !ui->GameIsPaused()) {
				UpdateSpots();
				Unconscious::Update();
			}
			g_taskPending = false;
		}

		void SamplerLoop()
		{
			while (g_samplerStarted) {
				std::this_thread::sleep_for(g_fastSampling ? kFastSampleStep : kSampleStep);
				if (const auto* main = RE::Main::GetSingleton(); main && main->GetRuntimeData().quitGame) {
					g_samplerStarted = false;
					break;
				}
				if (g_taskPending.exchange(true)) {
					continue;
				}
				if (const auto* tasks = SKSE::GetTaskInterface()) {
					tasks->AddTask([]() { OnTick(); });
				} else {
					g_taskPending = false;
				}
			}
		}
	}

	namespace
	{
		using HasTarget_t = bool(RE::Actor* a_target, RE::Actor* a_attacker);
		using DetectionLevel_t = std::int32_t(RE::Actor* a_observer, RE::Actor* a_target, std::int32_t a_priority);
		using HasFlags_t = bool(void* a_state, std::uint32_t a_flags);
		using SneakTest_t = bool(RE::Actor* a_actor);
		using Launch_t = RE::ProjectileHandle*(RE::ProjectileHandle* a_out, RE::Projectile::LaunchData* a_data);
		using Populate_t = void(RE::HitData* a_hitData, RE::Actor* a_aggressor, RE::Actor* a_target, RE::Projectile* a_projectile);

		std::uintptr_t g_originalSneakHasTarget{ 0 };
		std::uintptr_t g_originalSneakDetection{ 0 };
		std::uintptr_t g_originalSneakFlag{ 0 };
		std::uintptr_t g_originalSneakSwimFlag{ 0 };
		std::uintptr_t g_originalSneakTest{ 0 };
		std::uintptr_t g_originalLaunch{ 0 };
		std::uintptr_t g_originalPopulate{ 0 };

		struct ShotState
		{
			std::uint32_t handle{ 0 };
			bool          sneaking{ false };
			float         time{ 0.0f };
		};

		std::mutex                                           g_shotLock;
		std::unordered_map<const RE::Projectile*, ShotState> g_shots;
		thread_local std::optional<bool>                     t_shotSneaking;

		std::optional<std::int32_t> LastDetectionLevel(RE::Actor* a_observer, RE::Actor* a_target)
		{
			auto* const process = a_observer && a_target ? a_observer->GetActorRuntimeData().currentProcess : nullptr;
			if (!process || process->processLevel.get() != RE::PROCESS_TYPE::kHigh || !process->high) {
				return std::nullopt;
			}
			constexpr float           kMaxAge = 4.0f;
			const float               now = *DetectionClock();
			auto* const               high = process->high;
			const RE::BSReadLockGuard guard{ high->knowledgeLock };
			for (const auto& entry : high->knowledgeArray) {
				if (entry.first == a_target->GetFormID() && entry.second && entry.second->detectionState) {
					const auto& state = *entry.second->detectionState;
					return now - state.unk38 <= kMaxAge ? std::optional<std::int32_t>{ state.level } : std::nullopt;
				}
			}
			return std::nullopt;
		}

		std::int32_t DetectionWithLast(RE::Actor* a_observer, RE::Actor* a_target, std::int32_t a_level)
		{
			if (a_level != kNoDetectionState || !GetSettings()->sneakMissingDetection) {
				return a_level;
			}
			return LastDetectionLevel(a_observer, a_target).value_or(a_level);
		}

		std::int32_t SneakDetection(RE::Actor* a_observer, RE::Actor* a_target, std::int32_t a_priority)
		{
			const auto level = reinterpret_cast<DetectionLevel_t*>(g_originalSneakDetection)(a_observer, a_target, a_priority);
			return DetectionWithLast(a_observer, a_target, level);
		}

		bool SneakHasTarget(RE::Actor* a_target, RE::Actor* a_attacker)
		{
			const bool result = reinterpret_cast<HasTarget_t*>(g_originalSneakHasTarget)(a_target, a_attacker);
			if (!result || !a_target || !a_attacker) {
				return result;
			}
			const auto  settings = GetSettings();
			auto* const group = a_target->GetCombatGroup();
			if (!group) {
				return result;
			}
			const bool searching = group->searchState != 0;
			if (searching ? !settings->sneakSearchingGroup : !(settings->sneakWhileGroupBusy && group->unk150 > 0)) {
				return result;
			}
			const auto level = DetectionWithLast(a_target, a_attacker, a_target->RequestDetectionLevel(a_attacker, RE::DETECTION_PRIORITY::kNormal));
			return searching ? level <= 0 : level > 0;
		}

		bool ShotSneakState(bool a_live)
		{
			return t_shotSneaking ? *t_shotSneaking : a_live;
		}

		bool SneakFlag(void* a_state, std::uint32_t a_flags)
		{
			const bool live = reinterpret_cast<HasFlags_t*>(g_originalSneakFlag)(a_state, a_flags);
			return ShotSneakState(live);
		}

		bool SneakSwimFlag(void* a_state, std::uint32_t a_flags)
		{
			const bool live = reinterpret_cast<HasFlags_t*>(g_originalSneakSwimFlag)(a_state, a_flags);
			return t_shotSneaking ? false : live;
		}

		bool SneakTest(RE::Actor* a_actor)
		{
			const bool live = reinterpret_cast<SneakTest_t*>(g_originalSneakTest)(a_actor);
			return ShotSneakState(live);
		}

		RE::ProjectileHandle* SneakLaunch(RE::ProjectileHandle* a_out, RE::Projectile::LaunchData* a_data)
		{
			auto* const player = RE::PlayerCharacter::GetSingleton();
			const bool  mine = player && a_data && a_data->shooter == player;
			const bool  sneaking = mine && player->IsSneaking() && !player->AsActorState()->IsSwimming() && !player->IsOnMount();
			auto* const result = reinterpret_cast<Launch_t*>(g_originalLaunch)(a_out, a_data);
			if (mine && a_out && GetSettings()->sneakBowAtRelease) {
				if (const auto projectile = a_out->get()) {
					const auto            now = RE::GetDurationOfApplicationRunTime() * 0.001f;
					const std::lock_guard lock{ g_shotLock };
					std::erase_if(g_shots, [&](const auto& a_entry) { return now - a_entry.second.time > 30.0f; });
					g_shots[projectile.get()] = ShotState{ a_out->native_handle(), sneaking, now };
				}
			}
			return result;
		}

		void SneakPopulate(RE::HitData* a_hitData, RE::Actor* a_aggressor, RE::Actor* a_target, RE::Projectile* a_projectile)
		{
			std::optional<bool> shot;
			if (a_projectile && a_aggressor && a_aggressor->IsPlayerRef()) {
				const std::lock_guard lock{ g_shotLock };
				if (const auto it = g_shots.find(a_projectile); it != g_shots.end()) {
					if (GetSettings()->sneakBowAtRelease && it->second.handle == a_projectile->GetHandle().native_handle()) {
						shot = it->second.sneaking;
					}
					g_shots.erase(it);
				}
			}
			const auto previous = std::exchange(t_shotSneaking, shot);
			reinterpret_cast<Populate_t*>(g_originalPopulate)(a_hitData, a_aggressor, a_target, a_projectile);
			t_shotSneaking = previous;
		}

		std::uintptr_t FlagSite(REL::CallsiteID a_owner, REL::CallsiteID a_target, std::uint32_t a_flag)
		{
			const auto     calls = REL::resolve_callsites(a_owner, a_target);
			std::uintptr_t found = 0;
			for (const auto address : calls.addresses) {
				const auto* bytes = reinterpret_cast<const std::uint8_t*>(address - 16);
				for (std::size_t i = 0; i + 5 <= 16; ++i) {
					std::uint32_t imm;
					std::memcpy(&imm, bytes + i + 1, sizeof(imm));
					if (bytes[i] == 0xBA && imm == a_flag) {
						if (found) {
							return 0;
						}
						found = address;
						break;
					}
				}
			}
			return found;
		}

		void InstallSneakAttacks()
		{
			const REL::CallsiteID core{ REL::RelocationID(42844, 44016), 0x76F230 };
			auto&                 trampoline = SKSE::GetTrampoline();

			const auto hasTarget = Site(core, REL::CallsiteID{ REL::RelocationID(37618, 38571) });
			if (hasTarget) {
				g_originalSneakHasTarget = trampoline.write_call<5>(hasTarget, SneakHasTarget);
			}
			const REL::CallsiteID detection{ REL::RelocationID(36748, 37764) };
			const auto            checks = REL::resolve_callsites(core, detection);
			const auto            detectionSite = checks.addresses.size() == 2 ? checks.addresses[1] : 0;
			if (detectionSite) {
				g_originalSneakDetection = trampoline.write_call<5>(detectionSite, SneakDetection);
			}
			logs::info("Sneak attacks on searching NPCs who see you: {}", hasTarget ? "active" : "not active - another mod probably changes the same place");
			logs::info("Sneak attacks with outdated detection: {}", detectionSite ? "active" : "not active - another mod probably changes the same place");

			bool       bow = false;
			const auto launch = Site(REL::CallsiteID{ REL::RelocationID(17693, 18102) }, REL::CallsiteID{ REL::RelocationID(42928, 44108) });
			const auto populate = Site(REL::CallsiteID{ REL::RelocationID(37673, 38627), 0x631AF0 }, REL::CallsiteID{ REL::RelocationID(42833, 44002) });
			if (launch && populate) {
				if (REL::Module::IsAE()) {
					const REL::CallsiteID flags{ REL::RelocationID(0, 38986) };
					const auto            sneak = FlagSite(core, flags, 0x200);
					const auto            swim = FlagSite(core, flags, 0x400);
					if (sneak && swim) {
						g_originalSneakFlag = trampoline.write_call<5>(sneak, SneakFlag);
						g_originalSneakSwimFlag = trampoline.write_call<5>(swim, SneakSwimFlag);
						bow = true;
					}
				} else if (const auto test = Site(core, REL::CallsiteID{ REL::RelocationID(19632, 0), 0x2B19C0 })) {
					g_originalSneakTest = trampoline.write_call<5>(test, SneakTest);
					bow = true;
				}
				if (bow) {
					g_originalLaunch = trampoline.write_call<5>(launch, SneakLaunch);
					g_originalPopulate = trampoline.write_call<5>(populate, SneakPopulate);
				}
			}
			logs::info("Arrows count as you shot them: {}", bow ? "active" : "not active - another mod probably changes the same place");
		}
	}

	namespace
	{
		using Trespass_t = void(RE::Actor* a_doer, RE::TESObjectREFR* a_ref, RE::TESForm* a_owner, std::int32_t a_crime);

		enum class TrespassSite
		{
			kLockpick,
			kActivator,
			kMount,
			kFollowerActor,
			kFollowerLock,
			kFollowerDoor,
			kOwnedActor,
			kTotal
		};
		std::array<std::uintptr_t, static_cast<std::size_t>(TrespassSite::kTotal)> g_originalTrespass{};

		bool IsOwnerBase(const RE::Actor* a_actor, const RE::TESNPC* a_npc)
		{
			if (a_actor->GetActorBase() == a_npc) {
				return true;
			}
			const auto* leveled = a_actor->extraList.GetByType<RE::ExtraLeveledCreature>();
			return leveled && leveled->originalBase == a_npc;
		}

		bool OwnerWitnessNotices(const std::vector<RE::Actor*>& a_doers, RE::TESForm* a_owner, const RE::TESObjectREFR* a_victim = nullptr)
		{
			auto* const npc = a_owner ? a_owner->As<RE::TESNPC>() : nullptr;
			auto* const faction = a_owner ? a_owner->As<RE::TESFaction>() : nullptr;
			auto* const lists = RE::ProcessLists::GetSingleton();
			if ((!npc && !faction) || !lists || a_doers.empty()) {
				return true;
			}
			std::vector<RE::Actor*> actors;
			for (const auto& handle : lists->highActorHandles) {
				if (const auto actor = handle.get()) {
					actors.push_back(actor.get());
				}
			}
			RE::TESFaction* crimeFaction = npc ? npc->crimeFaction : faction;
			for (auto* const actor : actors) {
				if (!actor->IsDead() && (npc ? IsOwnerBase(actor, npc) : actor->IsInFaction(faction))) {
					crimeFaction = actor->GetCrimeFaction();
					break;
				}
			}
			const bool noAnimals = GetSettings()->noAnimalWitnesses;
			for (auto* const witness : actors) {
				if (witness->IsPlayerRef() || witness == a_victim || std::ranges::find(a_doers, witness) != a_doers.end() || witness->IsDead() ||
					witness->IsDisabled() ||
					witness->IsPlayerTeammate() || witness->IsCommandedActor() || witness->AsActorState()->IsUnconscious() || (noAnimals && witness->IsAnimal())) {
					continue;
				}
				const bool owner = (npc && IsOwnerBase(witness, npc)) || (faction && witness->IsInFaction(faction));
				if (!owner && !InSharedCrimeFaction(witness, crimeFaction)) {
					continue;
				}
				for (auto* const doer : a_doers) {
					auto level = witness->RequestDetectionLevel(doer, RE::DETECTION_PRIORITY::kNormal);
					if (level == kNoDetectionState && !doer->IsPlayerRef()) {
						level = witness->RequestDetectionLevel(RE::PlayerCharacter::GetSingleton(), RE::DETECTION_PRIORITY::kNormal);
					}
					if (level > 0) {
						return true;
					}
				}
			}
			return false;
		}

		using CommandedUse_t = void(RE::Actor* a_follower, RE::TESObjectREFR* a_target);
		std::uintptr_t          g_originalCommandedUse{ 0 };
		thread_local RE::Actor* t_commandedFollower{ nullptr };

		void CommandedUse(RE::Actor* a_follower, RE::TESObjectREFR* a_target)
		{
			auto* const previous = std::exchange(t_commandedFollower, a_follower);
			reinterpret_cast<CommandedUse_t*>(g_originalCommandedUse)(a_follower, a_target);
			t_commandedFollower = previous;
		}

		template <TrespassSite Site>
		void Trespass(RE::Actor* a_doer, RE::TESObjectREFR* a_ref, RE::TESForm* a_owner, std::int32_t a_crime)
		{
			const auto     original = reinterpret_cast<Trespass_t*>(g_originalTrespass[static_cast<std::size_t>(Site)]);
			const auto     settings = GetSettings();
			constexpr bool follower = Site == TrespassSite::kFollowerActor || Site == TrespassSite::kFollowerLock || Site == TrespassSite::kFollowerDoor;
			const bool     enabled = Site == TrespassSite::kOwnedActor ? settings->ownedAnimalWitnesses :
			                         follower                          ? settings->followerTrespass :
			                                                             settings->trespassWitnesses;
			if (!a_doer || !enabled ||
				!(a_doer->IsPlayerRef() || a_doer->IsPlayerTeammate())) {
				return original(a_doer, a_ref, a_owner, a_crime);
			}
			std::vector<RE::Actor*> doers;
			if constexpr (Site == TrespassSite::kFollowerActor || Site == TrespassSite::kFollowerLock) {
				if (t_commandedFollower) {
					doers.push_back(t_commandedFollower);
				} else if (auto* const lists = RE::ProcessLists::GetSingleton(); lists && a_ref) {
					RE::Actor* nearest = nullptr;
					float      best = 1500.0f * 1500.0f;
					for (const auto& handle : lists->highActorHandles) {
						const auto actor = handle.get();
						if (!actor || !actor->IsPlayerTeammate()) {
							continue;
						}
						if (const float d = actor->GetPosition().GetSquaredDistance(a_ref->GetPosition()); d < best) {
							best = d;
							nearest = actor.get();
						}
					}
					if (nearest) {
						doers.push_back(nearest);
					}
				}
			}
			if (doers.empty()) {
				doers.push_back(a_doer);
			}
			if (OwnerWitnessNotices(doers, a_owner, Site == TrespassSite::kOwnedActor ? a_ref : nullptr)) {
				original(a_doer, a_ref, a_owner, a_crime);
			} else {
				logs::debug("Trespass at {}: nobody who could report it noticed", a_ref ? a_ref->GetDisplayFullName() : "?");
			}
		}

		using HighestDetection_t = std::int32_t(RE::ProcessLists* a_lists, RE::Actor* a_actor, std::uint32_t* a_losCount);
		std::uintptr_t g_originalLockpickGate{ 0 };

		std::int32_t LockpickGate(RE::ProcessLists* a_lists, RE::Actor* a_actor, std::uint32_t* a_losCount)
		{
			const auto level = reinterpret_cast<HighestDetection_t*>(g_originalLockpickGate)(a_lists, a_actor, a_losCount);
			if (level <= 0 || !a_actor || !a_actor->IsPlayerRef() || !GetSettings()->trespassWitnesses) {
				return level;
			}
			auto* const lock = RE::LockpickingMenu::GetTargetReference();
			if (!lock || OwnerWitnessNotices({ a_actor }, lock->GetOwner())) {
				return level;
			}
			logs::debug("Lockpicking {}: nobody who could report it noticed", lock->GetDisplayFullName());
			return 0;
		}

		using PickpocketChance_t = std::int32_t(float a_thiefSkill, float a_targetSkill, std::int32_t a_value, float a_weight, RE::Actor* a_thief,
			RE::Actor* a_target, bool a_detected, RE::TESBoundObject* a_item);
		using ItemSide_t = bool(std::uint32_t a_filterFlag);
		std::uintptr_t    g_originalPickpocketChance{ 0 };
		std::uintptr_t    g_originalItemSide{ 0 };
		thread_local bool t_itemOfTarget{ true };

		bool ItemSide(std::uint32_t a_filterFlag)
		{
			t_itemOfTarget = reinterpret_cast<ItemSide_t*>(g_originalItemSide)(a_filterFlag);
			return t_itemOfTarget;
		}

		std::int32_t PickpocketChanceShown(float a_thiefSkill, float a_targetSkill, std::int32_t a_value, float a_weight, RE::Actor* a_thief,
			RE::Actor* a_target, bool a_detected, RE::TESBoundObject* a_item)
		{
			if (GetSettings()->pickpocketChanceShown) {
				if (a_target && a_target->AsActorState()->GetKnockState() != RE::KNOCK_STATE_ENUM::kNormal) {
					return 100;
				}
				if (!t_itemOfTarget) {
					a_value = 1;
				}
			}
			return reinterpret_cast<PickpocketChance_t*>(g_originalPickpocketChance)(a_thiefSkill, a_targetSkill, a_value, a_weight, a_thief, a_target,
				a_detected, a_item);
		}

		using ReportCrime_t = void(RE::Actor* a_witness, void* a_crime);
		std::uintptr_t g_originalSearchReport{ 0 };

		void SearchReport(RE::Actor* a_witness, void* a_crime)
		{
			if (a_witness && a_crime && GetSettings()->crimeSearchNeedsSight) {
				const auto*   crime = static_cast<const std::uint8_t*>(a_crime);
				RE::RefHandle criminalHandle;
				std::memcpy(&criminalHandle, crime + 0x0C, sizeof(criminalHandle));
				RE::NiPointer<RE::TESObjectREFR> criminalRef;
				RE::LookupReferenceByHandle(criminalHandle, criminalRef);
				auto* const criminal = criminalRef ? criminalRef->As<RE::Actor>() : nullptr;
				auto* const faction = *reinterpret_cast<RE::TESFaction* const*>(crime + 0x60);
				if (!criminal || a_witness->RequestDetectionLevel(criminal, RE::DETECTION_PRIORITY::kNormal) <= 0 ||
					(faction && !InSharedCrimeFaction(a_witness, faction))) {
					return;
				}
			}
			reinterpret_cast<ReportCrime_t*>(g_originalSearchReport)(a_witness, a_crime);
		}

		void InstallTrespass()
		{
			auto&                 trampoline = SKSE::GetTrampoline();
			const REL::CallsiteID caught{ REL::RelocationID(36432, 37427) };
			const auto            hook = [&](REL::CallsiteID a_owner, REL::VariantOffset a_offset, auto a_function, TrespassSite a_site,
                                  std::size_t a_index = 0, std::size_t a_count = 1) {
                const auto site = CallAt(a_owner, a_offset, caught, a_index, a_count);
                if (site) {
                    g_originalTrespass[static_cast<std::size_t>(a_site)] = trampoline.write_call<5>(site, a_function);
                }
                return site != 0;
			};
			const auto gate = CallAt(REL::CallsiteID{ REL::RelocationID(51099, 51981), 0x8C6EA0 }, REL::VariantOffset(0x56, 0x56, 0x56),
				REL::CallsiteID{ REL::RelocationID(40394, 41408) });
			if (gate) {
				g_originalLockpickGate = trampoline.write_call<5>(gate, LockpickGate);
			}
			const bool activator = hook(REL::CallsiteID{ REL::RelocationID(17300, 17702), 0x2358D0 }, REL::VariantOffset(0x98, 0xA4, 0x98),
				Trespass<TrespassSite::kActivator>, TrespassSite::kActivator);
			const bool mount = hook(REL::CallsiteID{ REL::RelocationID(39712, 40814), 0x6DE310 }, REL::VariantOffset(0x13F, 0x15E, 0x13F),
				Trespass<TrespassSite::kMount>, TrespassSite::kMount);
			logs::info("Locks and owned objects need a real witness: {}", gate && activator && mount ? "active" :
																		  gate || activator || mount ? "partly active - another mod probably changes some of the places" :
																									   "not active - another mod probably changes the same place");

			const REL::CallsiteID commanded{ REL::RelocationID(36496, 37496), 0x5EBDA0 };
			if (const auto use = CallAt(REL::CallsiteID{ REL::RelocationID(38438, 39436), 0x661300 }, REL::VariantOffset(0x1D0, 0x1D2, 0x1D0), commanded)) {
				g_originalCommandedUse = trampoline.write_call<5>(use, CommandedUse);
			}
			const bool followerActor = hook(commanded, REL::VariantOffset(0xE4, 0xE6, 0xE4), Trespass<TrespassSite::kFollowerActor>,
				TrespassSite::kFollowerActor, 0, 2);
			const bool followerLock = hook(commanded, REL::VariantOffset(0x1E9, 0x20E, 0x1E9), Trespass<TrespassSite::kFollowerLock>,
				TrespassSite::kFollowerLock, 1, 2);
			const bool door = hook(REL::CallsiteID{ REL::RelocationID(17521, 17922), 0x23DEA0 }, REL::VariantOffset(0x34C, 0x365, 0x34C),
				Trespass<TrespassSite::kFollowerDoor>, TrespassSite::kFollowerDoor);
			const bool owned = hook(REL::CallsiteID{ REL::RelocationID(37672, 38626) }, REL::VariantOffset(0x3F6, 0x40C, 0x3F6),
				Trespass<TrespassSite::kOwnedActor>, TrespassSite::kOwnedActor);
			logs::info("Hitting someone else's animal needs a real witness: {}", owned ? "active" : "not active - another mod probably changes the same place");
			logs::info("Commanded followers need a real witness: {}", followerActor && followerLock && door ? "active" :
																	  followerActor || followerLock || door ? "partly active - another mod probably changes some of the places" :
																											  "not active - another mod probably changes the same place");

			const REL::CallsiteID card{ REL::RelocationID(50201, 51130) };
			const auto            side = CallAt(card, REL::VariantOffset(0xA2, 0xA2, 0xA2), REL::CallsiteID{ REL::RelocationID(50943, 51820), 0x8B8EE0 });
			const auto            chance = CallAt(card, REL::VariantOffset(0x213, 0x213, 0x213), REL::CallsiteID{ REL::RelocationID(25822, 26379), 0x3CCD90 });
			if (side && chance) {
				g_originalItemSide = trampoline.write_call<5>(side, ItemSide);
				g_originalPickpocketChance = trampoline.write_call<5>(chance, PickpocketChanceShown);
			}
			logs::info("Pickpocket chance shown as rolled: {}", side && chance ? "active" : "not active - another mod probably changes the same place");

			const auto report = CallAt(REL::CallsiteID{ REL::RelocationID(38600, 39631), 0x667410 }, REL::VariantOffset(0x79, 0x79, 0x79),
				REL::CallsiteID{ REL::RelocationID(36671, 37679), 0x600490 });
			if (report) {
				g_originalSearchReport = trampoline.write_call<5>(report, SearchReport);
			}
			logs::info("Searching guards report only what they saw: {}", report ? "active" : "not active - another mod probably changes the same place");
		}
	}

	namespace
	{
		constexpr std::size_t kTargetSize = 0xA8;
		constexpr float       kNever = -std::numeric_limits<float>::max();

		struct GroupTargets
		{
			std::uint8_t* data{ nullptr };
			std::uint32_t count{ 0 };
		};

		GroupTargets TargetsOf(RE::CombatGroup* a_group)
		{
			const auto*  raw = reinterpret_cast<const std::uint8_t*>(a_group);
			GroupTargets result;
			std::memcpy(&result.data, raw + 0x08, sizeof(result.data));
			std::memcpy(&result.count, raw + 0x18, sizeof(result.count));
			return result;
		}

		std::uint8_t* TargetEntry(RE::CombatGroup* a_group, RE::RefHandle a_handle)
		{
			const auto targets = TargetsOf(a_group);
			for (std::uint32_t i = 0; targets.data && i < targets.count; ++i) {
				auto* const entry = targets.data + i * kTargetSize;
				if (*reinterpret_cast<const RE::RefHandle*>(entry) == a_handle) {
					return entry;
				}
			}
			return nullptr;
		}

		template <class F>
		void ForEachGroup(RE::CombatManager* a_manager, F a_function)
		{
			const auto*             raw = reinterpret_cast<const std::uint8_t*>(a_manager);
			RE::CombatGroup* const* groups;
			std::uint16_t           count;
			std::memcpy(&groups, raw + 0x08, sizeof(groups));
			std::memcpy(&count, raw + 0x12, sizeof(count));
			for (std::uint16_t i = 0; groups && i < count; ++i) {
				if (groups[i]) {
					a_function(groups[i]);
				}
			}
		}

		struct CachedFloat
		{
			const char*               name;
			float                     fallback;
			std::atomic<RE::Setting*> setting{ nullptr };
			float                     get()
			{
				auto* found = setting.load(std::memory_order_acquire);
				if (!found) {
					auto* const collection = RE::GameSettingCollection::GetSingleton();
					found = collection ? collection->GetSetting(name) : nullptr;
					if (!found) {
						return fallback;
					}
					setting.store(found, std::memory_order_release);
				}
				return found->GetFloat();
			}
		};
		CachedFloat g_stealthPointMax{ "fCombatStealthPointMax", 100.0f };
		CachedFloat g_stealthPointAttackedMax{ "fCombatStealthPointAttackedMaxValue", 50.0f };
		CachedFloat g_veryLowPriorityDistance{ "fCombatDetectionVeryLowPriorityDistance", 4096.0f };

		using MinStealthPoints_t = float(RE::CombatManager* a_manager, RE::Actor* a_actor);
		using StealthMeterValue_t = void(std::int32_t a_value, bool a_sneaking);
		std::uintptr_t                           g_originalMinStealthPoints{ 0 };
		std::uintptr_t                           g_originalStealthMeterValue{ 0 };
		thread_local std::optional<std::int32_t> t_combatEye;

		float CombatEyePoints(RE::CombatManager* a_manager, RE::Actor* a_actor, float a_vanilla);

		float CombatEye(RE::CombatManager* a_manager, RE::Actor* a_actor)
		{
			const float vanilla = reinterpret_cast<MinStealthPoints_t*>(g_originalMinStealthPoints)(a_manager, a_actor);
			t_combatEye.reset();
			if (a_manager && a_actor && GetSettings()->combatEye) {
				t_combatEye = 100 - static_cast<std::int32_t>(CombatEyePoints(a_manager, a_actor, vanilla));
			}
			return vanilla;
		}

		void StealthMeterValue(std::int32_t a_value, bool a_sneaking)
		{
			if (t_combatEye) {
				a_value = (std::max)(*t_combatEye, 0);
				t_combatEye.reset();
			}
			reinterpret_cast<StealthMeterValue_t*>(g_originalStealthMeterValue)(a_value, a_sneaking);
		}

		float CombatEyePoints(RE::CombatManager* a_manager, RE::Actor* a_actor, float a_vanilla)
		{
			const float maximum = g_stealthPointMax.get();
			if (maximum <= 0.0f) {
				return a_vanilla;
			}
			constexpr float kSearchEye = 0.7f;
			const auto      handle = a_actor->GetHandle().native_handle();
			float           lowest = 100.0f;
			ForEachGroup(a_manager, [&](RE::CombatGroup* a_group) {
				if (const auto* entry = TargetEntry(a_group, handle)) {
					const bool  lost = (*reinterpret_cast<const std::uint16_t*>(entry + 0xA6) & 2) != 0;
					const float points = std::clamp(*reinterpret_cast<const float*>(entry + 0x08) / maximum * 100.0f, 0.0f, 100.0f);
					lowest = (std::min)(lowest, lost ? 100.0f - kSearchEye * (100.0f - points) : points);
				}
			});
			return std::clamp(std::ceil(lowest), 0.0f, 100.0f);
		}

		using DetectionSet_t = void(void* a_state, std::int32_t a_level, bool a_inCone, bool a_lineOfSight, std::uint8_t a_kind,
			const RE::NiPoint3* a_seenAt, const RE::NiPoint3* a_position);
		std::uintptr_t g_originalLowProcessSet{ 0 };

		void LowProcessSet(void* a_state, std::int32_t a_level, bool a_inCone, bool a_lineOfSight, std::uint8_t a_kind, const RE::NiPoint3* a_seenAt,
			const RE::NiPoint3* a_position)
		{
			if (a_seenAt && GetSettings()->lowProcessDetection) {
				const auto* target = reinterpret_cast<const RE::Actor*>(reinterpret_cast<const std::uint8_t*>(a_seenAt) - 0x54);
				const auto* process = target->GetActorRuntimeData().currentProcess;
				if (process && process->InHighProcess()) {
					return;
				}
			}
			reinterpret_cast<DetectionSet_t*>(g_originalLowProcessSet)(a_state, a_level, a_inCone, a_lineOfSight, a_kind, a_seenAt, a_position);
		}

		using AddSearchLocation_t = void(RE::CombatGroup* a_group, const void* a_location, float a_duration, float a_start);
		std::uintptr_t g_originalStartSearchLocation{ 0 };

		void StartSearchLocation(RE::CombatGroup* a_group, const void* a_location, float a_duration, float a_start)
		{
			const auto add = reinterpret_cast<AddSearchLocation_t*>(g_originalStartSearchLocation);
			add(a_group, a_location, a_duration, a_start);
			if (!a_group || !a_location || !GetSettings()->searchLatestLocation) {
				return;
			}
			const auto targets = TargetsOf(a_group);
			const auto at = reinterpret_cast<const std::uint8_t*>(a_location);
			if (!targets.data || at < targets.data || at >= targets.data + targets.count * kTargetSize || (at - targets.data) % kTargetSize != 0x58) {
				return;
			}
			const auto* entry = at - 0x58;
			const float lastKnown = *reinterpret_cast<const float*>(entry + 0x90);
			const float attacked = *reinterpret_cast<const float*>(entry + 0x98);
			const float confirmed = *reinterpret_cast<const float*>(entry + 0x8C);
			if (lastKnown > attacked && *DetectionClock() - lastKnown < 30.0f) {
				add(a_group, entry + 0x10, confirmed != kNever ? 40.0f : 15.0f, kNever);
			}
		}

		using ApplySpell_t = void(RE::Actor* a_actor, RE::SpellItem* a_spell, RE::Actor* a_target, std::uint64_t a_unused);
		std::uintptr_t g_originalSneakSpell{ 0 };

		bool IsInvisibility(const RE::SpellItem* a_spell)
		{
			return a_spell && std::ranges::any_of(a_spell->effects, [](const RE::Effect* a_effect) {
				return a_effect && a_effect->baseEffect && a_effect->baseEffect->GetArchetype() == RE::EffectSetting::Archetype::kInvisibility;
			});
		}

		void ShadowWarriorGrace(RE::Actor* a_actor)
		{
			auto* const manager = RE::CombatManager::GetSingleton();
			if (!manager) {
				return;
			}
			const float grace = g_stealthPointAttackedMax.get();
			const auto  handle = a_actor->GetHandle().native_handle();
			ForEachGroup(manager, [&](RE::CombatGroup* a_group) {
				auto*                      lock = reinterpret_cast<RE::BSReadWriteLock*>(reinterpret_cast<std::uint8_t*>(a_group) + 0x160);
				const RE::BSWriteLockGuard guard{ *lock };
				if (auto* entry = TargetEntry(a_group, handle)) {
					auto& points = *reinterpret_cast<float*>(entry + 0x08);
					points = (std::max)(points, grace);
				}
			});
		}

		void SneakSpell(RE::Actor* a_actor, RE::SpellItem* a_spell, RE::Actor* a_target, std::uint64_t a_unused)
		{
			reinterpret_cast<ApplySpell_t*>(g_originalSneakSpell)(a_actor, a_spell, a_target, a_unused);
			if (a_actor && a_actor->IsPlayerRef() && GetSettings()->shadowWarriorGrace && IsInvisibility(a_spell)) {
				ShadowWarriorGrace(a_actor);
			}
		}

		using SneakEntry_t = void(std::uint32_t a_entryPoint, RE::Actor* a_actor, void* a_result);
		std::uintptr_t g_originalSneakEntry{ 0 };

		bool SelectSpellReplaced()
		{
			static const REL::Relocation<const std::uintptr_t*> slot{ REL::VariantID(675819, 369199, 0x1E90E10) };
			const auto                                          function = *slot.get();
			const auto                                          base = REL::Module::get().base();
			return function < base || function >= ImageEnd();
		}

		class SneakingSpells : public RE::PerkEntryVisitor
		{
		public:
			explicit SneakingSpells(RE::Actor* a_actor) :
				actor(a_actor)
			{}

			RE::BSContainer::ForEachResult Visit(RE::BGSPerkEntry* a_entry) override
			{
				RE::TESForm*           args[1]{ actor };
				constexpr std::uint8_t kSelectSpell = 10;
				if (a_entry && a_entry->CheckConditionFilters(1, args) && static_cast<std::uint8_t>(a_entry->GetFunction()) == kSelectSpell) {
					const auto* data = static_cast<const RE::BGSEntryPointFunctionData*>(a_entry->GetFunctionData());
					if (data && data->GetType() == RE::BGSEntryPointFunctionData::FunctionType::kSpellItem) {
						if (auto* const spell = static_cast<const RE::BGSEntryPointFunctionDataSpellItem*>(data)->spell) {
							spells.push_back(spell);
						}
					}
				}
				return RE::BSContainer::ForEachResult::kContinue;
			}

			RE::Actor*                  actor;
			std::vector<RE::SpellItem*> spells;
		};

		void CastSneakingSpell(RE::Actor* a_actor, RE::SpellItem* a_spell)
		{
			switch (a_spell->GetSpellType()) {
			case RE::MagicSystem::SpellType::kDisease:
			case RE::MagicSystem::SpellType::kAbility:
			case RE::MagicSystem::SpellType::kAddiction:
				a_actor->AddSpell(a_spell);
				return;
			default:
				break;
			}
			if (auto* const caster = a_actor->GetMagicCaster(RE::MagicSystem::CastingSource::kInstant)) {
				caster->CastSpellImmediate(a_spell, false, a_actor, 1.0f, false, 0.0f, nullptr);
			}
		}

		void SneakEntry(std::uint32_t a_entryPoint, RE::Actor* a_actor, void* a_result)
		{
			const auto settings = GetSettings();
			const auto original = reinterpret_cast<SneakEntry_t*>(g_originalSneakEntry);
			const bool replaced = SelectSpellReplaced() || g_originalSneakEntry < REL::Module::get().base() || g_originalSneakEntry >= ImageEnd();
			if (!a_actor || !a_result || replaced || !settings->allSneakingSpells) {
				original(a_entryPoint, a_actor, a_result);
				if (replaced && a_actor && a_actor->IsPlayerRef() && settings->shadowWarriorGrace) {
					SneakingSpells visitor{ a_actor };
					a_actor->ForEachPerkEntry(RE::BGSEntryPoint::ENTRY_POINT::kApplySneakingSpell, visitor);
					if (std::ranges::any_of(visitor.spells, IsInvisibility)) {
						ShadowWarriorGrace(a_actor);
					}
				}
				return;
			}
			SneakingSpells visitor{ a_actor };
			a_actor->ForEachPerkEntry(RE::BGSEntryPoint::ENTRY_POINT::kApplySneakingSpell, visitor);
			*static_cast<RE::SpellItem**>(a_result) = nullptr;
			for (auto* const spell : visitor.spells) {
				CastSneakingSpell(a_actor, spell);
			}
			if (a_actor->IsPlayerRef() && settings->shadowWarriorGrace && std::ranges::any_of(visitor.spells, IsInvisibility)) {
				ShadowWarriorGrace(a_actor);
			}
		}

		using UpdateTargetDetection_t = std::int32_t(RE::CombatGroup* a_group, RE::Actor* a_member, RE::Actor* a_target, void* a_state);
		std::uintptr_t g_originalTargetDetection{ 0 };

		std::int32_t TargetDetection(RE::CombatGroup* a_group, RE::Actor* a_member, RE::Actor* a_target, void* a_state)
		{
			const auto priority = reinterpret_cast<UpdateTargetDetection_t*>(g_originalTargetDetection)(a_group, a_member, a_target, a_state);
			if (priority == 2 && a_member && a_target && GetSettings()->detectionPriority) {
				const float veryLow = g_veryLowPriorityDistance.get();
				if (a_member->GetPosition().GetSquaredDistance(a_target->GetPosition()) > veryLow * veryLow) {
					return 1;
				}
			}
			return priority;
		}

		using CombatDialogue_t = bool(std::uint64_t a_manager, RE::Actor* a_speaker, RE::Actor* a_target, std::uint32_t a_type, std::uint32_t a_subtype,
			bool a_force, void* a_controller);
		std::uintptr_t g_originalGiveUpLine{ 0 };

		bool GiveUpLine(std::uint64_t a_manager, RE::Actor* a_speaker, RE::Actor* a_target, std::uint32_t a_type, std::uint32_t a_subtype, bool a_force,
			void* a_controller)
		{
			constexpr std::uint32_t kAlertToNormal = 60;
			constexpr std::uint32_t kCombatToNormal = 61;
			constexpr std::uint32_t kLostToNormal = 63;
			if (a_target && a_target->IsDead() && (a_subtype == kAlertToNormal || a_subtype == kLostToNormal) && GetSettings()->noGiveUpLineAfterKill) {
				a_subtype = kCombatToNormal;
			}
			return reinterpret_cast<CombatDialogue_t*>(g_originalGiveUpLine)(a_manager, a_speaker, a_target, a_type, a_subtype, a_force, a_controller);
		}

		void InstallCombat()
		{
			auto&      trampoline = SKSE::GetTrampoline();
			const auto hook = [&](REL::CallsiteID a_owner, REL::VariantOffset a_offset, REL::CallsiteID a_target, auto a_function, std::uintptr_t& a_original,
								  std::size_t a_index = 0, std::size_t a_count = 1) {
				const auto site = CallAt(a_owner, a_offset, a_target, a_index, a_count);
				if (site) {
					a_original = trampoline.write_call<5>(site, a_function);
				}
				return site != 0;
			};
			const auto report = [](const char* a_feature, bool a_active) {
				logs::info("{}: {}", a_feature, a_active ? "active" : "not active - another mod probably changes the same place");
			};
			{
				const REL::CallsiteID meter{ REL::RelocationID(39514, 40593), 0x6D1CE0 };
				const auto            points = CallAt(meter, REL::VariantOffset(0x41, 0x41, 0x41), REL::CallsiteID{ REL::RelocationID(45577, 46877), 0x7D15B0 });
				const auto            hud = CallAt(meter, REL::VariantOffset(0xC9, 0xC9, 0xC9), REL::CallsiteID{ REL::RelocationID(50728, 51623), 0x8AC9D0 });
				if (points && hud) {
					g_originalMinStealthPoints = trampoline.write_call<5>(points, CombatEye);
					g_originalStealthMeterValue = trampoline.write_call<5>(hud, StealthMeterValue);
				}
				report("Stealth eye in combat shows what they know", points && hud);
			}
			report("Far group members see normally",
				hook(REL::CallsiteID{ REL::RelocationID(43572, 44804), 0x79E020 }, REL::VariantOffset(0x130, 0x130, 0x130),
					REL::CallsiteID{ REL::RelocationID(41675, 42760), 0x7456D0 }, LowProcessSet, g_originalLowProcessSet));
			report("Search where they saw you last",
				hook(REL::CallsiteID{ REL::RelocationID(43536, 44762), 0x79A2A0 }, REL::VariantOffset(0x103, 0x102, 0x103),
					REL::CallsiteID{ REL::RelocationID(43544, 44770), 0x79B440 }, StartSearchLocation, g_originalStartSearchLocation, 0, 2));
			const bool entry = hook(REL::CallsiteID{ REL::RelocationID(36926, 37951) }, REL::VariantOffset(0xB6, 0x72, 0xB6),
				REL::CallsiteID{ REL::RelocationID(23073, 23526) }, SneakEntry, g_originalSneakEntry);
			const bool cast = hook(REL::CallsiteID{ REL::RelocationID(36926, 37951) }, REL::VariantOffset(0xCE, 0x8A, 0xCE),
				REL::CallsiteID{ REL::RelocationID(37817, 38766), 0x63B050 }, SneakSpell, g_originalSneakSpell);
			report("Every sneaking spell is cast (Shadow Warrior, Shadowcloak)", entry);
			report("Shadow Warrior gives stealth points", entry && cast);
			report("Far NPCs are checked less often",
				hook(REL::CallsiteID{ REL::RelocationID(43477, 44699), 0x795040 }, REL::VariantOffset(0x85, 0x85, 0x85),
					REL::CallsiteID{ REL::RelocationID(43501, 44725), 0x796920 }, TargetDetection, g_originalTargetDetection));
			report("Fitting line after a kill",
				hook(REL::CallsiteID{ REL::RelocationID(43571, 44803) }, REL::VariantOffset(0x135, 0x135, 0x135),
					REL::CallsiteID{ REL::RelocationID(43467, 44673), 0x7940F0 }, GiveUpLine, g_originalGiveUpLine));
		}
	}

	namespace
	{
		float*       g_clock{ nullptr };
		const float* g_frameTime{ nullptr };
		double       g_clockSum{ 0.0 };
		bool         g_clockSumValid{ false };

		void ClockStep()
		{
			if (!GetSettings()->preciseClock) {
				*g_clock += *g_frameTime;
				g_clockSumValid = false;
				return;
			}
			if (!g_clockSumValid || std::abs(g_clockSum - static_cast<double>(*g_clock)) > 1.0) {
				g_clockSum = *g_clock;
				g_clockSumValid = true;
			}
			g_clockSum += *g_frameTime;
			*g_clock = static_cast<float>(g_clockSum);
		}

		bool ClockBlockAt(std::uintptr_t a_site, std::uintptr_t a_clock, std::uintptr_t a_frame)
		{
			const auto* bytes = reinterpret_cast<const std::uint8_t*>(a_site);
			const auto  rel = [&](std::size_t a_at, std::uintptr_t a_target) {
                std::int32_t value;
                std::memcpy(&value, bytes + a_at, sizeof(value));
                return static_cast<std::intptr_t>(value) == static_cast<std::intptr_t>(a_target) - static_cast<std::intptr_t>(a_site + a_at + 4);
			};
			const auto op = [&](std::size_t a_at, std::uint8_t a_code) {
				return bytes[a_at] == 0xF3 && bytes[a_at + 1] == 0x0F && bytes[a_at + 2] == a_code && bytes[a_at + 3] == 0x05;
			};
			return op(0, 0x10) && rel(4, a_clock) && op(8, 0x58) && rel(12, a_frame) && op(16, 0x11) && rel(20, a_clock);
		}

		void InstallClock()
		{
			const REL::Relocation<std::uintptr_t> update{ RELOCATION_ID(35565, 36564) };
			const REL::Relocation<float*>         clock{ RELOCATION_ID(517597, 404125) };
			const REL::Relocation<const float*>   frame{ RELOCATION_ID(523660, 410199) };
			std::uintptr_t                        site = update.address() + REL::VariantOffset(0x269, 0x282, 0x2DB).offset();
			if (!ClockBlockAt(site, clock.address(), frame.address())) {
				site = 0;
				for (std::uintptr_t at = update.address(); at < update.address() + 0x600; ++at) {
					if (ClockBlockAt(at, clock.address(), frame.address())) {
						site = at;
						break;
					}
				}
			}
			if (!site) {
				logs::info("Precise detection clock: not active - another mod probably changes the same place");
				return;
			}
			g_clock = clock.get();
			g_frameTime = frame.get();
			SKSE::GetTrampoline().write_call<5>(site, ClockStep);
			REL::safe_fill(site + 5, 0x90, 19);
			logs::info("Precise detection clock: active");
		}
	}

	void Install()
	{
		SKSE::AllocTrampoline(1280);
		InstallFormula();
		InstallTouch();
		InstallEvents();
		if (g_formulaActive) {
			logs::info("Bodies in real light: active");
		} else {
			logs::info("Bodies in real light: not active - another mod probably changes the same place.");
		}
		InstallHitGuess();
		InstallWitnesses();
		InstallImpacts();
		InstallNoiseSources();
		InstallClock();
		InstallSneakAttacks();
		InstallTrespass();
		InstallCombat();
		InstallReactions();
		InstallCrimes();
		InstallLights();
		InstallMagic();
	}

	void OnDataLoaded()
	{
		if (auto* data = RE::TESDataHandler::GetSingleton()) {
			for (const auto* weapon : data->GetFormArray<RE::TESObjectWEAP>()) {
				if (weapon && weapon->impactDataSet) {
					g_weaponImpactSets.insert(weapon->impactDataSet);
				}
			}
			for (const auto* effect : data->GetFormArray<RE::EffectSetting>()) {
				if (effect && effect->data.impactDataSet) {
					g_spellImpactSets.insert(effect->data.impactDataSet);
				}
			}
		}
		g_impactSetsReady.store(true, std::memory_order_release);
		if (!g_samplerStarted.exchange(true)) {
			std::thread(SamplerLoop).detach();
		}
	}

	void ResetMemory()
	{
		g_touchedID = 0;
		g_touchedUntil = 0;
		g_contactID = 0;
		g_baseLight = -1.0f;
		g_lastCell = 0;
		{
			const std::lock_guard shots{ g_shotLock };
			g_shots.clear();
		}
		ResetMagic();
		std::scoped_lock lock{ g_spotLock };
		g_projectileSpots.clear();
		g_corpseSpots.clear();
		g_soundSpot = {};
	}

	bool ActionNoiseFixed()
	{
		return g_formulaActive && GetSettings()->actionNoiseFix;
	}

	void* SetDetectionFilter(void* a_filter)
	{
		if (!g_formulaActive) {
			return nullptr;
		}
		g_filter = reinterpret_cast<Formula_t*>(a_filter);
		logs::info("Detection rules from another mod: {}", a_filter ? "connected" : "disconnected");
		return reinterpret_cast<void*>(&CallGame);
	}
}

extern "C" __declspec(dllexport) void* StealthFixes_SetDetectionFilter(void* a_filter)
{
	return StealthFixes::Fixes::SetDetectionFilter(a_filter);
}

extern "C" __declspec(dllexport) bool StealthFixes_ActionNoiseFixed()
{
	return StealthFixes::Fixes::ActionNoiseFixed();
}
