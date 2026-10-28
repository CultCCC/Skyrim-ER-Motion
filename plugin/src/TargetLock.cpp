#include "TargetLock.h"

#include "ClipUtil.h"
#include "DebugDraw.h"
#include "Settings.h"

namespace SDS::TargetLock
{
	namespace
	{
		// annotation names; a clip carries them as "<name>" or "<name>.<payload>"
		enum Tag : std::uint16_t
		{
			kNone = 0,
			kOn = 1 << 0,  // SDS_NPC_TargetLockOn[.strength]
			kOff = 1 << 1,
			kStop = 1 << 2,
			kReset = 1 << 3,
			kLegacyStart = 1 << 4,  // MagnetismStartTCB.MaxAngle|TurnSpeed|Strength|AllowRetargeting
			kLegacyStop = 1 << 5,   // MagnetismStopTCB
			kTurnSpeed = 1 << 6,    // SDS_NPC_TurnSpeed.strength: change the lock's strength (and lock on)
			kReach = 1 << 8,        // SDS_NPC_ReachTarget.ref|min|max|arrive
			kReachEnd = 1 << 9,     // SDS_NPC_ReachTargetEnd
			kBumpOff = 1 << 10,     // SDS_NPC_BumpOff: Animation Motion Fix's movement magnetism off (no sticking on the target)
			kBumpOn = 1 << 11,      // SDS_NPC_BumpOn: back to normal
			kUnknown = 1 << 15      // clip whose annotations could not be read
		};

		Tag TagFromName(std::string_view a_name)
		{
			if (const auto dot = a_name.find('.'); dot != std::string_view::npos) {
				a_name = a_name.substr(0, dot);
			}
			if (a_name == "SDS_NPC_TargetLockOn"sv) return kOn;
			if (a_name == "SDS_NPC_TargetLockOff"sv) return kOff;
			if (a_name == "SDS_NPC_StopRotation"sv) return kStop;
			if (a_name == "SDS_NPC_TargetLockReset"sv) return kReset;
			if (a_name == "SDS_NPC_TurnSpeed"sv) return kTurnSpeed;
			if (a_name == "SDS_NPC_ReachTarget"sv) return kReach;
			if (a_name == "SDS_NPC_ReachTargetEnd"sv) return kReachEnd;
			if (a_name == "SDS_NPC_BumpOff"sv || a_name == "SDS_NPCBumpOff"sv) return kBumpOff;
			if (a_name == "SDS_NPC_BumpOn"sv || a_name == "SDS_NPCBumpOn"sv) return kBumpOn;
			if (a_name == "MagnetismStartTCB"sv) return kLegacyStart;
			if (a_name == "MagnetismStopTCB"sv) return kLegacyStop;
			return kNone;
		}

		std::string_view TagName(Tag a_tag)
		{
			switch (a_tag) {
			case kOn: return "SDS_NPC_TargetLockOn"sv;
			case kOff: return "SDS_NPC_TargetLockOff"sv;
			case kStop: return "SDS_NPC_StopRotation"sv;
			case kReset: return "SDS_NPC_TargetLockReset"sv;
			case kTurnSpeed: return "SDS_NPC_TurnSpeed"sv;
			case kReach: return "SDS_NPC_ReachTarget"sv;
			case kReachEnd: return "SDS_NPC_ReachTargetEnd"sv;
			case kBumpOff: return "SDS_NPC_BumpOff"sv;
			case kBumpOn: return "SDS_NPC_BumpOn"sv;
			case kLegacyStart: return "MagnetismStartTCB"sv;
			case kLegacyStop: return "MagnetismStopTCB"sv;
			default: return "?"sv;
			}
		}

		struct ActiveClip
		{
			const RE::hkbClipGenerator* clip;
			std::uint64_t               order;             // activation order, newest is highest
			std::uint16_t               tags{ 0 };         // annotations this clip's animation carries
			bool                        scanned{ false };  // tags are read lazily, when the actor first sends an annotation
			bool                        curveResolved{ false };  // looked up in the animmotion catalog
			std::uint8_t                keyTries{ 0 };           // frames the animation could not be read yet (not bound)
			const AnimCatalog::Curve*   curve{ nullptr };        // its annotated root motion, if the catalog knows the animation
		};

		struct ActorState
		{
			const RE::hkbCharacter*     character{ nullptr };  // the graph the clips below belong to
			std::vector<ActiveClip>     active;
			const RE::hkbClipGenerator* owner{ nullptr };  // the clip whose annotations set the flags below
			std::uint16_t               flags{ kNone };    // kOn | kOff | kStop, latched until reset or clip change
			std::optional<float>        strength;  // lock strength 0..1 from SDS_NPC_TargetLockOn.x / SDS_NPC_TurnSpeed.x; none = [TargetLock] default
			std::optional<float>        reachScale;  // SDS_NPC_ReachTarget: forward root motion multiplied by this until ReachTargetEnd
			bool                        bumpOff{ false };  // SDS_NPC_BumpOff until BumpOn / clip end (applied by the actor update)
			bool                        legacy{ false };   // a TCB magnetism session is running
			float                       legacyMaxAngle{ 180.0f };
			float                       legacyTurnSpeed{ 720.0f };
			float                       legacyStrength{ 100.0f };
			std::optional<float>        stopYaw;  // heading captured on the first frame of StopRotation

			// iMethod = 1: the lock turns the NPC through the AI turn speed, so root motion follows the new heading
			std::optional<float> engineWanted;           // heading the AI turn speed steers to, set by the actor update
			float                engineLimit{ 0.0f };    // radians per second
			std::uint32_t        framesSinceAiCall{ 0 };  // actor updates since the AI turn-speed store last ran
			bool                 fallbackLogged{ false };

			// the lock turned the NPC on its last update (ModifyMovementData then straightens the root motion)
			bool lockActive{ false };

			// ModifyMovementData since the last CSV frame row: the engine's local root-motion step (+Y forward) and turn
			std::uint32_t mmCalls{ 0 };
			float         mmX{ 0.0f };
			float         mmY{ 0.0f };
			float         mmRotZ{ 0.0f };
			std::uint32_t mmStraightened{ 0 };
			std::uint32_t mmReached{ 0 };
			std::uint32_t mmAnim{ 0 };  // steps whose direction came from the clip's animmotion

