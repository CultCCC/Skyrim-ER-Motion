#pragma once

namespace SDS::MotionDebug
{
	// Attack root motion check. For the newest playing clip with AMR "animmotion" annotations it draws, from
	// the clip start: the real path (green) and the annotated motion turned three ways - by the heading at
	// clip start + animrotation (yellow), by the heading of each frame (cyan), toward the target (orange).
	// One summary line per clip goes to SDSCombat.log.

	void Init();  // opens the CSV when [MotionDebug] bCsv is on
	void OnClipActivate(RE::Actor* a_actor, const RE::hkbClipGenerator* a_clip);
	void OnClipUpdate(RE::Actor* a_actor, const RE::hkbClipGenerator* a_clip);
	void OnClipDeactivate(RE::Actor* a_actor, const RE::hkbClipGenerator* a_clip);
	void OnUpdate(RE::Actor* a_actor, float a_delta);
	void Clear();
}
