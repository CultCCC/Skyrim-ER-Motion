#include "ClipUtil.h"

namespace SDS::ClipUtil
{
	namespace
	{
		// copies a NUL-terminated string; false if it does not fit
		bool CopyText(const char* a_text, char* a_out, std::size_t a_cap)
		{
			std::size_t n = 0;
			while (n + 1 < a_cap && a_text[n] != '\0') {
				a_out[n] = a_text[n];
				++n;
			}
			a_out[n] = '\0';
			return a_text[n] == '\0';
		}

		bool StartsWith(const char* a_text, const char* a_prefix)
		{
			for (; *a_prefix; ++a_text, ++a_prefix) {
				if (*a_text != *a_prefix) {
					return false;
				}
			}
			return true;
		}

		int CopyAnnotationsUnguarded(const RE::hkbClipGenerator* a_clip, RawAnnotation* a_out, int a_cap)
		{
			const auto binding = a_clip->binding;
			const auto animation = binding ? binding->animation.get() : nullptr;
			if (!animation) {
				return kNotBound;
			}
			// one annotation track per bone (100+ on a character skeleton); the cap only rejects garbage
			const auto& tracks = animation->annotationTracks;
			if (tracks.size() < 0 || tracks.size() > 4096) {
				return 0;
			}
			int count = 0;
			for (std::int32_t t = 0; t < tracks.size(); ++t) {
				const auto& annotations = tracks[t].annotations;
				if (annotations.size() < 0 || annotations.size() > 4096) {
					return 0;
				}
				for (std::int32_t a = 0; a < annotations.size() && count < a_cap; ++a) {
					const char* text = annotations[a].text.c_str();
					if (!text || !(StartsWith(text, "anim") || StartsWith(text, "SDS_NPC_") || StartsWith(text, "Magnetism"))) {
						continue;
					}
					a_out[count].time = annotations[a].time;
					if (CopyText(text, a_out[count].text, sizeof(a_out[count].text))) {
						++count;
					}
				}
			}
			return count;
		}

		// the animation file, or the node name when the clip has none (cloned clips may not)
		bool CopyName(const RE::hkbClipGenerator* a_clip, char* a_out, std::size_t a_cap)
		{
			__try {
				a_out[0] = '\0';
				if (const char* file = a_clip->animationName.c_str(); file && file[0]) {
					CopyText(file, a_out, a_cap);
				} else if (const char* node = a_clip->name.c_str(); node && node[0]) {
					CopyText(node, a_out, a_cap);
				}
				return true;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				a_out[0] = '\0';
				return false;
			}
		}
	}

	RE::Actor* ActorFromContext(const RE::hkbContext& a_context)
	{
		if (!a_context.character) {
			return nullptr;
		}
		// BShkbAnimationGraph::characterInstance is at 0xC0 (SE); holder is at 0x210
		const auto graph = reinterpret_cast<RE::BShkbAnimationGraph*>(reinterpret_cast<std::uintptr_t>(a_context.character) - 0xC0);
		const auto holder = reinterpret_cast<RE::TESObjectREFR*>(graph->holder);
		if (!holder || !holder->Is(RE::FormType::ActorCharacter) || holder->IsPlayerRef()) {
			return nullptr;
		}
		return static_cast<RE::Actor*>(holder);
	}

	int CopyAnnotations(const RE::hkbClipGenerator* a_clip, RawAnnotation* a_out, int a_cap)
	{
		__try {
			return CopyAnnotationsUnguarded(a_clip, a_out, a_cap);
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return kFaulted;
		}
	}

	std::string AnimationName(const RE::hkbClipGenerator* a_clip)
	{
		char name[160];
		if (!CopyName(a_clip, name, sizeof(name))) {
			return "<unreadable>";
		}
		return name;
	}

	namespace
	{
		bool RootHeadingUnguarded(RE::Actor* a_actor, float& a_out)
		{
			static const RE::BSFixedString rootName{ "NPC Root [Root]" };
			const auto                     root3D = a_actor->Get3D(false);
			const auto                     bone = root3D ? root3D->GetObjectByName(rootName) : nullptr;
			if (!bone) {
				return false;
			}
			// the bone's forward (+Y) axis in world space: second column of its world rotation
			const auto& m = bone->world.rotate.entry;
			const float fx = m[0][1];
			const float fy = m[1][1];
			if (std::abs(fx) + std::abs(fy) < 1e-4f) {
				return false;
			}
			a_out = std::atan2(fx, fy);
			return true;
		}
	}

	namespace
	{
		bool ReadQword(std::uintptr_t a_address, std::uint64_t& a_out)
		{
			__try {
				a_out = *reinterpret_cast<const std::uint64_t*>(a_address);
				return true;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}

		std::string Qword(std::uintptr_t a_address)
		{
			std::uint64_t value = 0;
			return ReadQword(a_address, value) ? std::format("{:X}", value) : "<fault>";
		}
	}

