#include "MotionDebug.h"

#include "ClipUtil.h"
#include "DebugDraw.h"
#include "Settings.h"

namespace SDS::MotionDebug
{
	namespace
	{
		constexpr std::uint32_t Green = 0x00FF00;
		constexpr std::uint32_t Yellow = 0xFFFF00;
		constexpr std::uint32_t Cyan = 0x00FFFF;
		constexpr std::uint32_t Orange = 0xFF8000;
		constexpr std::uint32_t Blue = 0x4080FF;
		constexpr std::uint32_t Magenta = 0xFF00FF;
		constexpr std::uint32_t White = 0xFFFFFF;

		std::ofstream csv;
		std::mutex    csvLock;

		struct MotionKey
		{
			float        time;
			RE::NiPoint3 offset;  // cumulative, clip-start local space: x right, y forward, z up
		};

		struct RotationKey
		{
			float time;
			float degrees;  // cumulative
		};

		// Everything the actor update needs from a clip is copied here by the clip's own hooks, which run while
		// the clip is alive. The actor update (a job thread) never touches the clip's memory: a stored clip
		// pointer is only an identity.
		struct ActiveClip
		{
			const RE::hkbClipGenerator* clip;
			std::uint64_t               order;
			std::string                 animation;
			bool                        parsed{ false };
			int                         unboundUpdates{ 0 };
			std::vector<MotionKey>      motion;
			std::vector<RotationKey>    rotation;
			float                       time{ 0.0f };
		};

		std::unordered_map<RE::FormID, std::string> skipReasons;  // last logged reason a watched clip was ignored, per actor

		struct Session
		{
			const RE::hkbClipGenerator* clip{ nullptr };
			std::uint64_t               order{ 0 };
			std::string                 animation;
			std::vector<MotionKey>      motion;
			std::vector<RotationKey>    rotation;

			RE::NiPoint3  start;
			float         startHeading{ 0.0f };
			float         startYawToTarget{ 0.0f };
			float         lastTime{ 0.0f };
			RE::NiPoint3  lastOffset;
			float         elapsed{ 0.0f };
			std::uint32_t frames{ 0 };

			RE::NiPoint3 actual;  // last drawn points
			RE::NiPoint3 clipFrame;
			RE::NiPoint3 currentFrame;
			RE::NiPoint3 rootFrame;
			RE::NiPoint3 ideal;

			float maxDevClip{ 0.0f };
			float maxDevCurrent{ 0.0f };
			float maxDevRoot{ 0.0f };
			float maxDevIdeal{ 0.0f };
			float lastHeading{ 0.0f };
			float startRoot{ 0.0f };
			float lastRoot{ 0.0f };
			float lastYawToTarget{ 0.0f };
		};

		struct ActorData
		{
			std::vector<ActiveClip> active;
			std::optional<Session>  session;
		};

		std::mutex                                lock;
		std::unordered_map<RE::FormID, ActorData> actors;
		std::uint64_t                             activations{ 0 };

		constexpr std::size_t MaxActiveClips = 64;
		constexpr float       Pi = std::numbers::pi_v<float>;

		float WrapPi(float a)
		{
			while (a > Pi) a -= 2.0f * Pi;
			while (a < -Pi) a += 2.0f * Pi;
			return a;
		}

		float Deg(float a_radians) { return a_radians * 180.0f / Pi; }

		// local (x right, y forward) to world for a Skyrim heading (0 = +Y, clockwise)
		RE::NiPoint3 ToWorld(const RE::NiPoint3& a_local, float a_heading)
		{
			const float s = std::sin(a_heading);
			const float c = std::cos(a_heading);
			return { a_local.x * c + a_local.y * s, -a_local.x * s + a_local.y * c, a_local.z };
		}

		float Distance2D(const RE::NiPoint3& a, const RE::NiPoint3& b) { return std::hypot(a.x - b.x, a.y - b.y); }

		RE::NiPoint3 SampleMotion(const std::vector<MotionKey>& a_keys, float a_time)
		{
			MotionKey previous{ 0.0f, {} };
			for (const auto& key : a_keys) {
				if (a_time <= key.time) {
					const float span = key.time - previous.time;
					const float f = span > 1e-6f ? (a_time - previous.time) / span : 1.0f;
					return previous.offset + (key.offset - previous.offset) * std::clamp(f, 0.0f, 1.0f);
				}
				previous = key;
			}
			return previous.offset;
		}

