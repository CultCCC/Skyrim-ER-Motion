#pragma once

namespace SDS
{
	// Everything in SDSCombat.ini, read once at load; the hooks only read it afterwards.
	struct Settings
	{
		// [General]
		bool enabled{ true };

		// [TargetLock] SDS_NPC_TargetLockOn
		float turnSpeed{ 0.0f };    // degrees per second; 0 = full force, the NPC faces the target every frame
		int   target{ 0 };          // 0 = player, only while the player is the NPC's combat target; 1 = player always; 2 = the NPC's combat target
		float maxDistance{ 0.0f };  // game units; 0 = no limit
		int   lockMethod{ 1 };      // 0 = SetHeading every frame (root motion drifts sideways); 1 = through the AI turn speed
		float maxEngineTurnSpeed{ 240.0f };  // degrees per second, iMethod = 1 with fTurnSpeed = 0
		bool  straightRootMotion{ true };    // while the lock is on, a sideways root-motion step goes straight ahead instead
		float straightMaxAngle{ 150.0f };  // degrees: steps further off ahead than this (backwards) are left alone
		bool  animMotion{ true };          // [AnimMotion] root-motion direction from the clip's animmotion annotations
		float animMotionMinStep{ 0.05f };  // annotated motion per frame (game units) below this: the engine's step is kept
		bool  bumpAnnotations{ true };     // [Bump] SDS_NPC_BumpOff / BumpOn drive Animation Motion Fix's AMF_bForbidMoveMagnetism
		float reachMinScale{ 0.0f };      // [ReachTarget] SDS_NPC_ReachTarget never scales the root motion below / above these
		float reachMaxScale{ 2.0f };

		// [StopRotation] SDS_NPC_StopRotation
		bool zeroAIRotation{ true };  // AI turn speed forced to 0
		bool holdHeading{ true };     // heading pinned to the value it had when the annotation fired (also blocks root-motion yaw)

		// [Compatibility] MagnetismStartTCB / MagnetismStopTCB left over from Trigger Combat Behaviour
		bool legacyTCB{ false };

		// [Log]
		bool logAnnotations{ false };  // every SDS annotation an NPC receives
		bool logTransitions{ false };   // every change of what the target lock does to an NPC

		// [Debug]
		bool diagnostics{ false };   // hook counters in the log every 5 seconds
		bool testDraw{ false };     // a cross over the player every frame, to see that drawing works at all
		bool fallbackDriver{ true };  // if Character::Update never fires, update the NPCs from the HUD render instead
		bool movementCsv{ false };     // per-frame movement of watched NPCs + their graph events to SDSCombat_movement.csv

		// [LockDebug] where a locked NPC should look and where it really looks, drawn every frame
		bool  lockDebug{ false };
		bool  combatDebug{ false };  // also watch / draw NPCs fighting the player when no SDS flag is set
		float lockArrowLength{ 110.0f };
		float lockThickness{ 3.0f };
		bool  lockDrawBody{ true };  // the facing of the upper body, from the shoulder bones

		// [MotionFix] root motion that jumps by the turn since the clip started is turned back (see MotionFix.h)
		bool  motionFix{ false };  // experimental: catches only part of the drift, moves the NPC with SetPosition
		float motionFixMinTurn{ 15.0f };    // degrees: smaller turns are not checked
		float motionFixTolerance{ 8.0f };   // degrees: how close the jump must be to the turn
		float motionFixMinStep{ 0.8f };     // game units per frame: slower motion is not checked

		// [MotionDebug] attack root motion: real path against the AMR animmotion annotations
		bool  motionDebug{ false };
		bool  motionOnlyFightingPlayer{ true };
		float motionMaxDistance{ 4000.0f };
		bool  motionCsv{ false };
		bool  motionDraw{ true };
		float motionPersistSeconds{ 6.0f };
		float motionThickness{ 2.0f };
		float motionZOffset{ 4.0f };
		bool  motionDrawActual{ true };
		bool  motionDrawClipFrame{ true };
		bool  motionDrawCurrentFrame{ true };
		bool  motionDrawRootFrame{ true };
		bool  motionDrawIdeal{ true };
		bool  motionDrawArrows{ true };
		float animRotationSign{ 1.0f };

		static const Settings& Get();
		static void            Load();

		static constexpr auto Path = "Data/SKSE/Plugins/SDSCombat.ini"sv;
	};
}
