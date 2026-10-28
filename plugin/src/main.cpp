#include "AnimCatalog.h"
#include "DebugDraw.h"
#include "Hooks.h"
#include "MotionDebug.h"
#include "MotionFix.h"
#include "Settings.h"
#include "TargetLock.h"

namespace
{
	void InitLog()
	{
		auto dir = logs::log_directory();
		if (!dir) {
			return;
		}
		auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>((*dir / "SDSCombat.log").string(), true);
		auto log = std::make_shared<spdlog::logger>("global", std::move(sink));
		log->set_level(spdlog::level::info);
		log->flush_on(spdlog::level::info);
		spdlog::set_default_logger(std::move(log));
		spdlog::set_pattern("[%H:%M:%S.%e] [%l] %v");
	}

	// the TrueHUD menu is created after kDataLoaded; hook it for the debug drawing as soon as it opens
	class MenuWatcher final : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
	{
	public:
		static MenuWatcher* GetSingleton()
		{
			static MenuWatcher singleton;
			return &singleton;
		}

		RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
		{
			if (a_event && a_event->opening && a_event->menuName == "TrueHUD"sv) {
				SDS::DebugDraw::TryAttach();
			}
			return RE::BSEventNotifyControl::kContinue;
		}
	};

	void OnMessage(SKSE::MessagingInterface::Message* a_message)
	{
		switch (a_message->type) {
		case SKSE::MessagingInterface::kDataLoaded:
			if (const auto ui = RE::UI::GetSingleton()) {
				ui->AddEventSink<RE::MenuOpenCloseEvent>(MenuWatcher::GetSingleton());
			}
			SDS::DebugDraw::TryAttach();
			if (SDS::Settings::Get().animMotion) {
				std::thread([] {
					const auto s = SDS::AnimCatalog::Build("Data/meshes");
					logs::info("animmotion catalog: {} animations with animmotion of {} scanned, {} usable, {} ambiguous (same data, "
							   "different paths: left alone), {} Reactive Dodge Framework clips skipped, {} unreadable, {:.1f} s",
						s.withMotion, s.scanned, s.entries, s.ambiguous, s.skippedRdf, s.failed, s.seconds);
				}).detach();
			}
			break;
		case SKSE::MessagingInterface::kNewGame:
		case SKSE::MessagingInterface::kPreLoadGame:
			SDS::TargetLock::Clear();
			SDS::MotionDebug::Clear();
			SDS::MotionFix::Clear();
			SDS::DebugDraw::Clear();
			break;
		case SKSE::MessagingInterface::kPostLoadGame:
			SDS::DebugDraw::TryAttach();
			break;
		default:
			break;
		}
	}
}

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
	InitLog();
	SKSE::Init(a_skse, { .log = false, .trampoline = true, .trampolineSize = 64 });  // keep the logger set up above
	logs::info("SDSCombat loaded");

	SDS::Settings::Load();
	SDS::MotionDebug::Init();
	SDS::Hooks::Install();
	SKSE::GetMessagingInterface()->RegisterListener(OnMessage);
	return true;
}
