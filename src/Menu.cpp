#include "Menu.h"

#include "Settings.h"

#include "SKSEMenuFramework.h"

namespace StealthFixes::Menu
{
	namespace
	{
		Settings edited;
		bool     dirty{ false };

		void Apply()
		{
			const bool debugChanged = edited.debugLogging != GetSettings()->debugLogging;
			ClampSettings(edited);
			SetSettings(edited);
			dirty = true;
			if (debugChanged) {
				ApplyLogLevel();
			}
		}

		void Tooltip(const char* a_text)
		{
			if (a_text && a_text[0] != '\0' && ImGuiMCP::IsItemHovered()) {
				ImGuiMCP::SetTooltip("%s", a_text);
			}
		}

		bool Check(const char* a_label, bool& a_value, const char* a_tooltip)
		{
			const bool changed = ImGuiMCP::Checkbox(a_label, &a_value);
			Tooltip(a_tooltip);
			return changed;
		}

		bool Slider(const char* a_label, float& a_value, float a_min, float a_max, const char* a_format, const char* a_tooltip)
		{
			const bool changed = ImGuiMCP::SliderFloat(a_label, &a_value, a_min, a_max, a_format);
			Tooltip(a_tooltip);
			return changed;
		}

		bool SliderInt(const char* a_label, std::int32_t& a_value, const char* a_tooltip)
		{
			int        value = a_value;
			const bool changed = ImGuiMCP::SliderInt(a_label, &value, 0, 200, "%d");
			Tooltip(a_tooltip);
			a_value = value;
			return changed;
		}