			// [AnimMotion]: the clip that drives the root motion and its play position on the last step
			const RE::hkbClipGenerator* animClip{ nullptr };
			float                       animPrevTime{ 0.0f };
			bool          straightLogged{ false };

			// AI turn-speed store calls since the last CSV frame row
			std::uint32_t aiCalls{ 0 };
			float         aiIn{ 0.0f };
			float         aiOut{ 0.0f };

			void ResetFlags()
			{
				flags = kNone;
				legacy = false;
				stopYaw.reset();
				strength.reset();
				reachScale.reset();
				bumpOff = false;
				engineWanted.reset();
				fallbackLogged = false;
				lockActive = false;
				straightLogged = false;
			}

			bool HasEffect() const { return flags != kNone || legacy; }
		};

		std::mutex                                 lock;  // graph updates and actor updates run on different threads
		std::unordered_set<RE::FormID>             bumpApplied;  // NPCs whose AMF_bForbidMoveMagnetism this plugin set to true
		std::unordered_map<RE::FormID, ActorState> actors;
		std::uint64_t                              activations{ 0 };

		constexpr std::size_t MaxActiveClips = 64;  // a graph that never sends Deactivate cannot grow without bound
		constexpr float       Pi = std::numbers::pi_v<float>;

		float WrapPi(float a_angle)
		{
			while (a_angle > Pi) a_angle -= 2.0f * Pi;
			while (a_angle < -Pi) a_angle += 2.0f * Pi;
			return a_angle;
		}

		float ToRadians(float a_degrees) { return a_degrees * Pi / 180.0f; }
		float ToDegrees(float a_radians) { return a_radians * 180.0f / Pi; }

		std::uint16_t ScanAnnotations(const RE::hkbClipGenerator* a_clip)
		{
			static thread_local std::vector<ClipUtil::RawAnnotation> raw(512);
			const int count = ClipUtil::CopyAnnotations(a_clip, raw.data(), static_cast<int>(raw.size()));
			if (count < 0) {
				return kUnknown;
			}
			std::uint16_t tags = kNone;
			for (int i = 0; i < count; ++i) {
				tags |= TagFromName(raw[i].text);
			}
			return tags;
		}

		// "180|720|100|true"; missing or bad fields keep their defaults
		void ParseLegacyPayload(ActorState& a_state, std::string_view a_payload)
		{
			float*      fields[] = { &a_state.legacyMaxAngle, &a_state.legacyTurnSpeed, &a_state.legacyStrength };
			std::size_t i = 0;
			while (i < std::size(fields) && !a_payload.empty()) {
				const auto bar = a_payload.find('|');
				const auto part = a_payload.substr(0, bar);
				float      value{};
				if (std::from_chars(part.data(), part.data() + part.size(), value).ec == std::errc{}) {
					*fields[i] = value;
				}
				++i;
				a_payload = bar == std::string_view::npos ? std::string_view{} : a_payload.substr(bar + 1);
			}
		}

		// the payload of "<name>.<payload>": the event's own payload, or what follows the first dot of its tag
		std::string_view PayloadOf(std::string_view a_tag, std::string_view a_payload)
		{
			if (!a_payload.empty()) {
				return a_payload;
			}
			const auto dot = a_tag.find('.');
			return dot == std::string_view::npos ? std::string_view{} : a_tag.substr(dot + 1);
		}

		std::optional<float> ParseFloat(std::string_view a_text)
		{
			while (!a_text.empty() && a_text.front() == ' ') a_text.remove_prefix(1);
			float value{};
			if (a_text.empty() || std::from_chars(a_text.data(), a_text.data() + a_text.size(), value).ec != std::errc{}) {
				return std::nullopt;
			}
			return value;
		}

		RE::Actor* ResolveTarget(RE::Actor* a_actor)
		{
			const auto player = RE::PlayerCharacter::GetSingleton();
			const auto combatTarget = a_actor->GetActorRuntimeData().currentCombatTarget.get();
			switch (Settings::Get().target) {
			case 0:
				return combatTarget.get() == player ? player : nullptr;
			case 1:
				return player;
			default:
				return combatTarget.get();
			}
		}

		// --- what the plugin did to an NPC this frame; changes are logged ---

		enum class Mode : std::uint8_t
		{
			kNone,           // no SDS flag, the engine turns the NPC
			kLock,           // TargetLockOn (or a TCB session): turning toward the target
			kLockNoTarget,   // a lock was asked for but there is no target to lock onto
			kLockNotPlayer,  // iTarget = 0 and the NPC is not fighting the player
			kLockTooFar,     // beyond fMaxDistance
			kLockOutOfCone,  // TCB session only: the target is outside its MaxAngle
			kOff,            // TargetLockOff
			kStop,           // StopRotation
			kSuspended,      // flags set, but the NPC is dead, in a killmove or ragdolled
			kCombat          // no SDS flag, in combat with the player: only watched by the debug ([LockDebug] bCombat)
		};

		std::string_view ModeName(Mode a_mode)
		{
			switch (a_mode) {
			case Mode::kLock: return "LOCK ON"sv;
			case Mode::kLockNoTarget: return "lock asked, no combat target"sv;
			case Mode::kLockNotPlayer: return "lock asked, combat target is not the player"sv;
			case Mode::kLockTooFar: return "lock asked, target beyond fMaxDistance"sv;
			case Mode::kLockOutOfCone: return "TCB lock asked, target outside MaxAngle"sv;
			case Mode::kOff: return "LOCK OFF (TargetLockOff)"sv;
			case Mode::kStop: return "ROTATION STOPPED"sv;
			case Mode::kSuspended: return "suspended (dead / killmove / ragdoll)"sv;
			case Mode::kCombat: return "in combat, engine turning (no SDS flags)"sv;
			default: return "released, engine turning"sv;
			}
		}

