#pragma once

namespace StealthFixes
{
	struct Settings
	{
		bool debugLogging{ false };

		bool  touchFix{ true };
		float touchGrace{ 0.25f };

		bool eventRealLight{ true };
		bool eventSightFix{ true };
		bool projectileSound{ true };
		bool eventSightNPCs{ true };

		bool         louderStays{ true };
		bool         quietSneakKillmove{ true };
		std::int32_t killmoveNoiseMin{ 20 };
		std::int32_t killmoveNoiseMax{ 50 };
		bool         sprintCounts{ true };

		bool summonNoise{ true };
		bool frenzyUnseen{ true };
		bool frenzyKeepsEnemies{ true };

		bool  corpseLight{ true };
		float shadowUnaware{ 0.30f };
		float shadowAlerted{ 0.40f };

		float exposureSpan{ 60.0f };
		bool  hiddenLightsOff{ true };
		bool  steadyTorch{ true };

		bool  hitGuess{ true };
		float hitGuessError{ 0.35f };

		bool noAnimalWitnesses{ true };
		bool openMurderKeepsEnemy{ true };
		bool guardAlarmNearby{ true };
		bool alarmWitnessesSee{ false };

		bool reactionsNeedSight{ true };
		bool bowCommentNeedsSight{ true };
		bool unconsciousNoticeNothing{ true };

		bool  wakeOnHit{ true };
		float wakeChanceMin{ 0.2f };
		float wakeDamage{ 0.25f };
		bool  alliesWake{ true };
		float allyRange{ 20.0f };
		float allyWakeTime{ 3.0f };

		bool actionNoiseFix{ true };
		bool eventPerception{ true };
		bool nightEyeBonusFix{ true };

		bool sneakSearchingGroup{ true };
		bool sneakMissingDetection{ true };
		bool sneakBowAtRelease{ true };
		bool sneakWhileGroupBusy{ false };

		bool trespassWitnesses{ true };
		bool followerTrespass{ true };
		bool pickpocketChanceShown{ true };
		bool crimeSearchNeedsSight{ true };
		bool ownedAnimalWitnesses{ true };

		bool combatEye{ true };
		bool lowProcessDetection{ true };
		bool searchLatestLocation{ true };
		bool shadowWarriorGrace{ false };
		bool noGiveUpLineAfterKill{ true };
		bool detectionPriority{ true };
		bool allSneakingSpells{ true };

		bool preciseClock{ true };

		bool         impactNoise{ true };
		std::int32_t impactSoft{ 15 };
		std::int32_t impactWood{ 40 };
		std::int32_t impactStone{ 50 };
		std::int32_t impactMetal{ 80 };
		std::int32_t impactWater{ 60 };
	};

	[[nodiscard]] std::shared_ptr<const Settings> GetSettings();
	void                                          SetSettings(const Settings& a_settings);

	void LoadSettings();

	void ClampSettings(Settings& a_settings);

	bool SaveUserSettings(const Settings& a_settings);

	void ResetUserSettings();

	void LogSettingsSummary();

	void ApplyLogLevel();
}
