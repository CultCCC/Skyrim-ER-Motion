#include "Hooks.h"

#include "ClipUtil.h"
#include "Diag.h"
#include "MotionDebug.h"
#include "MotionFix.h"
#include "Settings.h"
#include "TargetLock.h"

namespace SDS::Hooks
{
	namespace
	{
		bool MotionOn() { return Settings::Get().motionDebug; }

		std::atomic<std::int64_t> lastCharacterUpdateMs{ 0 };
		bool                      fallbackAnnounced = false;

		std::int64_t NowMs()
		{
			return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
		}

		void UpdateActor(RE::Actor* a_actor, float a_delta)
		{
			MotionFix::OnUpdate(a_actor);  // first, so the lock's debug and CSV see the corrected motion
			TargetLock::OnUpdate(a_actor, a_delta);
			if (MotionOn()) {
				MotionDebug::OnUpdate(a_actor, a_delta);
			}
		}

		struct ClipActivate
		{
			static void Thunk(RE::hkbClipGenerator* a_this, const RE::hkbContext& a_context)
			{
				func(a_this, a_context);
				if (!Settings::Get().enabled) {
					return;
				}
				if (const auto actor = ClipUtil::ActorFromContext(a_context)) {
					++Diag::clipActivations;
					Diag::Tick();
					TargetLock::OnClipActivate(actor, a_this, a_context.character);
					MotionFix::OnClipActivate(actor);
					if (MotionOn()) {
						MotionDebug::OnClipActivate(actor, a_this);
					}
				}
			}
			static inline REL::Relocation<decltype(Thunk)> func;
		};

		struct ClipUpdate
		{
			static void Thunk(RE::hkbClipGenerator* a_this, const RE::hkbContext& a_context, float a_timestep)
			{
				func(a_this, a_context, a_timestep);
				if (Settings::Get().enabled && MotionOn()) {
					if (const auto actor = ClipUtil::ActorFromContext(a_context)) {
						++Diag::clipUpdates;
						MotionDebug::OnClipUpdate(actor, a_this);
					}
				}
			}
			static inline REL::Relocation<decltype(Thunk)> func;
		};

		struct ClipDeactivate
		{
			static void Thunk(RE::hkbClipGenerator* a_this, const RE::hkbContext& a_context)
			{
				if (const auto actor = ClipUtil::ActorFromContext(a_context)) {
					TargetLock::OnClipDeactivate(actor, a_this);
					MotionDebug::OnClipDeactivate(actor, a_this);
				}
				func(a_this, a_context);
			}
			static inline REL::Relocation<decltype(Thunk)> func;
		};

		// Character's BSTEventSink<BSAnimationGraphEvent>: every graph event of every NPC
		struct GraphEvent
		{
			static RE::BSEventNotifyControl Thunk(RE::BSTEventSink<RE::BSAnimationGraphEvent>* a_this, RE::BSAnimationGraphEvent* a_event,
				RE::BSTEventSource<RE::BSAnimationGraphEvent>* a_source)
			{
				if (a_event && a_event->holder && Settings::Get().enabled) {
					const auto actor = const_cast<RE::Actor*>(a_event->holder->As<RE::Actor>());
					if (actor && !actor->IsPlayerRef()) {
						++Diag::graphEvents;
						const std::string_view tag = a_event->tag.c_str();
						Diag::SeenTag(tag);
						if (tag.starts_with("SDS_NPC_"sv)) {
							++Diag::sdsEvents;
						}
						TargetLock::OnGraphEvent(actor, tag, a_event->payload.c_str());
					}
				}
				return func(a_this, a_event, a_source);
			}
			static inline REL::Relocation<decltype(Thunk)> func;
		};

		struct CharacterUpdate
		{
			static void Thunk(RE::Character* a_this, float a_delta)
			{
				func(a_this, a_delta);
				++Diag::characterUpdates;
				lastCharacterUpdateMs = NowMs();
				if (a_this && Settings::Get().enabled) {
					UpdateActor(a_this, a_delta);
				}
			}
			static inline REL::Relocation<decltype(Thunk)> func;
		};