		// errors are measured each frame: "before" = how far the engine / animation turned the NPC away from where
		// the lock wants it since the last frame; "after" = left after this plugin's correction; "body" = the
		// NPC Root bone against the wanted direction, i.e. where the NPC really faces
		struct ModeRecord
		{
			Mode          mode{ Mode::kNone };
			std::uint32_t frames{ 0 };
			float         maxBefore{ 0.0f };
			float         maxAfter{ 0.0f };
			float         maxBody{ 0.0f };
			float         sumBody{ 0.0f };
			std::uint32_t bodyFrames{ 0 };

			// movement: the NPC's real displacement each frame, against the directions it could have followed.
			// The displacement of a frame was produced with the heading the NPC had at the end of the previous one.
			bool          hasPrevious{ false };
			RE::NiPoint3  previousPosition;
			float         previousHeading{ 0.0f };
			float         previousRoot{ 0.0f };
			float         startHeading{ 0.0f };
			std::uint32_t moveFrames{ 0 };
			float         distance{ 0.0f };
			float         forward{ 0.0f };  // along the previous heading
			float         side{ 0.0f };     // to the right of it
			float         offHeading{ 0.0f };  // sums of |angle| between the displacement and ...
			float         offStart{ 0.0f };
			float         offRoot{ 0.0f };
			float         offWanted{ 0.0f };
		};
		std::unordered_map<RE::FormID, ModeRecord> records;  // only NPCs whose mode is not kNone; guarded by `lock`

		std::string TargetText(RE::Actor* a_target, RE::Actor* a_actor)
		{
			if (!a_target) {
				return "none";
			}
			return std::format("{} [{:08X}] at {:.0f} units", a_target->GetName(), a_target->GetFormID(),
				a_actor->GetPosition().GetDistance(a_target->GetPosition()));
		}

		void LogMode(RE::Actor* a_actor, Mode a_mode, std::uint16_t a_flags, bool a_legacy, RE::Actor* a_target, float a_turn)
		{
			ModeRecord finished;
			bool       changed = false;
			{
				std::scoped_lock l{ lock };
				const auto       it = records.find(a_actor->GetFormID());
				const auto       previous = it == records.end() ? Mode::kNone : it->second.mode;
				if (previous == a_mode) {
					return;
				}
				changed = true;
				if (it != records.end()) {
					finished = it->second;
				}
				if (a_mode == Mode::kNone) {
					if (it != records.end()) {
						records.erase(it);
					}
				} else {
					records[a_actor->GetFormID()] = ModeRecord{ a_mode };
				}
			}
			if (!changed || !Settings::Get().logTransitions) {
				return;
			}
			if (finished.mode == Mode::kLock || finished.mode == Mode::kStop || finished.mode == Mode::kCombat) {
				logs::info("{} [{:08X}]   ...{} lasted {} frames | heading error: max before correction {:.1f} deg, after {:.1f} deg | "
						   "NPC Root vs wanted: max {:.1f} deg, average {:.1f} deg",
					a_actor->GetName(), a_actor->GetFormID(), ModeName(finished.mode), finished.frames, finished.maxBefore,
					finished.maxAfter, finished.maxBody, finished.bodyFrames ? finished.sumBody / finished.bodyFrames : 0.0f);
				if (finished.moveFrames > 0) {
					const float n = static_cast<float>(finished.moveFrames);
					logs::info("{} [{:08X}]   ...movement: {:.0f} units in {} moving frames, {:.0f} forward / {:+.0f} sideways (right +) of the heading | "
							   "average angle of the displacement to: heading {:.0f} deg, heading at start {:.0f} deg, NPC Root {:.0f} deg, wanted {:.0f} deg",
						a_actor->GetName(), a_actor->GetFormID(), finished.distance, finished.moveFrames, finished.forward, finished.side,
						finished.offHeading / n, finished.offStart / n, finished.offRoot / n, finished.offWanted / n);
				}
			}
			const auto combatTarget = a_actor->GetActorRuntimeData().currentCombatTarget.get();
			logs::info("{} [{:08X}] {} | flags on={} off={} stop={} tcb={} | lock target: {} | combat target: {} | angle to target {:.0f} deg",
				a_actor->GetName(), a_actor->GetFormID(), ModeName(a_mode), (a_flags & kOn) != 0, (a_flags & kOff) != 0,
				(a_flags & kStop) != 0, a_legacy, TargetText(a_target, a_actor), TargetText(combatTarget.get(), a_actor), ToDegrees(a_turn));
		}

		RE::NiPoint3 Direction(float a_heading) { return { std::sin(a_heading), std::cos(a_heading), 0.0f }; }

		// --- per-frame movement CSV ([Debug] bMovementCsv): one row per frame per watched NPC, plus its graph events ---

		std::ofstream csv;
		std::mutex    csvLock;
		bool          csvTried = false;
		const auto    csvStart = std::chrono::steady_clock::now();

		double CsvTime()
		{
			return std::chrono::duration<double>(std::chrono::steady_clock::now() - csvStart).count();
		}

		bool CsvOpen()
		{
			if (!Settings::Get().movementCsv) {
				return false;
			}
			std::scoped_lock l{ csvLock };
			if (!csvTried) {
				csvTried = true;
				if (const auto dir = logs::log_directory()) {
					csv.open(*dir / "SDSCombat_movement.csv", std::ios::trunc);
					csv << "time,actor,row,mode,x,y,z,heading,root,wanted,dx,dy,frameSeconds,attacking,bAnimationDriven,bAllowRotation,aiCalls,aiSpeedIn,aiSpeedOut,clip,clipTime,clipDx,clipDy,clipYaw,mmCalls,mmX,mmY,mmRotZ,straightened,reached,animSteered,event\n";
				}
			}
			return csv.is_open();
		}

		void CsvEvent(RE::Actor* a_actor, std::string_view a_tag)
		{
			if (!CsvOpen()) {
				return;
			}
			{
				std::scoped_lock l{ lock };
				if (!records.contains(a_actor->GetFormID())) {
					return;  // only NPCs the debug is watching
				}
			}
			std::scoped_lock l{ csvLock };
			csv << std::format("{:.4f},{:08X},event,,,,,,,,,,,,,,,,,,,,,,,,,,,,,{}\n", CsvTime(), a_actor->GetFormID(), a_tag);
		}