		float SampleRotation(const std::vector<RotationKey>& a_keys, float a_time)
		{
			RotationKey previous{ 0.0f, 0.0f };
			for (const auto& key : a_keys) {
				if (a_time <= key.time) {
					const float span = key.time - previous.time;
					const float f = span > 1e-6f ? (a_time - previous.time) / span : 1.0f;
					return previous.degrees + (key.degrees - previous.degrees) * std::clamp(f, 0.0f, 1.0f);
				}
				previous = key;
			}
			return previous.degrees;
		}

		std::vector<float> ParseFloats(std::string_view a_text)
		{
			std::vector<float> values;
			while (!a_text.empty()) {
				const auto first = a_text.find_first_not_of(' ');
				if (first == std::string_view::npos) {
					break;
				}
				a_text.remove_prefix(first);
				const auto end = a_text.find(' ');
				const auto part = a_text.substr(0, end);
				float      value{};
				if (std::from_chars(part.data(), part.data() + part.size(), value).ec == std::errc{}) {
					values.push_back(value);
				}
				a_text = end == std::string_view::npos ? std::string_view{} : a_text.substr(end);
			}
			return values;
		}

		// only from the clip's Update hook: the clip is playing there
		void ParseClip(ActiveClip& a_entry, const RE::hkbClipGenerator* a_clip)
		{
			static thread_local std::vector<ClipUtil::RawAnnotation> raw(1024);
			const int count = ClipUtil::CopyAnnotations(a_clip, raw.data(), static_cast<int>(raw.size()));
			if (count == ClipUtil::kNotBound) {
				if (++a_entry.unboundUpdates == 60 && Settings::Get().diagnostics) {
					logs::info("[clip] '{}': no animation binding after 60 updates", ClipUtil::AnimationName(a_clip));
				}
				return;  // the next clip update tries again
			}
			a_entry.parsed = true;
			a_entry.animation = ClipUtil::AnimationName(a_clip);
			if (count < 0) {
				static std::atomic<int> dumped{ 0 };
				if (Settings::Get().diagnostics) {
					logs::info("[clip] '{}': reading the annotations faulted", a_entry.animation);
					if (dumped++ < 6) {
						logs::info("[clip]   {}", ClipUtil::DumpClip(a_clip));
					}
				}
				return;  // unreadable: treated as a clip without animmotion
			}
			if (Settings::Get().diagnostics) {
				std::string sds;
				int         motion = 0;
				for (int i = 0; i < count; ++i) {
					const std::string_view text = raw[i].text;
					if (text.starts_with("animmotion")) {
						++motion;
					} else if (!text.starts_with("anim")) {
						sds += std::format(" {}@{:.2f}", text, raw[i].time);
					}
				}
				if (motion > 0 || !sds.empty()) {  // clips without either are not interesting
					logs::info("[clip] '{}': {} animmotion,{}", a_entry.animation, motion, sds.empty() ? " no SDS_NPC_*" : sds);
				}
			}
			for (int i = 0; i < count; ++i) {
				const std::string_view text = raw[i].text;
				if (text.starts_with("animmotion ")) {
					const auto v = ParseFloats(text.substr(11));
					if (v.size() >= 3) {
						a_entry.motion.push_back({ raw[i].time, { v[0], v[1], v[2] } });
					}
				} else if (text.starts_with("animrotation ")) {
					const auto v = ParseFloats(text.substr(13));
					if (!v.empty()) {
						a_entry.rotation.push_back({ raw[i].time, v[0] });
					}
				}
			}
			std::ranges::sort(a_entry.motion, {}, &MotionKey::time);
			std::ranges::sort(a_entry.rotation, {}, &RotationKey::time);
		}

		RE::Actor* TargetOf(RE::Actor* a_actor)
		{
			const auto combatTarget = a_actor->GetActorRuntimeData().currentCombatTarget.get();
			return combatTarget ? combatTarget.get() : RE::PlayerCharacter::GetSingleton();
		}

		bool Watched(RE::Actor* a_actor)
		{
			const auto& settings = Settings::Get();
			const auto  player = RE::PlayerCharacter::GetSingleton();
			if (!player || a_actor->IsDead() || !a_actor->IsInCombat()) {
				return false;
			}
			if (settings.motionOnlyFightingPlayer && a_actor->GetActorRuntimeData().currentCombatTarget.get().get() != player) {
				return false;
			}
			return settings.motionMaxDistance <= 0.0f ||
			       a_actor->GetPosition().GetDistance(player->GetPosition()) <= settings.motionMaxDistance;
		}

