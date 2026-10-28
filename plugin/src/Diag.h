#pragma once

#include <atomic>

namespace SDS::Diag
{
	// How often each hook fired; written to the log every few seconds when [Debug] bDiagnostics is on, so a
	// hook that never fires in game shows up as a zero instead of as silence.
	inline std::atomic<std::uint64_t> characterUpdates{ 0 };
	inline std::atomic<std::uint64_t> clipActivations{ 0 };  // NPC clips
	inline std::atomic<std::uint64_t> clipUpdates{ 0 };
	inline std::atomic<std::uint64_t> graphEvents{ 0 };  // all NPC graph events
	inline std::atomic<std::uint64_t> sdsEvents{ 0 };    // SDS_NPC_* among them
	inline std::atomic<std::uint64_t> renders{ 0 };
	inline std::atomic<std::uint64_t> linesDrawn{ 0 };
	inline std::atomic<std::uint64_t> fallbackUpdates{ 0 };

	void Tick();  // call from any hook; logs at most every 5 seconds

	void SeenTag(std::string_view a_tag);  // NPC graph event names of the current window (first 40 distinct)
}