		void CsvFrame(RE::Actor* a_actor, Mode a_mode, const RE::NiPoint3& a_position, float a_heading, float a_root, float a_wanted,
			const RE::NiPoint3& a_moved, float a_seconds)
		{
			if (!CsvOpen()) {
				return;
			}
			bool animationDriven = false, allowRotation = false;
			a_actor->GetGraphVariableBool("bAnimationDriven"sv, animationDriven);
			a_actor->GetGraphVariableBool("bAllowRotation"sv, allowRotation);
			std::uint32_t               aiCalls = 0;
			float                       aiIn = 0.0f, aiOut = 0.0f;
			const RE::hkbClipGenerator* newest = nullptr;  // the clip activated last: the attack, normally
			std::uint32_t               mmCalls = 0, mmStraightened = 0, mmReached = 0, mmAnim = 0;
			float                       mmX = 0.0f, mmY = 0.0f, mmRotZ = 0.0f;
			{
				std::scoped_lock l{ lock };
				if (const auto it = actors.find(a_actor->GetFormID()); it != actors.end()) {
					aiCalls = std::exchange(it->second.aiCalls, 0);
					aiIn = it->second.aiIn;
					aiOut = it->second.aiOut;
					mmCalls = std::exchange(it->second.mmCalls, 0);
					mmX = std::exchange(it->second.mmX, 0.0f);
					mmY = std::exchange(it->second.mmY, 0.0f);
					mmRotZ = std::exchange(it->second.mmRotZ, 0.0f);
					mmStraightened = std::exchange(it->second.mmStraightened, 0);
					mmReached = std::exchange(it->second.mmReached, 0);
					mmAnim = std::exchange(it->second.mmAnim, 0);
					std::uint64_t order = 0;
					for (const auto& c : it->second.active) {
						if (c.order >= order) {
							order = c.order;
							newest = c.clip;
						}
					}
				}
			}
			const auto motion = ClipUtil::ReadClipMotion(newest);
			std::scoped_lock l{ csvLock };
			csv << std::format("{:.4f},{:08X},frame,{},{:.1f},{:.1f},{:.1f},{:.1f},{:.1f},{:.1f},{:.2f},{:.2f},{:.4f},{},{},{},{},{:.3f},{:.3f},",
				CsvTime(), a_actor->GetFormID(), ModeName(a_mode), a_position.x, a_position.y, a_position.z, ToDegrees(a_heading),
				ToDegrees(a_root), ToDegrees(a_wanted), a_moved.x, a_moved.y, a_seconds, a_actor->IsAttacking() ? 1 : 0, animationDriven ? 1 : 0,
				allowRotation ? 1 : 0, aiCalls, aiIn, aiOut);
			if (motion) {
				csv << std::format("{:06X},{:.3f},{:.2f},{:.2f},{:.2f},", reinterpret_cast<std::uintptr_t>(newest) & 0xFFFFFF, motion->localTime,
					motion->x, motion->y, ToDegrees(motion->yaw));
			} else {
				csv << ",,,,,";
			}
			csv << std::format("{},{:.3f},{:.3f},{:.4f},{},{},{},\n", mmCalls, mmX, mmY, mmRotZ, mmStraightened, mmReached, mmAnim);
		}

		// a_before / a_after: wanted - heading before and after this frame's correction; a_root: NPC Root heading
		void Record(RE::Actor* a_actor, float a_wanted, float a_before, float a_after, std::optional<float> a_root)
		{
			const auto   position = a_actor->GetPosition();
			const float  heading = a_actor->data.angle.z;
			const float  seconds = RE::GetSecondsSinceLastFrame();
			RE::NiPoint3 moved;
			bool         drawMove = false;
			Mode         mode = Mode::kNone;
			{
				std::scoped_lock l{ lock };
				const auto       it = records.find(a_actor->GetFormID());
				if (it == records.end()) {
					return;
				}
				auto& r = it->second;
				mode = r.mode;
				++r.frames;
				r.maxBefore = (std::max)(r.maxBefore, std::abs(ToDegrees(a_before)));
				r.maxAfter = (std::max)(r.maxAfter, std::abs(ToDegrees(a_after)));
				if (a_root) {
					const float body = std::abs(ToDegrees(WrapPi(a_wanted - *a_root)));
					r.maxBody = (std::max)(r.maxBody, body);
					r.sumBody += body;
					++r.bodyFrames;
				}

				if (r.hasPrevious) {
					moved = position - r.previousPosition;
					moved.z = 0.0f;
					const float step = moved.Length();
					if (step > 0.3f && step < 200.0f) {  // ignore standing still and teleports
						const float yaw = std::atan2(moved.x, moved.y);
						const auto  along = Direction(r.previousHeading);
						const RE::NiPoint3 right{ along.y, -along.x, 0.0f };
						++r.moveFrames;
						r.distance += step;
						r.forward += moved.Dot(along);
						r.side += moved.Dot(right);
						r.offHeading += std::abs(ToDegrees(WrapPi(yaw - r.previousHeading)));
						r.offStart += std::abs(ToDegrees(WrapPi(yaw - r.startHeading)));
						r.offRoot += std::abs(ToDegrees(WrapPi(yaw - r.previousRoot)));
						r.offWanted += std::abs(ToDegrees(WrapPi(yaw - a_wanted)));
						drawMove = true;
					}
				} else {
					r.startHeading = heading;
				}
				r.hasPrevious = true;
				r.previousPosition = position;
				r.previousHeading = heading;
				r.previousRoot = a_root.value_or(heading);
			}

			CsvFrame(a_actor, mode, position, heading, a_root.value_or(heading), a_wanted, moved, seconds);

			// white arrow: where the NPC actually moved this frame, as a velocity (length = 0.3 s of travel)
			const auto& settings = Settings::Get();
			if (drawMove && settings.lockDebug && seconds > 1e-4f) {
				const RE::NiPoint3 feet{ position.x, position.y, position.z + 20.0f };
				DebugDraw::Arrow(feet, feet + moved * (0.3f / seconds), 0xFFFFFF, settings.lockThickness);
			}
		}

		// --- lock debug drawing ---

		// where the NPC really faces: the NPC Root bone (includes the animation's own root rotation)
		std::optional<float> BodyHeading(RE::Actor* a_actor) { return ClipUtil::RootHeading(a_actor); }