		float YawTo(const RE::NiPoint3& a_from, const RE::NiPoint3& a_to) { return std::atan2(a_to.x - a_from.x, a_to.y - a_from.y); }

		void Line(const RE::NiPoint3& a_from, const RE::NiPoint3& a_to, std::uint32_t a_color, float a_lift)
		{
			const auto& settings = Settings::Get();
			if (!settings.motionDraw) {
				return;
			}
			const RE::NiPoint3 lift{ 0.0f, 0.0f, a_lift };
			DebugDraw::Line(a_from + lift, a_to + lift, a_color, settings.motionThickness, settings.motionPersistSeconds);
		}

		void FinishSession(RE::Actor* a_actor, Session& a_session)
		{
			const auto& settings = Settings::Get();
			const auto  end = a_session.actual;
			const auto  intended = SampleMotion(a_session.motion, a_session.lastTime);
			const float devClip = Distance2D(end, a_session.clipFrame);
			const float devCurrent = Distance2D(end, a_session.currentFrame);
			const float devRoot = Distance2D(end, a_session.rootFrame);
			const float devIdeal = Distance2D(end, a_session.ideal);

			const char* closest = "clip frame (motion locked to the heading at clip start)";
			float       best = devClip;
			if (devCurrent < best) {
				best = devCurrent;
				closest = "heading frame (motion follows the NPC's heading)";
			}
			if (devRoot < best) {
				best = devRoot;
				closest = "root frame (motion follows where NPC Root faces)";
			}
			if (devIdeal < best) {
				closest = "ideal (NPC kept facing the target)";
			}

			logs::info(
				"[motion] {} [{:08X}] '{}' {:.2f}s {} frames, {} motion keys, {} rotation keys | annotated distance {:.0f}, actual {:.0f} | "
				"end deviation: clip {:.0f}, heading {:.0f}, root {:.0f}, ideal {:.0f} | max deviation: clip {:.0f}, heading {:.0f}, root {:.0f}, ideal {:.0f} | "
				"heading {:.0f} -> {:.0f} (turned {:+.0f}), NPC Root {:.0f} -> {:.0f} (turned {:+.0f}), animrotation {:+.0f}, yaw to target {:.0f} -> {:.0f} | "
				"closest: {}",
				a_actor->GetName(), a_actor->GetFormID(), a_session.animation, a_session.elapsed, a_session.frames, a_session.motion.size(),
				a_session.rotation.size(), std::hypot(intended.x, intended.y), Distance2D(end, a_session.start), devClip, devCurrent, devRoot,
				devIdeal, a_session.maxDevClip, a_session.maxDevCurrent, a_session.maxDevRoot, a_session.maxDevIdeal, Deg(a_session.startHeading),
				Deg(a_session.lastHeading), Deg(WrapPi(a_session.lastHeading - a_session.startHeading)), Deg(a_session.startRoot),
				Deg(a_session.lastRoot), Deg(WrapPi(a_session.lastRoot - a_session.startRoot)), SampleRotation(a_session.rotation, a_session.lastTime),
				Deg(a_session.startYawToTarget), Deg(a_session.lastYawToTarget), closest);

			if (settings.motionDraw) {
				DebugDraw::Point(a_session.start, White, 14.0f, settings.motionPersistSeconds);
				DebugDraw::Point(end, Green, 12.0f, settings.motionPersistSeconds);
			}
		}

		void StartSession(RE::Actor* a_actor, ActorData& a_data, const ActiveClip& a_clip)
		{
			auto& session = a_data.session.emplace();
			session.clip = a_clip.clip;
			session.order = a_clip.order;
			session.animation = a_clip.animation;
			session.motion = a_clip.motion;
			session.rotation = a_clip.rotation;
			session.start = a_actor->GetPosition();
			session.startHeading = a_actor->data.angle.z;
			session.lastHeading = session.startHeading;
			session.lastTime = a_clip.time;
			session.lastOffset = SampleMotion(session.motion, session.lastTime);
			if (const auto target = TargetOf(a_actor)) {
				session.startYawToTarget = YawTo(session.start, target->GetPosition());
				session.lastYawToTarget = session.startYawToTarget;
			}
			session.startRoot = ClipUtil::RootHeading(a_actor).value_or(session.startHeading);
			session.lastRoot = session.startRoot;
			session.actual = session.clipFrame = session.currentFrame = session.rootFrame = session.ideal = session.start;
		}

