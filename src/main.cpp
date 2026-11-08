#include "Fixes.h"
#include "GameSettings.h"
#include "Menu.h"
#include "Settings.h"
#include "Unconscious.h"
#include "Version.h"

namespace
{
	constexpr auto PluginName = "StealthFixes"sv;
	constexpr auto DisplayName = "Stealth Fixes"sv;
	constexpr auto Author = "Zzyxzz"sv;

	void InitializeLog()
	{
		auto directory = logs::log_directory();
		if (!directory) {
			return;
		}

		*directory /= "StealthFixes.log";
		auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(directory->string(), true);
		auto log = std::make_shared<spdlog::logger>("global", std::move(sink));
		log->set_level(spdlog::level::info);
		log->flush_on(spdlog::level::info);
		spdlog::set_default_logger(std::move(log));
		spdlog::set_pattern("[%H:%M:%S] [%l] %v");
	}

	void OnMessage(SKSE::MessagingInterface::Message* a_msg)
	{
		if (!a_msg) {
			return;
		}

		if (a_msg->type == SKSE::MessagingInterface::kPostPostLoad) {
			StealthFixes::Menu::Register();
		} else if (a_msg->type == SKSE::MessagingInterface::kPreLoadGame || a_msg->type == SKSE::MessagingInterface::kNewGame) {
			StealthFixes::Fixes::ResetMemory();
			StealthFixes::Unconscious::Reset();
		} else if (a_msg->type == SKSE::MessagingInterface::kSaveGame) {
			StealthFixes::Unconscious::BeforeSave();
		} else if (a_msg->type == SKSE::MessagingInterface::kDataLoaded) {
			StealthFixes::GameSettings::Cache();
			StealthFixes::Fixes::OnDataLoaded();
			StealthFixes::Unconscious::Install();
		}
	}
}

extern "C" __declspec(dllexport) constinit auto SKSEPlugin_Version = []() noexcept {
	SKSE::PluginVersionData version{};
	version.PluginName(PluginName.data());
	version.PluginVersion(REL::Version{ StealthFixes::Version::Major, StealthFixes::Version::Minor, StealthFixes::Version::Patch, 0 });
	version.AuthorName(Author.data());
	version.UsesAddressLibrary();
	version.UsesAddressLibraryV5();
	version.UsesNoStructs();
	return version;
}();

extern "C" __declspec(dllexport) bool SKSEAPI SKSEPlugin_Query(const SKSE::QueryInterface* a_skse, SKSE::PluginInfo* a_info)
{
	a_info->infoVersion = SKSE::PluginInfo::kVersion;
	a_info->name = PluginName.data();
	a_info->version = StealthFixes::Version::Packed;
	const auto runtime = a_skse->RuntimeVersion();
	const auto family = REL::Module::RuntimeFor(runtime);
	return !a_skse->IsEditor() && (family == REL::Module::Runtime::SE || family == REL::Module::Runtime::VR);
}

extern "C" __declspec(dllexport) bool SKSEAPI SKSEPlugin_Load(const SKSE::LoadInterface* a_skse)
{
	InitializeLog();

	const auto runtime = a_skse->RuntimeVersion();
	logs::info("{} v{} (game {}.{}.{})", DisplayName, StealthFixes::Version::String, runtime[0], runtime[1], runtime[2]);

	const auto family = REL::Module::RuntimeFor(runtime);
	const auto isSE = family == REL::Module::Runtime::SE;
	const auto isAE = family == REL::Module::Runtime::AE && runtime >= SKSE::RUNTIME_SSE_1_6_629;
	const auto isVR = family == REL::Module::Runtime::VR;
	if (!isSE && !isAE && !isVR) {
		logs::warn("This game version is not supported (SE 1.5.97, Anniversary Edition 1.6.629 and later, VR 1.4.15); nothing was changed.");
		return true;
	}

	if (REL::Module::FindAddressLibrary().empty()) {
		logs::warn("Address Library for SKSE Plugins is missing for this game version ({}); nothing was changed.",
			REL::Module::AddressLibraryFileName(REL::Module::get().version()));
		return true;
	}

	SKSE::Init(a_skse);

	StealthFixes::LoadSettings();
	StealthFixes::ApplyLogLevel();
	if (StealthFixes::GetSettings()->debugLogging) {
		logs::info("Debug logging: enabled");
	}
	StealthFixes::LogSettingsSummary();

	StealthFixes::Fixes::Install();

	if (auto* messaging = SKSE::GetMessagingInterface(); messaging) {
		messaging->RegisterListener(OnMessage);
	}

	return true;
}