		void DrawLockDebug(RE::Actor* a_actor, float a_wanted, bool a_stop, RE::Actor* a_target, std::optional<float> a_body)
		{
			const auto& settings = Settings::Get();
			if (!settings.lockDebug) {
				return;
			}
			constexpr std::uint32_t Wanted = 0x40FF40;    // green: where the lock wants the NPC to look
			constexpr std::uint32_t Pinned = 0xFF4040;    // red: the heading StopRotation holds
			constexpr std::uint32_t Heading = 0xFFD020;   // yellow: the NPC's heading (what the engine uses)
			constexpr std::uint32_t Body = 0x30C0FF;      // blue: where the NPC Root bone really faces
			constexpr std::uint32_t ToTarget = 0xFF40FF;  // magenta: line to the lock target

			const auto  base = a_actor->GetPosition();
			const float length = settings.lockArrowLength;
			const float thick = settings.lockThickness;
			const auto  at = [&](float a_z) { return RE::NiPoint3{ base.x, base.y, base.z + a_z }; };

			DebugDraw::Arrow(at(95.0f), at(95.0f) + Direction(a_wanted) * length, a_stop ? Pinned : Wanted, thick);
			DebugDraw::Arrow(at(100.0f), at(100.0f) + Direction(a_actor->data.angle.z) * (length * 0.85f), Heading, thick);
			if (settings.lockDrawBody && a_body) {
				DebugDraw::Arrow(at(105.0f), at(105.0f) + Direction(*a_body) * (length * 0.7f), Body, thick);
			}
			if (a_target && !a_stop) {
				const auto t = a_target->GetPosition();
				DebugDraw::Line(at(95.0f), RE::NiPoint3{ t.x, t.y, t.z + 95.0f }, ToTarget, 1.0f);
			}
		}

		// the frame time comes from RE::GetSecondsSinceLastFrame(): Character::Update's argument is not one
		void ApplyRotation(RE::Actor* a_actor, [[maybe_unused]] float a_delta)
		{
			ActorState snapshot;
			{
				std::scoped_lock l{ lock };
				const auto       it = actors.find(a_actor->GetFormID());
				if (it != actors.end()) {
					it->second.engineWanted.reset();  // set again below while the lock is on
					it->second.lockActive = false;
				}
				if (it != actors.end() && it->second.HasEffect()) {
					if ((it->second.flags & kStop) && !it->second.stopYaw) {
						it->second.stopYaw = a_actor->data.angle.z;
					}
					snapshot.flags = it->second.flags;
					snapshot.legacy = it->second.legacy;
					snapshot.legacyMaxAngle = it->second.legacyMaxAngle;
					snapshot.legacyTurnSpeed = it->second.legacyTurnSpeed;
					snapshot.legacyStrength = it->second.legacyStrength;
					snapshot.stopYaw = it->second.stopYaw;
					snapshot.strength = it->second.strength;
				}
			}
			if (!snapshot.HasEffect()) {
				// no SDS flag: the plugin changes nothing, but the debug still watches NPCs fighting the player
				const auto& settings = Settings::Get();
				const auto  combatTarget = a_actor->GetActorRuntimeData().currentCombatTarget.get();
				const auto  player = RE::PlayerCharacter::GetSingleton();
				if (settings.combatDebug && combatTarget && combatTarget.get() == player && !a_actor->IsDead() && a_actor->Is3DLoaded()) {
					const auto  from = a_actor->GetPosition();
					const auto  to = player->GetPosition();
					const float wanted = std::atan2(to.x - from.x, to.y - from.y);
					const float turn = WrapPi(wanted - a_actor->data.angle.z);
					LogMode(a_actor, Mode::kCombat, 0, false, player, turn);
					const auto body = BodyHeading(a_actor);
					Record(a_actor, wanted, turn, turn, body);
					DrawLockDebug(a_actor, wanted, false, player, body);
				} else {
					LogMode(a_actor, Mode::kNone, 0, false, nullptr, 0.0f);
				}
				return;
			}

			const auto log = [&](Mode a_mode, RE::Actor* a_target = nullptr, float a_turn = 0.0f) {
				LogMode(a_actor, a_mode, snapshot.flags, snapshot.legacy, a_target, a_turn);
			};

			if (a_actor->IsDead() || a_actor->IsInKillMove() || a_actor->IsInRagdollState()) {
				log(Mode::kSuspended);
				return;
			}
			const auto& settings = Settings::Get();
			const float current = a_actor->data.angle.z;

			if (snapshot.flags & kStop) {
				log(Mode::kStop);
				if (!snapshot.stopYaw) {
					return;
				}
				const float before = WrapPi(*snapshot.stopYaw - current);
				if (settings.holdHeading && std::abs(before) > 1e-4f) {
					a_actor->SetHeading(*snapshot.stopYaw);
				}
				const auto body = BodyHeading(a_actor);
				Record(a_actor, *snapshot.stopYaw, before, WrapPi(*snapshot.stopYaw - a_actor->data.angle.z), body);
				DrawLockDebug(a_actor, *snapshot.stopYaw, true, nullptr, body);
				return;
			}
			if (snapshot.flags & kOff) {
				log(Mode::kOff);
				return;
			}

			const auto target = ResolveTarget(a_actor);
			if (!target) {
				const bool fighting = a_actor->GetActorRuntimeData().currentCombatTarget.get() != nullptr;
				log(fighting ? Mode::kLockNotPlayer : Mode::kLockNoTarget);
				return;
			}
			const auto  from = a_actor->GetPosition();
			const auto  to = target->GetPosition();
			const float dx = to.x - from.x;
			const float dy = to.y - from.y;
			const float wanted = std::atan2(dx, dy);  // Skyrim heading: 0 = +Y, clockwise
			const float turn = WrapPi(wanted - current);
			if (settings.maxDistance > 0.0f && dx * dx + dy * dy > settings.maxDistance * settings.maxDistance) {
				log(Mode::kLockTooFar, target, turn);
				return;
			}

			// turn limit in degrees per second; negative = full force (face the target as fast as the engine allows)
			float limit = settings.turnSpeed > 0.0f ? settings.turnSpeed : -1.0f;
			if (!(snapshot.flags & kOn)) {  // only the TCB session is asking
				if (std::abs(turn) > ToRadians(snapshot.legacyMaxAngle)) {
					log(Mode::kLockOutOfCone, target, turn);
					return;
				}
				limit = snapshot.legacyTurnSpeed * std::clamp(snapshot.legacyStrength, 0.0f, 100.0f) / 100.0f;
			} else if (snapshot.strength) {
				// strength from the annotation: 1 = full force, 0 = no turning at all (AI turning off too), between: that share
				// of fMaxEngineTurnSpeed
				limit = *snapshot.strength >= 0.999f ? -1.0f : *snapshot.strength * settings.maxEngineTurnSpeed;
			}
			log(Mode::kLock, target, turn);
			{
				std::scoped_lock l{ lock };
				if (const auto it = actors.find(a_actor->GetFormID()); it != actors.end()) {
					it->second.lockActive = true;
				}
			}

			// iMethod = 1: the AI turn-speed store (TurnSpeedOverride) turns the NPC on the next frame. Setting the heading
			// directly instead leaves the root motion in a stale frame that even turns the other way: the NPC slides sideways.
			// If the store does not run for this NPC, SetHeading takes over so the lock still works.
			bool setHeading = settings.lockMethod == 0;
			if (!setHeading) {
				bool announce = false;
				{
					std::scoped_lock l{ lock };
					if (const auto it = actors.find(a_actor->GetFormID()); it != actors.end()) {
						auto& state = it->second;
						state.engineWanted = wanted;
						state.engineLimit = ToRadians(limit >= 0.0f ? limit : settings.maxEngineTurnSpeed);
						setHeading = ++state.framesSinceAiCall > 10;
						if (setHeading && !state.fallbackLogged) {
							state.fallbackLogged = true;
							announce = true;
						}
					}
				}
				if (announce) {
					logs::warn("{} [{:08X}] the AI turn speed is not being set during this lock: turning with SetHeading instead "
							   "(root motion may drift sideways)",
						a_actor->GetName(), a_actor->GetFormID());
				}
			}
			if (setHeading) {
				float step = turn;
				if (limit >= 0.0f) {
					const float frameLimit = ToRadians(limit) * RE::GetSecondsSinceLastFrame();
					step = std::clamp(turn, -frameLimit, frameLimit);
				}
				if (std::abs(step) > 1e-5f) {
					a_actor->SetHeading(current + step);
				}
			}

			const auto body = BodyHeading(a_actor);
			Record(a_actor, wanted, turn, WrapPi(wanted - a_actor->data.angle.z), body);
			DrawLockDebug(a_actor, wanted, false, target, body);
		}
	}

