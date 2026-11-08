#pragma once

namespace StealthFixes::Hooks
{
	inline constexpr std::int32_t kNoDetectionState = -1000;

	inline bool CheckedLayout()
	{
		if (REL::Module::IsVR()) {
			return true;
		}
		const auto version = REL::Module::get().version();
		return version == REL::Version(1, 5, 97, 0) || version == REL::Version(1, 6, 1170, 0) || version == REL::Version(1, 7, 99, 0) ||
		       version == REL::Version(1, 7, 104, 0);
	}

	inline std::uintptr_t ImageEnd()
	{
		const auto    base = REL::Module::get().base();
		std::int32_t  ntOffset;
		std::uint32_t size;
		std::memcpy(&ntOffset, reinterpret_cast<const void*>(base + 0x3C), sizeof(ntOffset));
		std::memcpy(&size, reinterpret_cast<const void*>(base + ntOffset + 0x50), sizeof(size));
		return base + size;
	}

	inline std::uintptr_t CallAt(REL::CallsiteID a_owner, REL::VariantOffset a_offset, REL::CallsiteID a_target, std::size_t a_index = 0,
		std::size_t a_count = 1)
	{
		const auto owner = a_owner.rva();
		const auto target = a_target.rva();
		if (!owner || !target) {
			return 0;
		}
		const auto base = REL::Module::get().base();
		if (CheckedLayout()) {
			const auto  site = base + *owner + a_offset.offset();
			const auto* bytes = reinterpret_cast<const std::uint8_t*>(site);
			if (bytes[0] != 0xE8) {
				return 0;
			}
			std::int32_t rel;
			std::memcpy(&rel, bytes + 1, sizeof(rel));
			const auto destination = site + 5 + static_cast<std::intptr_t>(rel);
			return destination == base + *target || destination < base || destination >= ImageEnd() ? site : 0;
		}
		const auto calls = REL::resolve_callsites(a_owner, a_target);
		return calls.addresses.size() == a_count ? calls.addresses[a_index] : 0;
	}

	inline std::uintptr_t Site(REL::CallsiteID a_owner, REL::CallsiteID a_target)
	{
		const auto site = REL::try_resolve_callsite(a_owner, REL::AUTO_CALLSITE(a_target));
		return site ? *site.address : 0;
	}

	inline bool InSharedCrimeFaction(RE::Actor* a_actor, RE::TESFaction* a_faction)
	{
		using func_t = bool(RE::Actor*, RE::TESFaction*);
		static const REL::Relocation<func_t*> func{ REL::VariantID(36674, 37682, 0x6009B0) };
		return a_actor && a_faction && func(a_actor, a_faction);
	}

	inline float* DetectionClock()
	{
		static const REL::Relocation<float*> clock{ RELOCATION_ID(517597, 404125) };
		return clock.get();
	}

	inline RE::NiPointer<RE::Actor> ActorFrom(RE::RefHandle a_handle)
	{
		RE::NiPointer<RE::TESObjectREFR> ref;
		RE::LookupReferenceByHandle(a_handle, ref);
		return ref && ref->As<RE::Actor>() ? RE::NiPointer<RE::Actor>{ ref->As<RE::Actor>() } : nullptr;
	}

	inline bool JumpAt(std::uintptr_t a_address, std::span<const std::uint8_t, 5> a_expected, const void* a_function)
	{
		if (!a_address || std::memcmp(reinterpret_cast<const void*>(a_address), a_expected.data(), a_expected.size()) != 0) {
			return false;
		}
		SKSE::GetTrampoline().write_branch<5>(a_address, reinterpret_cast<std::uintptr_t>(a_function));
		return true;
	}
}
