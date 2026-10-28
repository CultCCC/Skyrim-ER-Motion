#include "Diag.h"

#include "Settings.h"

namespace SDS::Diag
{
	namespace
	{
		std::atomic<std::int64_t> nextReport{ 0 };

		std::mutex               tagLock;
		std::vector<std::string> tags;  // distinct names this window

		std::int64_t NowMs()
		{
			return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
		}
	}

	void Tick()
	{
		if (!Settings::Get().diagnostics) {
			return;
		}
		const auto now = NowMs();
		auto       due = nextReport.load();
		if (now < due || !nextReport.compare_exchange_strong(due, now + 5000)) {
			return;
		}
		if (due == 0) {
			return;  // first call only arms the timer
		}
		logs::info("[diag] last 5 s: Character::Update {} | NPC clip activate {} / update {} | NPC graph events {} (SDS_NPC_* {}) | "
				   "renders {}, lines drawn {} | fallback actor updates {}",
			characterUpdates.exchange(0), clipActivations.exchange(0), clipUpdates.exchange(0), graphEvents.exchange(0), sdsEvents.exchange(0),
			renders.exchange(0), linesDrawn.exchange(0), fallbackUpdates.exchange(0));

		std::string list;
		{
			std::scoped_lock l{ tagLock };
			for (const auto& tag : tags) {
				list += list.empty() ? tag : ", " + tag;
			}
			tags.clear();
		}
		if (!list.empty()) {
			logs::info("[diag] NPC graph events seen: {}", list);
		}
	}

	void SeenTag(std::string_view a_tag)
	{
		if (!Settings::Get().diagnostics) {
			return;
		}
		std::scoped_lock l{ tagLock };
		if (tags.size() < 40 && std::ranges::find(tags, a_tag) == tags.end()) {
			tags.emplace_back(a_tag);
		}
	}
}