	namespace
	{
		// SDS_NPC_ReachTarget.ref|min|max|arrive (game units): the forward root motion of the window is scaled so the attack
		// ends `arrive` units from the target: k = clamp(distance - arrive, min, max) / ref, measured once, when the
		// annotation fires (a target that moves afterwards can dodge it). Returns a line for the log.
		std::string StartReach(RE::Actor* a_actor, ActorState& a_state, std::string_view a_payload)
		{
			float       fields[4]{ 0.0f, 0.0f, -1.0f, 0.0f };  // ref, min, max (-1: = ref), arrive
			std::size_t i = 0;
			for (auto rest = a_payload; i < 4 && !rest.empty(); ++i) {
				const auto bar = rest.find('|');
				if (const auto value = ParseFloat(rest.substr(0, bar))) {
					fields[i] = *value;
				}
				rest = bar == std::string_view::npos ? std::string_view{} : rest.substr(bar + 1);
			}
			// ref / min / max are animmotion distances: the engine plays them scaled with the NPC (a boss at 1.65 covers
			// 1.65 times the annotated path), so they are scaled the same way; arrive is a distance in the world
			float scale = 1.0f;
			if (const auto root = a_actor->Get3D(false)) {
				scale = root->world.scale;
			}
			if (!(scale > 0.05f && scale < 20.0f)) {
				scale = 1.0f;
			}
			const float ref = fields[0] * scale;
			const float minDist = (std::max)(0.0f, fields[1]) * scale;
			const float maxDist = (fields[2] < 0.0f ? fields[0] : fields[2]) * scale;
			const float arrive = (std::max)(0.0f, fields[3]);
			if (ref <= 0.0f) {
				a_state.reachScale.reset();
				return std::format("ignored, no authored distance in '{}' (SDS_NPC_ReachTarget.ref|min|max|arrive)", a_payload);
			}
			const auto target = ResolveTarget(a_actor);
			if (!target) {
				a_state.reachScale.reset();
				return "no target, root motion unchanged";
			}
			const auto  from = a_actor->GetPosition();
			const auto  to = target->GetPosition();
			const float distance = std::hypot(to.x - from.x, to.y - from.y);
			const float wanted = std::clamp(distance - arrive, minDist, (std::max)(minDist, maxDist));
			const auto& settings = Settings::Get();
			const float k = std::clamp(wanted / ref, settings.reachMinScale, settings.reachMaxScale);
			a_state.reachScale = k;
			return std::format("distance {:.0f}, stop {:.0f} short -> wants {:.0f} of the authored {:.0f} (range {:.0f}..{:.0f}, NPC scale {:.2f}): root motion x{:.2f}",
				distance, arrive, wanted, ref, minDist, maxDist, scale, k);
		}
	}

	void OnClipActivate(RE::Actor* a_actor, const RE::hkbClipGenerator* a_clip, const RE::hkbCharacter* a_character)
	{
		// nothing is read from the clip here: while a save loads, clips are activated before their data is usable
		std::scoped_lock l{ lock };
		auto&            state = actors[a_actor->GetFormID()];
		if (state.character != a_character) {  // a new graph (3D reloaded, save loaded): forget the old one
			state.character = a_character;
			state.active.clear();
			state.ResetFlags();
			state.owner = nullptr;
		}
		if (state.owner == a_clip) {  // the same clip played again
			state.ResetFlags();
			state.owner = nullptr;
		}
		std::erase_if(state.active, [a_clip](const ActiveClip& c) { return c.clip == a_clip; });
		if (state.active.size() >= MaxActiveClips) {
			state.active.erase(state.active.begin());
		}
		state.active.push_back({ a_clip, ++activations });
	}

