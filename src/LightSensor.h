#pragma once

namespace StealthFixes::LightSensor
{
	[[nodiscard]] float MeasureAt(const RE::NiPoint3& a_point, RE::Actor* a_ignore = nullptr);

	[[nodiscard]] float BaseLight();
}
