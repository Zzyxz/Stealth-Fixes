#include "Settings.h"

#include <Windows.h>

namespace StealthFixes
{
	namespace
	{
		constexpr const char* kMainConfigFile = ".\\Data\\SKSE\\Plugins\\StealthFixes.ini";

		constexpr const char* kOverrideDir = ".\\Data\\SKSE\\Plugins\\StealthFixesCustomINI";

		constexpr const char* kUserConfigFile = ".\\Data\\SKSE\\Plugins\\StealthFixes_User.ini";

		constexpr const char* kLog = "Log";
		constexpr const char* kTouch = "Touch";
		constexpr const char* kEvents = "Events";
		constexpr const char* kBodies = "Bodies";
		constexpr const char* kLight = "Light";
		constexpr const char* kSearch = "Search";
		constexpr const char* kCrime = "Crime";
		constexpr const char* kImpacts = "Impacts";
		constexpr const char* kFormula = "Formula";
		constexpr const char* kNoise = "Noise";
		constexpr const char* kSpells = "Spells";
		constexpr const char* kSneakAttacks = "SneakAttacks";
		constexpr const char* kTrespass = "Trespass";
		constexpr const char* kCombat = "Combat";
		constexpr const char* kClock = "Clock";
		constexpr const char* kReactions = "Reactions";
		constexpr const char* kUnconscious = "Unconscious";

		struct IntOption
		{
			const char*   section;
			const char*   key;
			std::int32_t& (*get)(Settings&);
			std::int32_t  min;
			std::int32_t  max;
		};

		struct BoolOption
		{
			const char* section;
			const char* key;
			bool&       (*get)(Settings&);
		};

		struct FloatOption
		{
			const char* section;
			const char* key;
			float&      (*get)(Settings&);
			float       min;
			float       max;
		};

#define SF_BOOL(section, key, expr)                             \
	BoolOption                                                  \
	{                                                           \
		section, key, [](Settings& s) -> bool& { return expr; } \
	}
#define SF_INT(section, key, expr, lo, hi)                                      \
	IntOption                                                                   \
	{                                                                           \
		section, key, [](Settings& s) -> std::int32_t& { return expr; }, lo, hi \
	}
#define SF_FLOAT(section, key, expr, lo, hi)                             \
	FloatOption                                                          \
	{                                                                    \
		section, key, [](Settings& s) -> float& { return expr; }, lo, hi \
	}

