#include "MotionFix.h"

#include "Settings.h"
#include "TargetLock.h"

namespace SDS::MotionFix
{
	namespace
	{
		constexpr float Pi = std::numbers::pi_v<float>;

		float WrapPi(float a_angle)
		{
			while (a_angle > Pi) a_angle -= 2.0f * Pi;
			while (a_angle < -Pi) a_angle += 2.0f * Pi;
			return a_angle;
		}

		float ToDegrees(float a_radians) { return a_radians * 180.0f / Pi; }
		float ToRadians(float a_degrees) { return a_degrees * Pi / 180.0f; }

		struct State
		{
			std::optional<float> clipStartHeading;  // heading when the newest clip was activated
			bool                 hasPrevious{ false };
			RE::NiPoint3         previousPosition;
			float                previousHeading{ 0.0f };
			float                previousStep{ 0.0f };
			std::optional<float> previousOffset;  // direction of the last displacement minus the heading it was made with
			std::optional<float> correction;      // the jump being undone
			bool                 pending{ false };  // a start / stop seen on the last frame, confirmed if it holds on this one
			float                pendingOffset{ 0.0f };
			float                pendingJump{ 0.0f };
			float                pendingTurned{ 0.0f };
			std::optional<float> pendingTotal;  // the correction to use (start / update), or nothing to stop
		};

		std::mutex                            lock;  // clip activation runs on the graph thread
		std::unordered_map<RE::FormID, State> states;

	}

	void OnClipActivate(RE::Actor* a_actor)
	{
		if (!Settings::Get().motionFix) {
			return;
		}
		std::scoped_lock l{ lock };
		auto&            state = states[a_actor->GetFormID()];
		state.clipStartHeading = a_actor->data.angle.z;
		state.previousOffset.reset();
		state.correction.reset();
		state.pending = false;
	}

	void OnUpdate(RE::Actor* a_actor)
	{
		const auto& settings = Settings::Get();
		if (!settings.motionFix) {
			return;
		}
		bool animationDriven = false;
		a_actor->GetGraphVariableBool("bAnimationDriven"sv, animationDriven);
		const bool watched = animationDriven && a_actor->IsAttacking() && !a_actor->IsDead() && a_actor->Is3DLoaded() &&
		                     a_actor->GetActorRuntimeData().currentCombatTarget.get();

		const auto  position = a_actor->GetPosition();
		const float heading = a_actor->data.angle.z;

		std::string          note;
		std::optional<float> apply;
		RE::NiPoint3         from;
		{
			std::scoped_lock l{ lock };
			if (!watched) {
				if (const auto it = states.find(a_actor->GetFormID()); it != states.end()) {
					if (it->second.correction && settings.logTransitions) {
						note = "ended (attack over)";
					}
					states.erase(it);
				}
			} else {
				auto& s = states[a_actor->GetFormID()];
				if (s.hasPrevious) {
					RE::NiPoint3 moved = position - s.previousPosition;
					moved.z = 0.0f;
					const float step = moved.Length();
					if (step > settings.motionFixMinStep && step < 200.0f) {
						const float offset = WrapPi(std::atan2(moved.x, moved.y) - s.previousHeading);
						const float turned = s.clipStartHeading ? WrapPi(heading - *s.clipStartHeading) : 0.0f;
						const float steady = ToRadians(8.0f);  // a change must hold for a second frame (single-frame spikes at animation keys)
						if (s.pending) {
							if (std::abs(WrapPi(offset - s.pendingOffset)) < steady) {
								if (!s.pendingTotal) {
									note = std::format("ended: root motion changed direction by {:+.0f} deg (was correcting {:+.0f})",
										ToDegrees(s.pendingJump), ToDegrees(s.correction.value_or(0.0f)));
									s.correction.reset();
								} else {
									s.correction = *s.pendingTotal;
									note = std::format("ON: root motion is off by {:+.0f} deg, the turn since the clip started is {:+.0f} deg; turning it back",
										ToDegrees(*s.pendingTotal), ToDegrees(s.pendingTurned));
								}
							}
							s.pending = false;
						} else if (s.previousOffset) {
							// the motion snaps TO the turn since the clip started (not BY it): what is already off plus this jump
							const float jump = WrapPi(offset - *s.previousOffset);
							const float total = WrapPi(s.correction.value_or(0.0f) + jump);
							const bool  sameSpeed = s.previousStep > 0.0f && step > 0.7f * s.previousStep && step < 1.4f * s.previousStep;
							const float tolerance = (std::max)(ToRadians(settings.motionFixTolerance), 0.12f * std::abs(turned));
							// on / update: the motion now sits at the turn since the clip started; off: any other big change of direction
							// (the jump back, or a new step of the animation: then it is safer to stop correcting)
							const bool snap = std::abs(jump) > ToRadians(6.0f) && sameSpeed && std::abs(turned) > ToRadians(settings.motionFixMinTurn) &&
							                  std::abs(WrapPi(total - turned)) < tolerance;
							const bool stop = s.correction && std::abs(jump) > ToRadians(20.0f);
							if (snap || stop) {
								s.pending = true;
								s.pendingOffset = offset;
								s.pendingJump = jump;
								s.pendingTurned = turned;
								s.pendingTotal = snap ? std::optional<float>(total) : std::nullopt;
							}
						}
						s.previousOffset = offset;
						s.previousStep = step;
						if (s.correction) {
							apply = s.correction;
							from = s.previousPosition;
						}
					}
				}
				s.hasPrevious = true;
				s.previousHeading = heading;
				s.previousPosition = position;
			}
		}

		if (apply) {
			// the displacement of this frame, turned back by the jump
			RE::NiPoint3 moved = position - from;
			const float  c = std::cos(-*apply);
			const float  s = std::sin(-*apply);
			// heading convention: angle a = atan2(x, y), clockwise; turning by d maps (x, y) to (x cos d + y sin d, -x sin d + y cos d)
			const RE::NiPoint3 fixed{ moved.x * c + moved.y * s, -moved.x * s + moved.y * c, moved.z };
			const RE::NiPoint3 target = from + fixed;
			a_actor->SetPosition(target, true);
			std::scoped_lock l{ lock };
			if (const auto it = states.find(a_actor->GetFormID()); it != states.end()) {
				it->second.previousPosition = target;
			}
		}
		if (!note.empty()) {
			if (settings.logTransitions) {
				logs::info("{} [{:08X}] motion fix {}", a_actor->GetName(), a_actor->GetFormID(), note);
			}
			TargetLock::CsvNote(a_actor, note.starts_with("ON") ? "SDS_motionfix_on"sv : "SDS_motionfix_off"sv);
		}
	}

	void Clear()
	{
		std::scoped_lock l{ lock };
		states.clear();
	}
}