		void StepSession(RE::Actor* a_actor, Session& a_session, float a_time, float a_delta)
		{
			const auto& settings = Settings::Get();
			if (a_time + 1e-4f < a_session.lastTime) {  // looped or restarted
				a_session.lastOffset = {};
			}
			const auto  offset = SampleMotion(a_session.motion, a_time);
			const float rotation = SampleRotation(a_session.rotation, a_time);
			const auto  step = offset - a_session.lastOffset;

			const auto  position = a_actor->GetPosition();
			const float heading = a_actor->data.angle.z;
			const float root = ClipUtil::RootHeading(a_actor).value_or(heading);
			const auto  target = TargetOf(a_actor);
			const float yawToTarget = target ? YawTo(position, target->GetPosition()) : heading;
			const float clipHeading = a_session.startHeading + settings.animRotationSign * rotation * Pi / 180.0f;

			const auto advance = [&](const RE::NiPoint3& a_point, float a_frameHeading) {
				const auto world = ToWorld(step, a_frameHeading);
				return RE::NiPoint3{ a_point.x + world.x, a_point.y + world.y, position.z };
			};
			const auto clipFrame = advance(a_session.clipFrame, clipHeading);
			const auto currentFrame = advance(a_session.currentFrame, heading);
			const auto rootFrame = advance(a_session.rootFrame, root);
			const auto ideal = advance(a_session.ideal, yawToTarget);

			const float z = settings.motionZOffset;
			if (settings.motionDrawActual) Line(a_session.actual, position, Green, 0.0f);
			if (settings.motionDrawClipFrame) Line(a_session.clipFrame, clipFrame, Yellow, z);
			if (settings.motionDrawCurrentFrame) Line(a_session.currentFrame, currentFrame, Cyan, 2.0f * z);
			if (settings.motionDrawRootFrame) Line(a_session.rootFrame, rootFrame, Blue, 3.0f * z);
			if (settings.motionDrawIdeal) Line(a_session.ideal, ideal, Orange, 4.0f * z);
			if (settings.motionDraw && settings.motionDrawArrows) {
				const RE::NiPoint3 chest{ position.x, position.y, position.z + 60.0f };
				DebugDraw::Arrow(chest, chest + ToWorld({ 0.0f, 90.0f, 0.0f }, heading), Cyan, settings.motionThickness);
				DebugDraw::Arrow(chest, chest + ToWorld({ 0.0f, 80.0f, 0.0f }, root), Blue, settings.motionThickness);
				DebugDraw::Arrow(chest, chest + ToWorld({ 0.0f, 100.0f, 0.0f }, yawToTarget), Magenta, settings.motionThickness);
			}

			a_session.actual = position;
			a_session.clipFrame = clipFrame;
			a_session.currentFrame = currentFrame;
			a_session.rootFrame = rootFrame;
			a_session.ideal = ideal;
			a_session.lastOffset = offset;
			a_session.lastTime = a_time;
			a_session.lastHeading = heading;
			a_session.lastRoot = root;
			a_session.lastYawToTarget = yawToTarget;
			a_session.elapsed += a_delta;
			++a_session.frames;
			a_session.maxDevClip = (std::max)(a_session.maxDevClip, Distance2D(position, clipFrame));
			a_session.maxDevCurrent = (std::max)(a_session.maxDevCurrent, Distance2D(position, currentFrame));
			a_session.maxDevRoot = (std::max)(a_session.maxDevRoot, Distance2D(position, rootFrame));
			a_session.maxDevIdeal = (std::max)(a_session.maxDevIdeal, Distance2D(position, ideal));

			if (csv.is_open()) {
				std::scoped_lock c{ csvLock };
				csv << std::format("{:08X},{},{:.4f},{:.1f},{:.1f},{:.1f},{:.2f},{:.2f},{:.2f},{:.2f},{:.2f},{:.1f},{:.1f},{:.1f},{:.1f},{:.1f},{:.1f}\n",
					a_actor->GetFormID(), a_session.animation, a_time, position.x, position.y, position.z, Deg(heading), Deg(yawToTarget),
					offset.x, offset.y, rotation, clipFrame.x, clipFrame.y, currentFrame.x, currentFrame.y, ideal.x, ideal.y);
			}
		}
	}