	void OnClipDeactivate(RE::Actor* a_actor, const RE::hkbClipGenerator* a_clip)
	{
		std::scoped_lock l{ lock };
		const auto       it = actors.find(a_actor->GetFormID());
		if (it == actors.end()) {
			return;
		}
		auto& state = it->second;
		std::erase_if(state.active, [a_clip](const ActiveClip& c) { return c.clip == a_clip; });
		if (state.owner == a_clip) {  // the clip that set the flags is over
			state.ResetFlags();
			state.owner = nullptr;
		}
		if (state.active.empty() && !state.owner) {
			actors.erase(it);
		}
	}

	void OnGraphEvent(RE::Actor* a_actor, std::string_view a_tag, std::string_view a_payload)
	{
		CsvEvent(a_actor, a_tag);
		const auto tag = TagFromName(a_tag);
		if (tag == kNone) {
			return;
		}
		const auto& settings = Settings::Get();
		if ((tag == kLegacyStart || tag == kLegacyStop) && !settings.legacyTCB) {
			return;
		}

		std::scoped_lock l{ lock };
		auto&            state = actors[a_actor->GetFormID()];

		// read the annotations of clips that are playing right now (the event comes from one of them)
		for (auto& c : state.active) {
			if (!c.scanned) {
				c.tags = ScanAnnotations(c.clip);
				c.scanned = true;
			}
		}

		// the clip that sent it: newest active clip that carries this annotation, then newest unreadable one, then newest
		const RE::hkbClipGenerator* source = nullptr;
		const auto                  newestWith = [&](std::uint16_t a_want) {
            std::uint64_t newest = 0;
            for (const auto& c : state.active) {
                if ((a_want == 0 || (c.tags & a_want)) && c.order >= newest) {
                    newest = c.order;
                    source = c.clip;
                }
            }
            return source != nullptr;
		};
		newestWith(tag) || newestWith(kUnknown) || newestWith(0);
		if (source != state.owner) {  // another clip took over: whatever the previous one set is gone
			state.ResetFlags();
			state.owner = source;
		}

		const auto payload = PayloadOf(a_tag, a_payload);
		std::string reachNote;
		switch (tag) {
		case kReset:
			state.ResetFlags();
			break;
		case kOn:
		case kTurnSpeed:
			state.flags |= kOn;
			if (const auto value = ParseFloat(payload)) {
				state.strength = std::clamp(*value, 0.0f, 1.0f);
			} else if (tag == kTurnSpeed) {
				logs::warn("{} [{:08X}] SDS_NPC_TurnSpeed without a number (0..1): '{}'", a_actor->GetName(), a_actor->GetFormID(), payload);
			}
			break;
		case kOff:
		case kStop:
			state.flags |= tag;
			break;
		case kReach:
			reachNote = StartReach(a_actor, state, payload);
			break;
		case kReachEnd:
			state.reachScale.reset();
			reachNote = "ended";
			break;
		case kBumpOff:
			state.bumpOff = true;
			break;
		case kBumpOn:
			state.bumpOff = false;
			break;
		case kLegacyStart:
			state.legacy = true;
			ParseLegacyPayload(state, a_payload);
			break;
		case kLegacyStop:
			state.legacy = false;
			break;
		default:
			break;
		}

		if (settings.logAnnotations) {
			logs::info("{} [{:08X}] annotation {}{}{} -> flags on={} off={} stop={} tcb={} strength={} | source clip {} ({} active)",
				a_actor->GetName(), a_actor->GetFormID(), TagName(tag), payload.empty() ? "" : " payload ", payload,
				(state.flags & kOn) != 0, (state.flags & kOff) != 0, (state.flags & kStop) != 0, state.legacy,
				state.strength ? std::format("{:.2f}", *state.strength) : "default"s, static_cast<const void*>(source), state.active.size());
		}
		if (!reachNote.empty() && settings.logTransitions) {
			logs::info("{} [{:08X}] reach target: {}", a_actor->GetName(), a_actor->GetFormID(), reachNote);
		}
	}

	void CsvNote(RE::Actor* a_actor, std::string_view a_text) { CsvEvent(a_actor, a_text); }

