#pragma once

namespace SDS::TargetLock
{
	// Annotation-driven NPC rotation control. NPCs have no target lock until a clip asks for it:
	//   SDS_NPC_TargetLockOn     lock onto the player at full force
	//   SDS_NPC_TargetLockOff    no lock, even if TargetLockOn fires later in the clip
	//   SDS_NPC_StopRotation     no rotation at all (AI turning and root-motion yaw)
	//   SDS_NPC_TargetLockReset  forget the three above, so they can be fired again
	// Priority: Reset > StopRotation > TargetLockOff > TargetLockOn. Each flag lives until the clip that
	// fired it is deactivated (or played again); clips without these annotations change nothing.

	void OnClipActivate(RE::Actor* a_actor, const RE::hkbClipGenerator* a_clip, const RE::hkbCharacter* a_character);
	void OnClipDeactivate(RE::Actor* a_actor, const RE::hkbClipGenerator* a_clip);
	void OnGraphEvent(RE::Actor* a_actor, std::string_view a_tag, std::string_view a_payload);
	void OnUpdate(RE::Actor* a_actor, float a_delta);
	// called from the engine's AI turn-speed store: the speed to use instead of the AI's (StopRotation: 0, a lock with
	// [TargetLock] iMethod = 1: the speed that faces the target this frame), or nothing to keep the AI's
	std::optional<float> TurnSpeedOverride(RE::Actor* a_actor, float a_aiSpeed);
	void Clear();

	// an extra event row in the movement CSV (only for NPCs the debug is watching)
	void CsvNote(RE::Actor* a_actor, std::string_view a_text);

	// Actor::ModifyMovementData: the engine's root-motion step for this frame, in the NPC's own frame (+Y ahead).
	// Recorded for the CSV; while the lock is on ([TargetLock] bStraightRootMotion) a sideways step is turned straight ahead.
	void OnModifyMovement(RE::Actor* a_actor, RE::NiPoint3& a_translation, RE::NiPoint3& a_rotation);
}