	std::string DumpClip(const RE::hkbClipGenerator* a_clip)
	{
		const auto    base = reinterpret_cast<std::uintptr_t>(a_clip);
		std::uint64_t binding = 0, animation = 0, bindingIndex = 0;
		ReadQword(base + 0xA0, binding);
		ReadQword(base + 0x70, bindingIndex);
		std::string text = std::format("clip {:X}: vtable {} name@38 {} animationName@48 {} bindingIndex@70 {} binding@A0 {:X}", base,
			Qword(base), Qword(base + 0x38), Qword(base + 0x48), bindingIndex & 0xFFFF, binding);
		if (binding && ReadQword(binding + 0x18, animation)) {
			text += std::format(" | binding: vtable {} animation@18 {:X}", Qword(binding), animation);
			if (animation) {
				text += std::format(" | animation: vtable {} type@10 {} tracks.data@28 {} tracks.size@30 {}", Qword(animation),
					Qword(animation + 0x10), Qword(animation + 0x28), Qword(animation + 0x30));
				std::uint64_t tracks = 0;
				if (ReadQword(animation + 0x28, tracks) && tracks) {
					text += std::format(" | track[0]: name {} annotations.data {} size {}", Qword(tracks), Qword(tracks + 0x8), Qword(tracks + 0x10));
				}
			}
		}
		return text;
	}

	namespace
	{
		// same offsets as in the .hkx packfile (AnimCatalog::Parse): hkaAnimation type 0x10, transform tracks 0x18;
		// spline: numFrames 0x38, numBlocks 0x3C, data 0x98 / size 0xA0; interleaved: transforms 0x38 / count 0x40
		bool AnimationKeyUnguarded(const RE::hkbClipGenerator* a_clip, AnimCatalog::Key& a_key)
		{
			const auto binding = a_clip->binding;
			const auto animation = binding ? binding->animation.get() : nullptr;
			if (!animation) {
				return false;
			}
			const auto base = reinterpret_cast<const std::uint8_t*>(animation);
			const auto i32 = [base](std::size_t a_off) { return *reinterpret_cast<const std::int32_t*>(base + a_off); };
			const auto ptr = [base](std::size_t a_off) { return *reinterpret_cast<const std::uint8_t* const*>(base + a_off); };
			switch (i32(0x10)) {
			case 5:  // spline compressed
				{
					const std::int32_t size = i32(0xA0);
					const auto         data = ptr(0x98);
					if (size < 0 || (size > 0 && !data)) {
						return false;
					}
					a_key = { 'S', i32(0x38), i32(0x3C), size, AnimCatalog::Crc32(data, std::size_t((std::min)(256, size))) };
					return true;
				}
			case 1:  // interleaved
				{
					const std::int32_t count = i32(0x40);
					const auto         data = ptr(0x38);
					if (count < 0 || (count > 0 && !data)) {
						return false;
					}
					const std::size_t bytes = std::size_t(count) * 48;
					a_key = { 'I', i32(0x18), count, 0, AnimCatalog::Crc32(data, (std::min)(std::size_t(256), bytes)) };
					return true;
				}
			default:
				return false;
			}
		}
	}

	std::optional<AnimCatalog::Key> AnimationKey(const RE::hkbClipGenerator* a_clip)
	{
		if (!a_clip) {
			return std::nullopt;
		}
		AnimCatalog::Key key;
		bool             ok = false;
		__try {
			ok = AnimationKeyUnguarded(a_clip, key);
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			ok = false;
		}
		return ok ? std::optional<AnimCatalog::Key>(key) : std::nullopt;
	}

	std::optional<ClipMotion> ReadClipMotion(const RE::hkbClipGenerator* a_clip)
	{
		if (!a_clip) {
			return std::nullopt;
		}
		ClipMotion out{};
		bool       ok = false;
		__try {
			const auto& t = a_clip->extractedMotion;
			out.localTime = a_clip->localTime;
			out.x = t.translation.quad.m128_f32[0];
			out.y = t.translation.quad.m128_f32[1];
			const float qz = t.rotation.vec.quad.m128_f32[2];
			const float qw = t.rotation.vec.quad.m128_f32[3];
			out.yaw = 2.0f * std::atan2(qz, qw);
			ok = true;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			ok = false;
		}
		return ok ? std::optional<ClipMotion>(out) : std::nullopt;
	}

	std::optional<float> RootHeading(RE::Actor* a_actor)
	{
		float heading = 0.0f;
		bool  ok = false;
		__try {
			ok = RootHeadingUnguarded(a_actor, heading);
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			ok = false;
		}
		return ok ? std::optional<float>(heading) : std::nullopt;
	}
}