	void Init()
	{
		if (Settings::Get().motionDebug && Settings::Get().motionCsv) {
			if (const auto dir = logs::log_directory()) {
				csv.open(*dir / "SDSCombat_motion.csv", std::ios::trunc);
				csv << "actor,animation,clipTime,x,y,z,heading,yawToTarget,motionX,motionY,animRotation,clipFrameX,clipFrameY,currentFrameX,currentFrameY,idealX,idealY\n";
			}
		}
	}

	void OnClipActivate(RE::Actor* a_actor, const RE::hkbClipGenerator* a_clip)
	{
		if (!a_actor->IsInCombat()) {  // attack clips only; also skips OAR's main-menu dummy
			return;
		}
		// nothing is read from the clip here (see ClipUtil); OnClipUpdate parses it once it plays
		std::scoped_lock l{ lock };
		auto&            data = actors[a_actor->GetFormID()];
		std::erase_if(data.active, [a_clip](const ActiveClip& c) { return c.clip == a_clip; });
		if (data.active.size() >= MaxActiveClips) {
			data.active.erase(data.active.begin());
		}
		data.active.emplace_back(ActiveClip{ a_clip, ++activations });
	}

	void OnClipUpdate(RE::Actor* a_actor, const RE::hkbClipGenerator* a_clip)
	{
		std::scoped_lock l{ lock };
		const auto       it = actors.find(a_actor->GetFormID());
		if (it == actors.end()) {
			return;
		}
		for (auto& entry : it->second.active) {
			if (entry.clip == a_clip) {
				entry.time = a_clip->localTime;
				if (!entry.parsed) {
					ParseClip(entry, a_clip);
				}
				break;
			}
		}
	}

	void OnClipDeactivate(RE::Actor* a_actor, const RE::hkbClipGenerator* a_clip)
	{
		std::scoped_lock l{ lock };
		if (const auto it = actors.find(a_actor->GetFormID()); it != actors.end()) {
			auto& data = it->second;
			std::erase_if(data.active, [a_clip](const ActiveClip& c) { return c.clip == a_clip; });
			if (data.active.empty() && !data.session) {
				actors.erase(it);
			}
		}
	}

	void OnUpdate(RE::Actor* a_actor, float a_delta)
	{
		std::scoped_lock l{ lock };
		const auto       it = actors.find(a_actor->GetFormID());
		if (it == actors.end()) {
			return;
		}
		auto& data = it->second;

		// newest active clip that carries animmotion keys
		const ActiveClip* chosen = nullptr;
		for (const auto& entry : data.active) {
			if (!entry.motion.empty() && (!chosen || entry.order > chosen->order)) {
				chosen = &entry;
			}
		}
		if (chosen && !Watched(a_actor)) {
			if (Settings::Get().diagnostics) {
				const auto player = RE::PlayerCharacter::GetSingleton();
				const auto target = a_actor->GetActorRuntimeData().currentCombatTarget.get();
				const auto& settings = Settings::Get();
				const bool  inRange = player && (settings.motionMaxDistance <= 0.0f ||
                                                a_actor->GetPosition().GetDistance(player->GetPosition()) <= settings.motionMaxDistance);
				const auto  reason = std::format("in combat {}, combat target is the player {}, within fMaxDistance {}", a_actor->IsInCombat(),
					 target.get() == player, inRange);
				if (skipReasons[a_actor->GetFormID()] != reason) {
					skipReasons[a_actor->GetFormID()] = reason;
					logs::info("[motion] {} [{:08X}] '{}' not drawn: {}", a_actor->GetName(), a_actor->GetFormID(), chosen->animation, reason);
				}
			}
			chosen = nullptr;
		}

		if (data.session && (!chosen || chosen->clip != data.session->clip || chosen->order != data.session->order)) {
			FinishSession(a_actor, *data.session);
			data.session.reset();
		}
		if (!chosen) {
			if (data.active.empty()) {
				actors.erase(it);
			}
		} else if (!data.session) {
			StartSession(a_actor, data, *chosen);
		} else {
			StepSession(a_actor, *data.session, chosen->time, a_delta);
		}
	}

	void Clear()
	{
		std::scoped_lock l{ lock };
		actors.clear();
	}
}