		// Actor::ModifyMovementData (vfunc 0x11A): called by the movement update (REL::ID 36365) with this frame's
		// translation (the NPC's own frame, +Y ahead) and rotation, before they are applied
		struct ModifyMovementData
		{
			static void Thunk(RE::Character* a_this, float a_delta, RE::NiPoint3& a_translation, RE::NiPoint3& a_rotation)
			{
				func(a_this, a_delta, a_translation, a_rotation);
				if (a_this && Settings::Get().enabled && !a_this->IsPlayerRef()) {
					TargetLock::OnModifyMovement(a_this, a_translation, a_rotation);
				}
			}
			static inline REL::Relocation<decltype(Thunk)> func;
		};

		// the call that stores the AI turn speed (same site RotateAnnotation hooks; the two chain)
		struct SetRotationSpeedZ
		{
			static void Thunk(RE::AIProcess* a_process, float a_speed)
			{
				if (a_process && Settings::Get().enabled) {
					const auto actor = a_process->GetUserData();
					if (actor && !actor->IsPlayerRef()) {
						if (const auto speed = TargetLock::TurnSpeedOverride(actor, a_speed)) {
							if (a_process->high) {
								a_process->high->pathingCurrentRotationSpeed.z = *speed;
							}
							a_speed = *speed;
						}
					}
				}
				func(a_process, a_speed);
			}
			static inline REL::Relocation<decltype(Thunk)> func;
		};
	}

	void Install()
	{
		REL::Relocation<std::uintptr_t> clipVtbl{ RE::VTABLE_hkbClipGenerator[0] };
		ClipActivate::func = clipVtbl.write_vfunc(0x4, ClipActivate::Thunk);
		ClipUpdate::func = clipVtbl.write_vfunc(0x5, ClipUpdate::Thunk);
		ClipDeactivate::func = clipVtbl.write_vfunc(0x7, ClipDeactivate::Thunk);

		REL::Relocation<std::uintptr_t> sinkVtbl{ RE::VTABLE_Character[2] };
		GraphEvent::func = sinkVtbl.write_vfunc(0x1, GraphEvent::Thunk);

		REL::Relocation<std::uintptr_t> characterVtbl{ RE::VTABLE_Character[0] };
		CharacterUpdate::func = characterVtbl.write_vfunc(0xAD, CharacterUpdate::Thunk);
		ModifyMovementData::func = characterVtbl.write_vfunc(0x11A, ModifyMovementData::Thunk);

		REL::Relocation<std::uintptr_t> rotation{ REL::ID(36365), 0x356 };
		if (*reinterpret_cast<const std::uint8_t*>(rotation.address()) == 0xE8) {
			SetRotationSpeedZ::func = SKSE::GetTrampoline().write_call<5>(rotation.address(), SetRotationSpeedZ::Thunk);
		} else {
			logs::warn("AI rotation speed call not found at {:X}; StopRotation will rely on bHoldHeading", rotation.address());
		}
		logs::info("hooks installed");
	}

	void OnFrame(float a_delta)
	{
		Diag::Tick();
		const auto& settings = Settings::Get();
		if (!settings.enabled || !settings.fallbackDriver || NowMs() - lastCharacterUpdateMs.load() < 1000) {
			return;
		}
		const auto processLists = RE::ProcessLists::GetSingleton();
		if (!processLists) {
			return;
		}
		if (!fallbackAnnounced) {
			fallbackAnnounced = true;
			logs::warn("Character::Update has not fired for a second: NPCs are now updated from the HUD frame (fallback driver)");
		}
		for (auto& handle : processLists->highActorHandles) {
			const auto actor = handle.get();
			if (actor && !actor->IsPlayerRef() && actor->Is3DLoaded()) {
				++Diag::fallbackUpdates;
				UpdateActor(actor.get(), a_delta);
			}
		}
	}
}