		void __stdcall RenderFixes()
		{
			bool changed = false;
			ImGuiMCP::TextWrapped("%s", "Fixes for base game stealth detection bugs, sorted like on the mod page. Each one can be turned off on its own; changes apply at once.");

			if (ImGuiMCP::CollapsingHeader("Noise")) {
				changed |= Check("Only the person touched notices", edited.touchFix,
					"In the base game, touching another person counts as a loud noise for 5 seconds that every NPC nearby hears.\n"
					"With this on, only the person you touch notices it. A touch during your own attack is part of the blow.");
				ImGuiMCP::BeginDisabled(!edited.touchFix);
				changed |= Slider("Brief touch unnoticed", edited.touchGrace, 0.0f, 2.0f, "%.2f s",
					"A touch shorter than this goes unnoticed (brushing past someone).");
				ImGuiMCP::EndDisabled();
				changed |= Check("Louder noise stays", edited.louderStays,
					"In the base game the last noise always replaces the one before: a quiet dagger stab right after a fireball\n"
					"erases the fireball's noise. With this on, a quieter noise close by (within about 10 m) does not erase a louder one\n"
					"that still counts. A new noise further away counts, as in the base game.");
				changed |= Check("Quiet sneak killmoves", edited.quietSneakKillmove,
					"In the base game every blow of a killmove is the weapon's full noise that every NPC nearby hears for 5 seconds\n"
					"(dagger 10, sword 50, two-handed 100). The movement of the killmove animation also counts as walking or running.\n"
					"With this on, each blow of a sneak killmove stays between the two limits below and the animation counts as standing still.");
				ImGuiMCP::BeginDisabled(!edited.quietSneakKillmove);
				{
					int low = edited.killmoveNoiseMin;
					int high = edited.killmoveNoiseMax;
					if (ImGuiMCP::SliderInt("Quietest blow", &low, 0, 100, "%d")) {
						edited.killmoveNoiseMin = low;
						edited.killmoveNoiseMax = (std::max)(edited.killmoveNoiseMax, low);
						changed = true;
					}
					Tooltip("A blow is at least this loud: a dagger (10) is raised to it. 20 = NPCs close by can hear it.");
					if (ImGuiMCP::SliderInt("Loudest blow", &high, 0, 100, "%d")) {
						edited.killmoveNoiseMax = high;
						edited.killmoveNoiseMin = (std::min)(edited.killmoveNoiseMin, high);
						changed = true;
					}
					Tooltip("A blow is at most this loud: louder weapons are lowered to it. 50 = a sword stays as it is, a two-handed weapon (100) is halved.");
				}
				ImGuiMCP::EndDisabled();
				changed |= Check("Sprinting and running backwards", edited.sprintCounts,
					"Running is louder than walking. In the base game sprinting counts as running only at exactly full speed.\n"
					"Running backwards never does. Blocking while you move counts as running. With this on, sprinting always\n"
					"counts, running backwards counts too, blocking does not.");
				changed |= Check("Full action noise multiplier", edited.actionNoiseFix,
					"The noise of attacks, shots and spells is multiplied by the game setting fSneakActionMult. The base game cuts\n"
					"it to a whole number first: 0.5 becomes 0 (attacks and spells make no noise at all), 1.5 becomes 1.\n"
					"With this on the full value counts. Nothing changes while the setting is a whole number (the base game uses 2).");
				changed |= Check("Passing projectiles use their sound level", edited.projectileSound,
					"NPCs also notice your arrows flying past. The base game treats every projectile as Normal there, even\n"
					"silent ones such as bound arrows or Nightingale arrows. With this on, each uses its own sound level.");
				changed |= Check("Summons use the spell's sound", edited.summonNoise,
					"In the base game every summon makes a loud noise where the creature appears, even with Quiet Casting.\n"
					"With this on, it is as loud as the spell and Quiet Casting silences it.");
				ImGuiMCP::SeparatorText("Arrow impacts");
				changed |= Check("Impact noise by surface", edited.impactNoise,
					"How loud your arrows and bolts are where they hit. NPCs nearby may come to look at the spot.\n"
					"In the base game every surface is the same (50, \"Normal\"): grass as loud as a steel door.");
				ImGuiMCP::BeginDisabled(!edited.impactNoise);
				changed |= SliderInt("Soft ground", edited.impactSoft, "Earth, grass, snow, sand, mud, flesh, cloth. 0 = silent, 50 = the game's Normal.");
				changed |= SliderInt("Wood", edited.impactWood, "Wooden walls, floors, barrels, furniture.");
				changed |= SliderInt("Stone", edited.impactStone, "Stone, gravel, glass, ice, bone.");
				changed |= SliderInt("Metal", edited.impactMetal, "Metal, armor, weapons, chains. 100 = loud.");
				changed |= SliderInt("Water", edited.impactWater, "A splash in water or a puddle.");
				ImGuiMCP::EndDisabled();
			}
			if (ImGuiMCP::CollapsingHeader("Sight and Light")) {
				changed |= Check("Seen in the real light", edited.eventRealLight,
					"In the base game, NPCs judge your flying arrows and the spot of your noises with a fixed light, brighter\n"
					"inside than any real place. With this on, they see them only as well as the real light at that spot allows:\n"
					"an arrow flying through a dark corner goes unseen.");
				changed |= Check("View cone and blindness", edited.eventSightFix,
					"In the base game, an NPC sees your arrows and the spot of your noises even behind its back (its view cone is\n"
					"checked in the wrong unit there), and blind NPCs such as Falmer see them as if they had full sight.\n"
					"With this on, only NPCs facing the spot see it, and blindness counts. Hearing them is unchanged.");
				ImGuiMCP::BeginDisabled(!edited.eventSightFix);
				changed |= Check("Also for NPCs' arrows and noises", edited.eventSightNPCs,
					"The same view cone and blindness when NPCs notice the arrows and noises of other NPCs, for example your\n"
					"follower's (the game checks those only while someone fights).");
				ImGuiMCP::EndDisabled();
				changed |= Check("NPC perception against arrows and noises", edited.eventPerception,
					"When an NPC checks your arrows and the spot of your noises, the base game works out its perception (skill, perks,\n"
					"alertness) and then throws it away. With this on it counts, like in the NPC's check of you.\n"
					"Alert and skilled NPCs notice arrows and noises more easily with it.");
				changed |= Check("Full Night Eye bonus", edited.nightEyeBonusFix,
					"An NPC with Night Eye sees more light. The base game cuts the game setting fDetectionNightEyeBonus to a whole\n"
					"number first: 1.5 becomes 1, a value below 1 makes such NPCs see nothing. No change with the default 3.0.");
				changed |= Check("Your torch counts once", edited.hiddenLightsOff,
					"Your torch exists twice, once for each camera view; the other one is only hidden. In the base game both light\n"
					"you up for the NPCs. With this on, hidden lights do not count (they light nothing on screen either).");
				changed |= Check("Steady torch", edited.steadyTorch,
					"A torch flickers. In the base game your light level for the NPCs flickers with it, so the stealth eye\n"
					"twitches while you stand still. With this on, your torch counts with its mean brightness.");
				ImGuiMCP::SeparatorText("Bodies");
				changed |= Check("Found in the real light", edited.corpseLight,
					"NPCs find a body only as well as the light where it lies allows. Hide one in a dark corner and it is found\n"
					"later - someone has to come close or bring a light. In the base game a body in the dark is found as easily as\n"
					"one under a torch.");
				ImGuiMCP::BeginDisabled(!edited.corpseLight);
				ImGuiMCP::TextWrapped("%s", "Share of the sight left in full shadow:");
				changed |= Slider("Unaware NPC", edited.shadowUnaware, 0.0f, 1.0f, "%.2f",
					"0.30 = a body in deep shadow is only a third as visible as in light; 1.0 = shadow does not help.");
				changed |= Slider("Alerted NPC", edited.shadowAlerted, 0.0f, 1.0f, "%.2f", "Alerted, or in combat: looks more carefully.");
				ImGuiMCP::EndDisabled();
				changed |= Slider("Fully lit at", edited.exposureSpan, 10.0f, 200.0f, "%.0f",
					"Light above the surroundings' base light that counts as fully lit (bodies and hits from hiding).\nLower = light matters sooner.");
			}
			if (ImGuiMCP::CollapsingHeader("Reactions")) {
				changed |= Check("Noticing is not detecting", edited.reactionsNeedSight,
					"In the base game an NPC who noticed something but has not detected you (the eye half open) reacts as if it\n"
					"saw you: it may try to arrest you, warn you off its property, flee or warn you to keep away. With this on, it\n"
					"searches and joins a fight. Everything else needs a detection.");
				changed |= Check("Comments on your bow need a sighting", edited.bowCommentNeedsSight,
					"In the base game an NPC comments on your drawn bow aimed at it even when it does not see you, unless it is\n"
					"talking and greeting you at the same time. With this on, the rule of every other comment counts: no comment\n"
					"when it talks, greets you or does not see you.");
				changed |= Check("Unconscious NPCs notice nothing", edited.unconsciousNoticeNothing,
					"In the base game an unconscious NPC leaves combat and is no crime witness, but it still sees and hears you:\n"
					"it can open the stealth eye and spoil a sneak attack. With this on, it notices nothing until it wakes up.");
			}
			if (ImGuiMCP::CollapsingHeader("Unconscious NPCs")) {
				if (REL::Module::IsVR()) {
					ImGuiMCP::TextWrapped("Not available in VR.");
				}
				changed |= Check("A hit can wake them", edited.wakeOnHit,
					"The base game leaves an unconscious NPC lying there whatever happens to it. With this on, a hit can wake it;\n"
					"the harder the hit, the likelier. Once awake it is on guard for a while. Knockouts from Sneak Tools end the same way.");
				ImGuiMCP::BeginDisabled(!edited.wakeOnHit);
				changed |= Slider("Chance of a light hit", edited.wakeChanceMin, 0.0f, 1.0f, "%.2f",
					"Chance that even the lightest hit wakes it (0.2 = one in five).");
				changed |= Slider("Sure wake-up at damage", edited.wakeDamage, 0.05f, 1.0f, "%.2f",
					"A hit that takes this share of its health always wakes it; weaker hits are less likely to.");
				ImGuiMCP::EndDisabled();
				changed |= Check("Allies wake them", edited.alliesWake,
					"An awake NPC of the same group (bandits, guards, a town) that sees an unconscious one walks over and wakes it.\n"
					"Both are on guard for a while afterwards. NPCs in combat and your followers do not.");
				ImGuiMCP::BeginDisabled(!edited.alliesWake);
				changed |= Slider("Noticed within##ally", edited.allyRange, 2.0f, 50.0f, "%.0f m",
					"How far away an ally notices an unconscious NPC (it also has to see it).");
				changed |= Slider("Time to wake it##ally", edited.allyWakeTime, 0.5f, 15.0f, "%.1f s",
					"Seconds the ally needs at its side to wake it. Enough time to knock the helper out as well.");
				ImGuiMCP::EndDisabled();
			}
			if (ImGuiMCP::CollapsingHeader("Sneak Attacks")) {
				changed |= Check("Searching NPCs who see you", edited.sneakSearchingGroup,
					"In the base game every melee hit on an NPC whose group is searching for you is a sneak attack, even when\n"
					"that NPC sees you right now. With this on, an NPC who sees you gets no sneak attack.");
				changed |= Check("Outdated detection", edited.sneakMissingDetection,
					"When the game's detection result for an NPC is a little outdated, the base game treats the NPC as not seeing\n"
					"you and gives a sneak attack. With this on, the last result counts.");
				changed |= Check("Arrows count as you shot them", edited.sneakBowAtRelease,
					"In the base game an arrow or bolt checks whether you sneak when it hits, not when you shoot: shoot standing,\n"
					"crouch before it lands, and it is a sneak attack. With this on, your state at the shot counts.");
				changed |= Check("Sneak attacks while their group fights others (option)", edited.sneakWhileGroupBusy,
					"In the base game there are no sneak attacks on a group that is still fighting, for example your follower,\n"
					"even on NPCs who do not see you. With this on, an NPC who does not see you can be sneak attacked.\n"
					"Off by default: this changes the balance.");
			}
			if (ImGuiMCP::CollapsingHeader("Combat and Search")) {
				changed |= Check("Hit from hiding: they only know the direction", edited.hitGuess,
					"In the base game, an NPC you hit from afar - even from deep shadow - learns your exact spot and its group\n"
					"searches right there. With this on, it only knows roughly where the hit came from: the guess is off in distance and\n"
					"up to about 18 degrees in direction.\n"
					"The guess is worse in the dark and with a high Sneak skill. Up close, and while its group sees you, nothing changes.");
				ImGuiMCP::BeginDisabled(!edited.hitGuess);
				changed |= Slider("Guess spread", edited.hitGuessError, 0.0f, 1.0f, "%.2f",
					"How far off the guess is, as a share of the distance, in full darkness (0.35 = at 30 m about 10 m).\nIn full light half of that; with a high Sneak skill up to half more.");
				ImGuiMCP::EndDisabled();
				changed |= Check("Search where they saw you last", edited.searchLatestLocation,
					"In the base game, for 10 seconds after an attack, a group only searches where the attack came from, even\n"
					"after it saw you somewhere else. With this on, it also searches where it saw you last.");
				changed |= Check("Stealth eye shows what they know", edited.combatEye,
					"In combat the base game shows stealth points: the eye stays fully open (\"detected\") for up to 50 seconds after the\n"
					"enemies have lost you. With this on, it is fully open only while they know where you are. While they search for you\n"
					"it is at most partly open and closes as their search runs out.");
				changed |= Check("Far group members see normally", edited.lowProcessDetection,
					"In the base game group members that the game does not fully simulate see everything near them through walls,\n"
					"every second. With this on, they no longer do.");
				changed |= Check("Fitting line after a kill", edited.noGiveUpLineAfterKill,
					"In the base game a \"must have been nothing\" line can follow after the group's only target died.\n"
					"With this on, they say an end-of-combat line instead.");
				changed |= Check("Far NPCs are checked less often", edited.detectionPriority,
					"The base game never uses its lowest check rate for far away NPCs because of a wrong order of two checks.\n"
					"With this on, far NPCs are checked a bit less often, as intended. Only for NPCs fighting each other.");
				changed |= Check("Precise detection clock", edited.preciseClock,
					"The detection uses its own clock. In very long sessions with a high frame rate it slows down and can stop\n"
					"(from about 36 hours at 144 fps); detection then freezes. With this on, it keeps counting precisely.");
				changed |= Check("Every sneaking spell", edited.allSneakingSpells,
					"Crouching casts the spells of perks such as Shadow Warrior and the Nightingale's Shadowcloak. In the base game\n"
					"only one of them is cast, by chance. With this on, all of them are. With Scrambled Bugs installed, it does this instead.");
				changed |= Check("Shadow Warrior gives stealth points (option)", edited.shadowWarriorGrace,
					"Shadow Warrior makes you invisible for a second when you crouch in combat, but in the base game the enemies\n"
					"see you again right after, because your stealth points stay empty. With this on, it also gives back half\n"
					"of them. Off by default: this changes the balance.");
				changed |= Check("Frenzy from hiding", edited.frenzyUnseen,
					"In the base game a frenzied NPC always turns on you, even when nobody noticed you, so Fury from hiding gives\n"
					"you away. With this on, it attacks the people around it while nobody notices you. When someone does, it is\n"
					"the base game (it turns on you, a witness may report it).");
				changed |= Check("Enemies stay enemies after a frenzy", edited.frenzyKeepsEnemies,
					"When a frenzy ends, the base game ends every fight of the NPC, also the one it had with you before.\n"
					"With this on, an NPC that fought you before fights on when the frenzy ends, if it still sees you.");
			}
			if (ImGuiMCP::CollapsingHeader("Crime")) {
				changed |= Check("Animals are no witnesses", edited.noAnimalWitnesses,
					"In the base game an animal that sees a theft or a murder - a deer, a dog, a chicken - is put on the crime's\n"
					"witness list like a person. With this on, animals are left out. They still see, hear and fight you as usual.");
				changed |= Check("Locks and owned objects need a real witness", edited.trespassWitnesses,
					"In the base game picking an owned lock or using an owned object is a crime as soon as anyone notices you:\n"
					"an animal, an enemy, a stranger far away. With this on, only the owner and NPCs of its crime faction count,\n"
					"like for theft.");
				changed |= Check("Commanded followers need a real witness", edited.followerTrespass,
					"In the base game a follower you command to pick an owned lock always gives you a bounty, with no witness at all.\n"
					"With this on, only when a real witness notices it. A witness that never looked at the follower counts if it sees you.");
				changed |= Check("Searching guards report what they saw", edited.crimeSearchNeedsSight,
					"In the base game a guard who already searches you in combat reports any crime, even one it did not see.\n"
					"With this on, it needs to see you.");
				changed |= Check("Pickpocket chance as rolled", edited.pickpocketChanceShown,
					"The pickpocket chance shown in the menu differs from the rolled one in the base game: placing items shows a\n"
					"value penalty the roll does not use, and a knocked down NPC shows a normal chance but always succeeds.");
				changed |= Check("Someone else's animal", edited.ownedAnimalWitnesses,
					"In the base game hitting someone else's horse, dog or chicken is a crime even when nobody sees it, as soon as\n"
					"the owner is nearby. With this on, only a witness who may report it counts, like for theft.");
				changed |= Check("An open murder is not forgotten", edited.openMurderKeepsEnemy,
					"A reported murder makes the guards of that hold treat you as an enemy when they recognize you. When an assault\n"
					"in the same hold is cleared, the base game forgets the murder too. With this on, the guards keep treating you\n"
					"as a murderer while the murder is open. Nothing happens until one of them recognizes you.");
				changed |= Check("Alarmed guards report murders only nearby", edited.guardAlarmNearby,
					"In the base game a murder without any witness is reported as soon as any guard anywhere has an alarm, for\n"
					"example one still chasing you for a theft elsewhere. With this on, only a living alarmed guard of the\n"
					"victim's crime faction near the victim counts.");
				changed |= Check("Alarmed NPCs are witnesses only when they see you (option)", edited.alarmWitnessesSee,
					"When a witness raises the alarm, the base game puts everyone it alarms on the crime's witness list, also\n"
					"people who never saw you. Then killing the witnesses who saw you does not clear the crime. With this on,\n"
					"alarmed NPCs count as witnesses only when they see you. Off by default: this changes the balance.");
			}

			ImGuiMCP::SeparatorText("Log");
			changed |= Check("Debug logging", edited.debugLogging,
				"Writes more detail to StealthFixes.log: bodies seen or missed in the dark, hits from hiding, animals left out of crimes.");

			if (changed) {
				Apply();
			}

			ImGuiMCP::SeparatorText("Settings");
			ImGuiMCP::TextWrapped("%s", "Menu changes are saved to StealthFixes_User.ini when the menu closes. Load order: StealthFixes.ini, then the presets in StealthFixesCustomINI, then the menu settings.");
			if (ImGuiMCP::Button("Reset to defaults")) {
				const bool debug = edited.debugLogging;
				edited = Settings{};
				edited.debugLogging = debug;
				Apply();
			}
			Tooltip("Sets everything on this page back to the mod's defaults (debug logging stays as it is). Saved when the menu closes.");
			ImGuiMCP::SameLine();
			if (ImGuiMCP::Button("Reset to INI values")) {
				ResetUserSettings();
				ApplyLogLevel();
				edited = *GetSettings();
				dirty = false;
				logs::info("Menu settings: reset to the INI values");
			}
			Tooltip("Removes the menu changes and loads StealthFixes.ini and the presets again.");
		}

		void __stdcall OnMenuEvent(SKSEMenuFramework::Model::EventType a_type)
		{
			if (a_type == SKSEMenuFramework::Model::kOpenMenu) {
				edited = *GetSettings();
				dirty = false;
			} else if (a_type == SKSEMenuFramework::Model::kCloseMenu && dirty) {
				dirty = false;
				if (SaveUserSettings(edited)) {
					logs::info("Menu settings: saved");
				} else {
					logs::warn("Menu settings could not be saved; they stay active for this session.");
				}
			}
		}
	}

	bool Register()
	{
		if (!SKSEMenuFramework::IsInstalled()) {
			logs::info("Settings menu: not available - SKSE Menu Framework is not installed. The INI settings are used.");
			return false;
		}

		edited = *GetSettings();
		SKSEMenuFramework::SetSection("Stealth Fixes");
		SKSEMenuFramework::AddSectionItem("Fixes", RenderFixes);
		SKSEMenuFramework::AddEvent(OnMenuEvent, 0.0f);
		logs::info("Settings menu: active");
		return true;
	}
}