		const std::array kBoolOptions{
			SF_BOOL(kTouch, "bEnabled", s.touchFix),
			SF_BOOL(kEvents, "bRealLight", s.eventRealLight),
			SF_BOOL(kEvents, "bSightFix", s.eventSightFix),
			SF_BOOL(kBodies, "bRealLight", s.corpseLight),
			SF_BOOL(kSearch, "bHitGuess", s.hitGuess),
			SF_BOOL(kCrime, "bNoAnimalWitnesses", s.noAnimalWitnesses),
			SF_BOOL(kImpacts, "bEnabled", s.impactNoise),
			SF_BOOL(kFormula, "bActionNoiseFix", s.actionNoiseFix),
			SF_BOOL(kFormula, "bEventPerception", s.eventPerception),
			SF_BOOL(kFormula, "bNightEyeBonusFix", s.nightEyeBonusFix),
			SF_BOOL(kEvents, "bProjectileSound", s.projectileSound),
			SF_BOOL(kNoise, "bLouderStays", s.louderStays),
			SF_BOOL(kNoise, "bQuietSneakKillmove", s.quietSneakKillmove),
			SF_BOOL(kSpells, "bSummonNoise", s.summonNoise),
			SF_BOOL(kSneakAttacks, "bSearchingGroup", s.sneakSearchingGroup),
			SF_BOOL(kSneakAttacks, "bMissingDetection", s.sneakMissingDetection),
			SF_BOOL(kSneakAttacks, "bBowAtRelease", s.sneakBowAtRelease),
			SF_BOOL(kSneakAttacks, "bWhileGroupBusy", s.sneakWhileGroupBusy),
			SF_BOOL(kTrespass, "bWitnesses", s.trespassWitnesses),
			SF_BOOL(kTrespass, "bFollowers", s.followerTrespass),
			SF_BOOL(kTrespass, "bPickpocketChanceShown", s.pickpocketChanceShown),
			SF_BOOL(kTrespass, "bSearchNeedsSight", s.crimeSearchNeedsSight),
			SF_BOOL(kCombat, "bStealthEye", s.combatEye),
			SF_BOOL(kCombat, "bFarMembers", s.lowProcessDetection),
			SF_BOOL(kCombat, "bSearchLatestLocation", s.searchLatestLocation),
			SF_BOOL(kCombat, "bShadowWarriorGrace", s.shadowWarriorGrace),
			SF_BOOL(kCombat, "bNoGiveUpLineAfterKill", s.noGiveUpLineAfterKill),
			SF_BOOL(kCombat, "bDetectionPriority", s.detectionPriority),
			SF_BOOL(kClock, "bPreciseClock", s.preciseClock),
			SF_BOOL(kEvents, "bNPCTargets", s.eventSightNPCs),
			SF_BOOL(kNoise, "bSprintAndBackwards", s.sprintCounts),
			SF_BOOL(kSpells, "bFrenzyUnseen", s.frenzyUnseen),
			SF_BOOL(kSpells, "bFrenzyKeepsEnemies", s.frenzyKeepsEnemies),
			SF_BOOL(kLight, "bNoHiddenLights", s.hiddenLightsOff),
			SF_BOOL(kLight, "bSteadyTorch", s.steadyTorch),
			SF_BOOL(kCrime, "bOpenMurderKeepsEnemy", s.openMurderKeepsEnemy),
			SF_BOOL(kCrime, "bGuardAlarmNearby", s.guardAlarmNearby),
			SF_BOOL(kCrime, "bAlarmWitnessesSee", s.alarmWitnessesSee),
			SF_BOOL(kReactions, "bNoticedOnly", s.reactionsNeedSight),
			SF_BOOL(kReactions, "bBowComment", s.bowCommentNeedsSight),
			SF_BOOL(kReactions, "bUnconscious", s.unconsciousNoticeNothing),
			SF_BOOL(kUnconscious, "bWakeOnHit", s.wakeOnHit),
			SF_BOOL(kUnconscious, "bAlliesWake", s.alliesWake),
			SF_BOOL(kTrespass, "bOwnedAnimals", s.ownedAnimalWitnesses),
			SF_BOOL(kCombat, "bAllSneakingSpells", s.allSneakingSpells),
		};

		const std::array kIntOptions{
			SF_INT(kImpacts, "iSoft", s.impactSoft, 0, 200),
			SF_INT(kImpacts, "iWood", s.impactWood, 0, 200),
			SF_INT(kImpacts, "iStone", s.impactStone, 0, 200),
			SF_INT(kImpacts, "iMetal", s.impactMetal, 0, 200),
			SF_INT(kImpacts, "iWater", s.impactWater, 0, 200),
			SF_INT(kNoise, "iKillmoveNoiseMin", s.killmoveNoiseMin, 0, 100),
			SF_INT(kNoise, "iKillmoveNoiseMax", s.killmoveNoiseMax, 0, 100),
		};