	void OnModifyMovement(RE::Actor* a_actor, RE::NiPoint3& a_translation, RE::NiPoint3& a_rotation)
	{
		const auto& settings = Settings::Get();
		bool        announce = false;
		float       before = 0.0f;
		std::string identified;  // a clip found in the animmotion catalog for the first time, for the log

		// graph variables are read before taking the lock: the graph thread holds its own while it sends events to us
		bool animationDriven = false, dodging = false;
		if (settings.animMotion) {
			a_actor->GetGraphVariableBool("bAnimationDriven"sv, animationDriven);
			a_actor->GetGraphVariableBool("RDF_IsDodging"sv, dodging);  // Reactive Dodge Framework moves its dodges itself
		}
		{
			std::scoped_lock l{ lock };
			const auto       it = actors.find(a_actor->GetFormID());
			if (it == actors.end()) {
				return;
			}
			auto& state = it->second;
			++state.mmCalls;

			// [AnimMotion] the direction of the step from the clip's own animmotion path, relative to where the NPC faces
			// now (as Elden Ring applies its root-motion track). The engine's direction is the part that goes wrong
			// (20-125 deg off while an NPC turns); its length is right and is kept.
			bool steered = false;
			if (settings.animMotion && animationDriven && !dodging && AnimCatalog::Ready()) {
				ActiveClip* driver = nullptr;
				for (auto& c : state.active) {
					if (!c.curveResolved) {
						const auto key = ClipUtil::AnimationKey(c.clip);
						c.curveResolved = key.has_value() || ++c.keyTries >= 30;  // a clip binds its animation on its first updates
						if (key) {
							c.curve = AnimCatalog::Find(*key);
							if (c.curve && settings.logTransitions) {
								identified = std::format("{} ({} motion keys{})", c.curve->file, c.curve->moveTime.size(),
									c.curve->rotTime.empty() ? "" : std::format(", {} rotation keys", c.curve->rotTime.size()));
							}
						}
					}
					if (c.curve && (!driver || c.order > driver->order)) {
						driver = &c;
					}
				}
				const auto motion = driver ? ClipUtil::ReadClipMotion(driver->clip) : std::nullopt;
				if (driver && motion) {
					const float now = motion->localTime;
					if (state.animClip == driver->clip && now >= state.animPrevTime - 1e-4f) {
						const auto  from = driver->curve->At(state.animPrevTime);
						const auto  to = driver->curve->At(now);
						const float dx = to.x - from.x;
						const float dy = to.y - from.y;
						const float length = std::hypot(a_translation.x, a_translation.y);
						if (std::hypot(dx, dy) > settings.animMotionMinStep && length > 1e-3f) {
							// animmotion is in the frame the clip started in; animrotation turns the NPC since then
							const float angle = std::atan2(dx, dy) - ToRadians(to.rot * settings.animRotationSign);
							a_translation.x = length * std::sin(angle);
							a_translation.y = length * std::cos(angle);
							++state.mmAnim;
							steered = true;
						}
					}
					state.animClip = driver->clip;
					state.animPrevTime = now;
				} else {
					state.animClip = nullptr;
				}
			}

			// the lock faces the target: a root-motion step that goes sideways is turned to go straight ahead (same length).
			// Steps that go backwards (more than fStraightMaxAngle off) are left as they are.
			const float length = std::hypot(a_translation.x, a_translation.y);
			const float angle = std::atan2(a_translation.x, a_translation.y);
			if (!steered && settings.straightRootMotion && state.lockActive && length > 1e-3f && std::abs(angle) > ToRadians(1.0f) &&
				std::abs(angle) < ToRadians(settings.straightMaxAngle)) {
				before = angle;
				a_translation.x = 0.0f;
				a_translation.y = length;
				++state.mmStraightened;
				if (!state.straightLogged) {
					state.straightLogged = true;
					announce = true;
				}
			}
			// SDS_NPC_ReachTarget: steps that go forward are stretched / shortened (the whole step, so its direction stays);
			// stepping back is never touched
			if (state.reachScale && a_translation.y > 0.0f) {
				a_translation.x *= *state.reachScale;
				a_translation.y *= *state.reachScale;
				++state.mmReached;
			}
			state.mmX += a_translation.x;
			state.mmY += a_translation.y;
			state.mmRotZ += a_rotation.z;
		}
		if (!identified.empty()) {
			logs::info("{} [{:08X}] root motion from the animation: {}", a_actor->GetName(), a_actor->GetFormID(), identified);
		}
		if (announce && settings.logTransitions) {
			logs::info("{} [{:08X}] lock: root motion straightened (it went {:+.0f} deg off ahead)", a_actor->GetName(), a_actor->GetFormID(),
				ToDegrees(before));
		}
	}

	// SDS_NPC_BumpOff / BumpOn: Animation Motion Fix reads the graph variable AMF_bForbidMoveMagnetism (with its
	// DisableNpcMovementMagnetism = false): true = the engine's attack movement magnetism is off, so the NPC does not
	// stop dead on its target's collision, and AMF also stops it from pushing the target. BFCO adds the variable to
	// the humanoid graph and sets it in a window of its own, so this only writes false back when it wrote the true.
	void UpdateBump(RE::Actor* a_actor)
	{
		bool wanted = false, applied = false;
		{
			std::scoped_lock l{ lock };
			if (const auto it = actors.find(a_actor->GetFormID()); it != actors.end()) {
				wanted = it->second.bumpOff && Settings::Get().bumpAnnotations;
			}
			applied = bumpApplied.contains(a_actor->GetFormID());
		}
		if (!wanted && !applied) {
			return;
		}
		static constexpr auto variable = "AMF_bForbidMoveMagnetism"sv;
		bool                  current = false;
		const bool            exists = a_actor->GetGraphVariableBool(variable, current);
		if (wanted) {
			if (!current) {
				if (!exists || !a_actor->SetGraphVariableBool(variable, true)) {
					static std::atomic<bool> warned{ false };
					if (!warned.exchange(true)) {
						logs::warn("{} [{:08X}] SDS_NPC_BumpOff: its graph has no AMF_bForbidMoveMagnetism (BFCO adds it to the humanoid graph)",
							a_actor->GetName(), a_actor->GetFormID());
					}
					return;
				}
				if (Settings::Get().logTransitions) {
					logs::info("{} [{:08X}] bump off: movement magnetism disabled (passes the target instead of stopping on it)",
						a_actor->GetName(), a_actor->GetFormID());
				}
			}
			std::scoped_lock l{ lock };
			bumpApplied.insert(a_actor->GetFormID());
		} else {
			if (current) {
				a_actor->SetGraphVariableBool(variable, false);
			}
			if (Settings::Get().logTransitions) {
				logs::info("{} [{:08X}] bump on: movement magnetism back", a_actor->GetName(), a_actor->GetFormID());
			}
			std::scoped_lock l{ lock };
			bumpApplied.erase(a_actor->GetFormID());
		}
	}

	void OnUpdate(RE::Actor* a_actor, float a_delta)
	{
		UpdateBump(a_actor);
		ApplyRotation(a_actor, a_delta);
	}

	std::optional<float> TurnSpeedOverride(RE::Actor* a_actor, float a_aiSpeed)
	{
		std::scoped_lock l{ lock };
		const auto       it = actors.find(a_actor->GetFormID());
		if (it == actors.end()) {
			return std::nullopt;
		}
		auto& state = it->second;
		++state.aiCalls;
		state.aiIn = a_aiSpeed;
		state.framesSinceAiCall = 0;

		std::optional<float> speed;
		if (state.flags & kStop) {
			if (Settings::Get().zeroAIRotation) {
				speed = 0.0f;
			}
		} else if (state.engineWanted) {
			const float seconds = (std::max)(RE::GetSecondsSinceLastFrame(), 1.0f / 240.0f);
			const float turn = WrapPi(*state.engineWanted - a_actor->data.angle.z);
			speed = std::clamp(turn / seconds, -state.engineLimit, state.engineLimit);
		}
		state.aiOut = speed.value_or(a_aiSpeed);
		return speed;
	}

	void Clear()
	{
		std::scoped_lock l{ lock };
		actors.clear();
		records.clear();
	}
}
