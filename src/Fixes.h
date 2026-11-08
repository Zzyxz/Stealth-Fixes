#pragma once

namespace StealthFixes::Fixes
{
	void Install();

	void OnDataLoaded();

	void ResetMemory();

	void* SetDetectionFilter(void* a_filter);

	bool ActionNoiseFixed();

	void InstallReactions();
	void InstallCrimes();
	void InstallLights();
	void InstallMagic();

	void ResetMagic();

	[[nodiscard]] float CorrectLampLight(RE::BSLight* a_light, float a_luminance);
}