		const std::array kFloatOptions{
			SF_FLOAT(kTouch, "fGrace", s.touchGrace, 0.0f, 2.0f),
			SF_FLOAT(kUnconscious, "fWakeChance", s.wakeChanceMin, 0.0f, 1.0f),
			SF_FLOAT(kUnconscious, "fWakeDamage", s.wakeDamage, 0.05f, 1.0f),
			SF_FLOAT(kUnconscious, "fAllyRange", s.allyRange, 2.0f, 50.0f),
			SF_FLOAT(kUnconscious, "fAllyWakeTime", s.allyWakeTime, 0.5f, 15.0f),
			SF_FLOAT(kBodies, "fShadowUnaware", s.shadowUnaware, 0.0f, 1.0f),
			SF_FLOAT(kBodies, "fShadowAlerted", s.shadowAlerted, 0.0f, 1.0f),
			SF_FLOAT(kLight, "fExposureSpan", s.exposureSpan, 10.0f, 200.0f),
			SF_FLOAT(kSearch, "fHitGuessError", s.hitGuessError, 0.0f, 1.0f),
		};

#undef SF_BOOL
#undef SF_INT
#undef SF_FLOAT

		std::atomic<std::shared_ptr<const Settings>> g_settings{ std::make_shared<const Settings>() };

		std::optional<std::string> ReadRaw(const char* a_file, const char* a_section, const char* a_key)
		{
			char             buf[128]{};
			const auto       len = GetPrivateProfileStringA(a_section, a_key, "", buf, static_cast<DWORD>(sizeof(buf)), a_file);
			std::string_view value{ buf, len };

			if (const auto comment = value.find_first_of(";#"); comment != std::string_view::npos) {
				value = value.substr(0, comment);
			}
			const auto first = value.find_first_not_of(" \t");
			if (first == std::string_view::npos) {
				return std::nullopt;
			}
			const auto last = value.find_last_not_of(" \t");
			return std::string{ value.substr(first, last - first + 1) };
		}

		std::optional<bool> ParseBool(std::string_view a_text)
		{
			static constexpr std::array<std::pair<std::string_view, bool>, 6> kWords{ { { "true", true }, { "false", false }, { "yes", true }, { "no", false }, { "on", true }, { "off", false } } };
			for (const auto& [word, value] : kWords) {
				if (a_text.size() == word.size() && _strnicmp(a_text.data(), word.data(), word.size()) == 0) {
					return value;
				}
			}

			long value = 0;
			const auto [ptr, ec] = std::from_chars(a_text.data(), a_text.data() + a_text.size(), value);
			if (ec != std::errc{} || ptr == a_text.data()) {
				return std::nullopt;
			}
			return value != 0;
		}

		std::optional<float> ParseFloat(std::string_view a_text)
		{
			float value = 0.0f;
			const auto [ptr, ec] = std::from_chars(a_text.data(), a_text.data() + a_text.size(), value);
			if (ec != std::errc{} || ptr == a_text.data() || !std::isfinite(value)) {
				return std::nullopt;
			}
			return value;
		}

		std::optional<std::int32_t> ParseInt(std::string_view a_text)
		{
			std::int32_t value = 0;
			const auto [ptr, ec] = std::from_chars(a_text.data(), a_text.data() + a_text.size(), value);
			if (ec != std::errc{} || ptr == a_text.data()) {
				return std::nullopt;
			}
			return value;
		}

		std::vector<std::string> BuildOverrideFileList()
		{
			std::vector<std::string> out;

			std::error_code             ec;
			const std::filesystem::path dir{ kOverrideDir };
			if (!std::filesystem::is_directory(dir, ec)) {
				return out;
			}

			for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
				if (ec) {
					break;
				}
				if (!entry.is_regular_file(ec)) {
					continue;
				}

				auto ext = entry.path().extension().string();
				std::ranges::transform(ext, ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
				if (ext == ".ini") {
					out.push_back(entry.path().string());
				}
			}

			std::ranges::sort(out);
			return out;
		}

		struct AuditEntry
		{
			std::string key;
			std::string value;
			std::string sourceFile;
		};

		std::vector<std::string> g_overrideFiles;
		std::vector<AuditEntry>  g_audit;

