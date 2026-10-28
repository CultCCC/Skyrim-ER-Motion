#pragma once

namespace SDS::MotionFix
{
	// Root motion that suddenly turns by the NPC's turn since the clip started.
	// Measured on the SDS bosses with Root Motion Revolution: while an NPC turns during an animation-driven attack,
	// the annotated root motion is applied along heading + animation, as it should be, until at some point in the clip
	// it jumps by exactly (heading now - heading at clip start) and stays there until a later step of the animation.
	// The NPC then slides sideways. This watches for that jump and turns the displacement back while it lasts.
	// [MotionFix] in SDSCombat.ini.

	void OnClipActivate(RE::Actor* a_actor);
	void OnUpdate(RE::Actor* a_actor);  // after the engine moved the actor, before the lock's own update
	void Clear();
}
