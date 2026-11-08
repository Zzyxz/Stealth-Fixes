#pragma once

namespace StealthFixes::GameSettings
{
	void Cache();

	[[nodiscard]] float        Float(std::string_view a_name, float a_fallback = 0.0f);
	[[nodiscard]] std::int32_t Int(std::string_view a_name, std::int32_t a_fallback = 0);

	[[nodiscard]] std::vector<std::string> Describe();
}