		template <class T>
		std::pair<std::optional<T>, std::string> ReadMerged(const char* a_section, const char* a_key, bool a_allowPresets,
			std::optional<T> (*a_parse)(std::string_view))
		{
			std::optional<T> result;
			std::string      source;

			const auto apply = [&](const char* a_file) {
				if (const auto raw = ReadRaw(a_file, a_section, a_key); raw) {
					if (const auto parsed = a_parse(*raw); parsed) {
						result = parsed;
						source = a_file;
					} else {
						logs::warn("INI: invalid value '{}' for [{}] {} in {} - ignored", *raw, a_section, a_key, a_file);
					}
				}
			};

			apply(kMainConfigFile);
			if (a_allowPresets) {
				for (const auto& file : g_overrideFiles) {
					apply(file.c_str());
				}
			}
			apply(kUserConfigFile);
			return { result, source };
		}

		bool WriteValue(const char* a_section, const char* a_key, const std::string& a_value)
		{
			return WritePrivateProfileStringA(a_section, a_key, a_value.c_str(), kUserConfigFile) != FALSE;
		}
	}

	std::shared_ptr<const Settings> GetSettings()
	{
		return g_settings.load(std::memory_order_acquire);
	}

	void SetSettings(const Settings& a_settings)
	{
		auto copy = std::make_shared<Settings>(a_settings);
		ClampSettings(*copy);
		g_settings.store(std::move(copy), std::memory_order_release);
	}

	void ClampSettings(Settings& a_settings)
	{
		for (const auto& option : kFloatOptions) {
			auto& value = option.get(a_settings);
			value = std::isfinite(value) ? std::clamp(value, option.min, option.max) : option.min;
		}
		for (const auto& option : kIntOptions) {
			auto& value = option.get(a_settings);
			value = std::clamp(value, option.min, option.max);
		}
		a_settings.killmoveNoiseMin = (std::min)(a_settings.killmoveNoiseMin, a_settings.killmoveNoiseMax);
	}

	void LoadSettings()
	{
		Settings loaded{};
		g_overrideFiles = BuildOverrideFileList();
		g_audit.clear();

		if (const auto [value, source] = ReadMerged<bool>(kLog, "enableLog", false, ParseBool); value) {
			loaded.debugLogging = *value;
		}

		const auto isMain = [](const std::string& a_source) { return a_source.empty() || a_source == kMainConfigFile; };

		for (const auto& option : kBoolOptions) {
			if (const auto [value, source] = ReadMerged<bool>(option.section, option.key, true, ParseBool); value) {
				option.get(loaded) = *value;
				if (!isMain(source)) {
					g_audit.push_back({ option.key, *value ? "1" : "0", source });
				}
			}
		}

		for (const auto& option : kIntOptions) {
			if (const auto [value, source] = ReadMerged<std::int32_t>(option.section, option.key, true, ParseInt); value) {
				const auto clamped = std::clamp(*value, option.min, option.max);
				if (clamped != *value) {
					logs::warn("INI: {}={} is out of range, using {}", option.key, *value, clamped);
				}
				option.get(loaded) = clamped;
				if (!isMain(source)) {
					g_audit.push_back({ option.key, std::format("{}", clamped), source });
				}
			}
		}

		for (const auto& option : kFloatOptions) {
			if (const auto [value, source] = ReadMerged<float>(option.section, option.key, true, ParseFloat); value) {
				const float clamped = std::clamp(*value, option.min, option.max);
				if (clamped != *value) {
					logs::warn("INI: {}={} is out of range, using {}", option.key, *value, clamped);
				}
				option.get(loaded) = clamped;
				if (!isMain(source)) {
					g_audit.push_back({ option.key, std::format("{}", clamped), source });
				}
			}
		}

		SetSettings(loaded);
	}

	bool SaveUserSettings(const Settings& a_settings)
	{
		Settings copy = a_settings;
		bool     ok = WriteValue(kLog, "enableLog", copy.debugLogging ? "1" : "0");
		for (const auto& option : kBoolOptions) {
			ok &= WriteValue(option.section, option.key, option.get(copy) ? "1" : "0");
		}
		for (const auto& option : kIntOptions) {
			ok &= WriteValue(option.section, option.key, std::format("{}", option.get(copy)));
		}
		for (const auto& option : kFloatOptions) {
			ok &= WriteValue(option.section, option.key, std::format("{:.4f}", option.get(copy)));
		}
		WritePrivateProfileStringA(kNoise, "iSneakKillmoveNoise", nullptr, kUserConfigFile);
		return ok;
	}

