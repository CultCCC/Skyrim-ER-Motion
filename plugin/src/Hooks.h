#pragma once

namespace SDS::Hooks
{
	// One set of engine hooks for every module (target lock, motion debug); each hook hands its event on.
	void Install();  // from SKSEPluginLoad, after SKSE::Init with a trampoline

	// Called once per HUD frame on the main thread. If Character::Update has not fired for a second, the
	// modules' per-actor update runs from here instead, for every high-process NPC ([Debug] bFallbackDriver).
	void OnFrame(float a_delta);
}
