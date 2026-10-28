#pragma once

#include "AnimCatalog.h"

namespace SDS::ClipUtil
{
	// The NPC (never the player) that owns the behavior graph a clip hook was called for.
	RE::Actor* ActorFromContext(const RE::hkbContext& a_context);

	// Clip memory is not always readable, even inside the clip's own hooks: OAR's dummy character (built at
	// kDataLoaded, in the main menu) and clips restored while a save loads carry garbage string pointers.
	// These copy plain bytes under SEH with a length cap; parse the copies afterwards.
	struct RawAnnotation
	{
		float time;
		char  text[48];
	};

	constexpr int kNotBound = -1;  // the animation is not bound yet, try again on a later update
	constexpr int kFaulted = -2;   // reading the clip faulted

	// copies the annotations that start with "anim", "SDS_NPC_" or "Magnetism"; returns how many, or an error above
	int CopyAnnotations(const RE::hkbClipGenerator* a_clip, RawAnnotation* a_out, int a_cap);

	// the clip's animation file name, or its node name, or empty
	std::string AnimationName(const RE::hkbClipGenerator* a_clip);

	// raw pointers of a clip, its binding and animation, every read guarded; for the log
	std::string DumpClip(const RE::hkbClipGenerator* a_clip);

	// Where the NPC really faces: the forward (+Y) axis of the "NPC Root [Root]" bone, as a Skyrim heading
	// (0 = +Y, clockwise). Includes the animation's own root rotation, unlike the actor's heading.
	std::optional<float> RootHeading(RE::Actor* a_actor);

	// the clip's play position and the root motion it extracted on its last update (model space: +Y forward)
	struct ClipMotion
	{
		float localTime;
		float x;
		float y;
		float yaw;  // radians, the extracted rotation about Z
	};
	std::optional<ClipMotion> ReadClipMotion(const RE::hkbClipGenerator* a_clip);

	// the catalog key of the clip's animation (see AnimCatalog.h), read from its compressed data; nothing if the clip
	// is not bound yet, the animation is of another kind (mirrored, delta, wavelet...) or the read faults
	std::optional<AnimCatalog::Key> AnimationKey(const RE::hkbClipGenerator* a_clip);
}