	void ResetUserSettings()
	{
		std::error_code ec;
		std::filesystem::remove(kUserConfigFile, ec);
		LoadSettings();
	}

	void ApplyLogLevel()
	{
		const auto level = GetSettings()->debugLogging ? spdlog::level::debug : spdlog::level::info;
		spdlog::set_level(level);
		spdlog::flush_on(level);
	}

	void LogSettingsSummary()
	{
		std::error_code ec;
		if (std::filesystem::exists(kUserConfigFile, ec)) {
			logs::info("Menu settings: loaded (StealthFixes_User.ini)");
		}
		if (!g_overrideFiles.empty()) {
			logs::info("Preset INIs loaded: {}", g_overrideFiles.size());
			for (const auto& file : g_overrideFiles) {
				logs::debug("  {}", file);
			}
		}
		for (const auto& e : g_audit) {
			logs::debug("  {} = {} (from {})", e.key, e.value, e.sourceFile);
		}

		const auto s = GetSettings();
		if (s->touchFix) {
			logs::info("  touching someone: only that person notices, after {:.2f} s", s->touchGrace);
		} else {
			logs::info("  touching someone: loud for everyone nearby (base game)");
		}
		logs::info("  arrows and noise spots: {}, {}", s->eventRealLight ? "seen in the real light there" : "fixed light (base game)",
			s->eventSightFix ? "real view cone and blindness" : "always in view (base game)");
		if (s->corpseLight) {
			logs::info("  bodies: seen in the real light where they lie (shadow keeps {:.2f} of the sight, {:.2f} when alerted; fully lit at {:.0f})",
				s->shadowUnaware, s->shadowAlerted, s->exposureSpan);
		} else {
			logs::info("  bodies: as in the base game");
		}
		if (s->hitGuess) {
			logs::info("  hit from hiding: they guess where you are, spread up to {:.0f}% of the distance in the dark (half in full light, more with high Sneak)",
				s->hitGuessError * 100.0f);
		} else {
			logs::info("  hit from hiding: they know your exact spot (base game)");
		}
		logs::info("  animals as crime witnesses: {}", s->noAnimalWitnesses ? "no" : "yes (base game)");
		if (s->impactNoise) {
			logs::info("  arrow impacts: soft {}, wood {}, stone {}, metal {}, water {}", s->impactSoft, s->impactWood, s->impactStone,
				s->impactMetal, s->impactWater);
		} else {
			logs::info("  arrow impacts: the same on every surface (base game)");
		}
		logs::info("  action noise: {}", s->actionNoiseFix ? "the full multiplier counts" : "multiplier cut to a whole number (base game)");
		logs::info("  NPC perception against arrows and noise spots: {}", s->eventPerception ? "counts" : "dropped (base game)");
		logs::info("  Night Eye bonus: {}", s->nightEyeBonusFix ? "the full bonus counts" : "cut to a whole number (base game)");
		logs::info("  passing projectiles: {}", s->projectileSound ? "as loud as their own sound level" : "always the same (base game)");
		logs::info("  a quieter noise {} a louder one that still counts", s->louderStays ? "does not erase" : "erases (base game)");
		if (s->quietSneakKillmove) {
			logs::info("  sneak killmove: each blow {} - {} loud, the animation counts as standing still", s->killmoveNoiseMin, s->killmoveNoiseMax);
		} else {
			logs::info("  sneak killmove: its blows are heard by everyone nearby, its animation counts as movement (base game)");
		}
		logs::info("  summons: {}", s->summonNoise ? "as loud as the spell, Quiet Casting counts" : "always loud (base game)");
		logs::info("  sneak attacks: searching group {}, outdated detection {}, arrows {}, group busy with others {}",
			s->sneakSearchingGroup ? "checks sight" : "free pass (base game)", s->sneakMissingDetection ? "counts as it was" : "counts as unseen (base game)",
			s->sneakBowAtRelease ? "as you shot" : "as they hit (base game)", s->sneakWhileGroupBusy ? "allowed when unseen" : "never (base game)");
		logs::info("  locks and owned objects: {}; commanded followers: {}", s->trespassWitnesses ? "owner and its crime faction only" : "anyone who notices you (base game)",
			s->followerTrespass ? "need a witness" : "always a crime (base game)");
		logs::info("  pickpocket chance shown: {}; guards searching in combat report {}", s->pickpocketChanceShown ? "the rolled one" : "as in the base game",
			s->crimeSearchNeedsSight ? "only what they saw" : "every crime (base game)");
		logs::info("  combat: eye {}, far members {}, search {}, Shadow Warrior {}, line after a kill: {}, far checks {}",
			s->combatEye ? "shows what they know" : "shows stealth points (base game)", s->lowProcessDetection ? "see normally" : "see through walls (base game)",
			s->searchLatestLocation ? "also where they saw you last" : "only where the attack came from (base game)",
			s->shadowWarriorGrace ? "with stealth points" : "as in the base game", s->noGiveUpLineAfterKill ? "end of combat" : "must have been nothing (base game)",
			s->detectionPriority ? "less often" : "as in the base game");
		logs::info("  detection clock: {}", s->preciseClock ? "precise" : "float (base game)");
		logs::info("  NPCs' arrows and noises: {}; sprinting and running backwards: {}",
			s->eventSightFix && s->eventSightNPCs ? "real view cone and blindness" : "always in view (base game)",
			s->sprintCounts ? "count as running" : "as in the base game");
		logs::info("  your torch: {}, {}", s->hiddenLightsOff ? "counts once" : "counts twice (base game)", s->steadyTorch ? "steady" : "flickers (base game)");
		logs::info("  noticed but not detected: {}; comments on your bow: {}", s->reactionsNeedSight ? "they search" : "they react as if they saw you (base game)",
			s->bowCommentNeedsSight ? "need a sighting" : "also unseen (base game)");
		logs::info("  unconscious NPCs: {}", s->unconsciousNoticeNothing ? "notice nothing" : "still see and hear you (base game)");
		if (s->wakeOnHit) {
			logs::info("  unconscious NPCs wake on a hit: at least {:.0f}%, sure at {:.0f}% of their health", s->wakeChanceMin * 100.0f,
				s->wakeDamage * 100.0f);
		} else {
			logs::info("  unconscious NPCs wake on a hit: off (base game: they stay down)");
		}
		if (s->alliesWake) {
			logs::info("  allies wake unconscious NPCs they see within {:.0f} m ({:.1f} s at their side)", s->allyRange, s->allyWakeTime);
		} else {
			logs::info("  allies wake unconscious NPCs: off (base game: they walk past)");
		}
		logs::info("  an open murder is remembered after an assault is cleared: {}; alarmed guards report murders: {}; alarmed NPCs as witnesses: {}",
			s->openMurderKeepsEnemy ? "yes" : "no (base game)", s->guardAlarmNearby ? "only near the victim" : "anywhere (base game)",
			s->alarmWitnessesSee ? "only when they see you" : "always (base game)");
		logs::info("  hitting someone else's animal: {}", s->ownedAnimalWitnesses ? "needs a witness" : "always a crime (base game)");
		logs::info("  frenzy: {}, {}; sneaking spells: {}", s->frenzyUnseen ? "turns on you only when someone notices you" : "turns on you (base game)",
			s->frenzyKeepsEnemies ? "enemies who still see you stay enemies" : "ends every fight (base game)", s->allSneakingSpells ? "all" : "one by chance (base game)");
	}
}
