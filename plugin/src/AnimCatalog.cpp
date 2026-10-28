#include "AnimCatalog.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <iterator>
#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string_view>
#include <unordered_map>

#ifdef CATALOG_DEBUG
#include <cstdio>
#define CATALOG_TRACE(n) std::fprintf(stderr, "parse fail at line %d\n", n)
#else
#define CATALOG_TRACE(n)
#endif

namespace SDS::AnimCatalog
{
	namespace
	{
		template <class T>
		bool Read(const std::vector<std::uint8_t>& a_b, std::size_t a_off, T& a_out)
		{
			if (a_off + sizeof(T) > a_b.size()) {
				return false;
			}
			std::memcpy(&a_out, a_b.data() + a_off, sizeof(T));
			return true;
		}

		std::string_view CString(const std::vector<std::uint8_t>& a_b, std::size_t a_off, std::size_t a_max = 256)
		{
			if (a_off >= a_b.size()) {
				return {};
			}
			const auto  begin = reinterpret_cast<const char*>(a_b.data() + a_off);
			std::size_t n = 0;
			const auto  cap = (std::min)(a_max, a_b.size() - a_off);
			while (n < cap && begin[n] != '\0') {
				++n;
			}
			return n < cap ? std::string_view(begin, n) : std::string_view{};
		}

		struct Section
		{
			std::uint32_t start{ 0 }, localFixups{ 0 }, globalFixups{ 0 }, virtualFixups{ 0 }, exports{ 0 }, imports{ 0 }, end{ 0 };
		};

		bool Sections(const std::vector<std::uint8_t>& a_b, Section& a_classNames, Section& a_data)
		{
			static constexpr std::uint8_t magic[8]{ 0x57, 0xE0, 0xE0, 0x57, 0x10, 0xC0, 0xC0, 0x10 };
			if (a_b.size() < 0x100 || std::memcmp(a_b.data(), magic, 8) != 0) {
				return false;
			}
			std::int32_t version = 0, count = 0;
			Read(a_b, 12, version);
			Read(a_b, 20, count);
			const std::size_t base = version < 11 ? 64 : 80;
			bool              haveNames = false, haveData = false;
			for (std::int32_t i = 0; i < count && i < 8; ++i) {
				const std::size_t o = base + std::size_t(i) * 48;
				const auto        name = CString(a_b, o, 19);
				Section           s;
				if (!Read(a_b, o + 20, s)) {
					return false;
				}
				if (name == "__classnames__") {
					a_classNames = s;
					haveNames = true;
				} else if (name == "__data__") {
					a_data = s;
					haveData = true;
				}
			}
			return haveNames && haveData && a_data.start < a_b.size();
		}

		float FirstNumber(std::string_view a_text, std::size_t a_index)
		{
			// "animmotion x y z" -> a_index 0..2; "animrotation deg" -> 0
			const auto space = a_text.find(' ');
			if (space == std::string_view::npos) {
				return 0.0f;
			}
			std::string rest(a_text.substr(space + 1));
			for (auto& ch : rest) {
				if (ch == ',' || ch == '|' || ch == '\t') {
					ch = ' ';
				}
			}
			const char* p = rest.c_str();
			float       v = 0.0f;
			for (std::size_t i = 0; i <= a_index; ++i) {
				char* next = nullptr;
				v = std::strtof(p, &next);
				if (next == p) {
					return 0.0f;
				}
				p = next;
			}
			return v;
		}

		std::mutex                                  catalogLock;
		std::unordered_map<Key, Curve, KeyHash>     catalog;
		std::unordered_map<Key, bool, KeyHash>      ambiguousKeys;
		std::atomic<bool>                           ready{ false };

		bool SamePath(const Curve& a_l, const Curve& a_r)
		{
			return a_l.moveTime == a_r.moveTime && a_l.x == a_r.x && a_l.y == a_r.y && a_l.rotTime == a_r.rotTime && a_l.rot == a_r.rot;
		}
	}

	std::uint32_t Crc32(const std::uint8_t* a_data, std::size_t a_size)
	{
		static const auto table = [] {
			std::array<std::uint32_t, 256> t{};
			for (std::uint32_t i = 0; i < 256; ++i) {
				std::uint32_t c = i;
				for (int k = 0; k < 8; ++k) {
					c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
				}
				t[i] = c;
			}
			return t;
		}();
		std::uint32_t crc = 0xFFFFFFFFu;
		for (std::size_t i = 0; i < a_size; ++i) {
			crc = table[(crc ^ a_data[i]) & 0xFF] ^ (crc >> 8);
		}
		return crc ^ 0xFFFFFFFFu;
	}

	Curve::Sample Curve::At(float a_time) const
	{
		const auto interp = [a_time](const std::vector<float>& a_t, const std::vector<float>& a_v) -> float {
			if (a_t.empty()) {
				return 0.0f;
			}
			if (a_time <= a_t.front()) {  // from the clip start (0) to the first key
				return a_t.front() > 1e-5f ? a_v.front() * (std::max)(0.0f, a_time) / a_t.front() : a_v.front();
			}
			if (a_time >= a_t.back()) {
				return a_v.back();
			}
			const auto   it = std::upper_bound(a_t.begin(), a_t.end(), a_time);
			const auto   i = static_cast<std::size_t>(it - a_t.begin());
			const float  t0 = a_t[i - 1], t1 = a_t[i];
			const float  f = t1 > t0 ? (a_time - t0) / (t1 - t0) : 0.0f;
			return a_v[i - 1] + (a_v[i] - a_v[i - 1]) * f;
		};
		return { interp(moveTime, x), interp(moveTime, y), interp(rotTime, rot) };
	}

	bool Parse(const std::vector<std::uint8_t>& a_b, Key& a_key, Curve& a_curve, bool& a_rdf)
	{
		Section names, data;
		if (!Sections(a_b, names, data)) {
			{ CATALOG_TRACE(160); return false; }
		}
		const std::size_t st = data.start;

		// local fixups: pointer slot -> target, both relative to the data section
		std::unordered_map<std::uint32_t, std::uint32_t> fixups;
		for (std::size_t i = st + data.localFixups; i + 8 <= st + data.globalFixups; i += 8) {
			std::int32_t src = 0, dst = 0;
			Read(a_b, i, src);
			Read(a_b, i + 4, dst);
			if (src >= 0) {
				fixups[std::uint32_t(src)] = std::uint32_t(dst);
			}
		}

		// virtual fixups: object -> class name; the animation object gives the key
		bool haveKey = false;
		for (std::size_t i = st + data.virtualFixups; i + 12 <= st + data.exports; i += 12) {
			std::int32_t obj = 0, sec = 0, nameOff = 0;
			Read(a_b, i, obj);
			Read(a_b, i + 4, sec);
			Read(a_b, i + 8, nameOff);
			if (obj < 0) {
				continue;
			}
			const auto cls = CString(a_b, names.start + std::size_t(nameOff), 64);
			const std::size_t o = st + std::size_t(obj);
			if (cls == "hkaSplineCompressedAnimation") {
				std::int32_t frames = 0, blocks = 0, size = 0;
				Read(a_b, o + 0x38, frames);
				Read(a_b, o + 0x3C, blocks);
				Read(a_b, o + 0xA0, size);
				const auto ptr = fixups.find(std::uint32_t(obj) + 0x98);
				if (ptr == fixups.end() || size < 0 || st + ptr->second + std::size_t(size) > a_b.size()) {
					{ CATALOG_TRACE(194); return false; }
				}
				a_key = { 'S', frames, blocks, size, Crc32(a_b.data() + st + ptr->second, std::size_t((std::min)(256, size))) };
				haveKey = true;
			} else if (cls == "hkaInterleavedUncompressedAnimation") {
				std::int32_t tracks = 0, count = 0;
				Read(a_b, o + 0x18, tracks);
				Read(a_b, o + 0x40, count);
				const auto ptr = fixups.find(std::uint32_t(obj) + 0x38);
				const std::size_t bytes = count > 0 ? std::size_t(count) * 48 : 0;
				if (ptr == fixups.end() || st + ptr->second + bytes > a_b.size()) {
					{ CATALOG_TRACE(205); return false; }
				}
				a_key = { 'I', tracks, count, 0, Crc32(a_b.data() + st + ptr->second, (std::min)(std::size_t(256), bytes)) };
				haveKey = true;
			}
		}
		if (!haveKey) {
			{ CATALOG_TRACE(212); return false; }
		}

		// annotations: { float time; pad; hkStringPtr text }, the text slot is 8 bytes after the time
		struct Point
		{
			float t, a, b;
		};
		std::vector<Point> moves, rots;
		a_rdf = false;
		for (const auto& [src, dst] : fixups) {
			const auto text = CString(a_b, st + dst, 128);
			if (text.size() < 4) {
				continue;
			}
			const bool move = text.starts_with("animmotion");
			const bool rot = !move && text.starts_with("animrotation");
			if (text.starts_with("RDF_Motion")) {
				a_rdf = true;
			}
			if ((!move && !rot) || src < 8) {
				continue;
			}
			float time = 0.0f;
			if (!Read(a_b, st + src - 8, time) || !(time >= 0.0f && time < 1000.0f)) {
				continue;
			}
			if (move) {
				moves.push_back({ time, FirstNumber(text, 0), FirstNumber(text, 1) });
			} else {
				rots.push_back({ time, FirstNumber(text, 0), 0.0f });
			}
		}
		if (moves.empty()) {
			{ CATALOG_TRACE(246); return false; }
		}
		const auto byTime = [](const Point& l, const Point& r) { return l.t < r.t; };
		std::sort(moves.begin(), moves.end(), byTime);
		std::sort(rots.begin(), rots.end(), byTime);
		a_curve = {};
		for (const auto& p : moves) {
			a_curve.moveTime.push_back(p.t);
			a_curve.x.push_back(p.a);
			a_curve.y.push_back(p.b);
		}
		for (const auto& p : rots) {
			a_curve.rotTime.push_back(p.t);
			a_curve.rot.push_back(p.a);
		}
		return true;
	}

	BuildStats Build(const std::filesystem::path& a_root)
	{
		BuildStats stats;
		const auto started = std::chrono::steady_clock::now();
		std::unordered_map<Key, Curve, KeyHash> built;
		std::unordered_map<Key, bool, KeyHash>  ambiguous;

		std::error_code ec;
		std::vector<std::uint8_t> buffer;
		for (auto it = std::filesystem::recursive_directory_iterator(a_root, std::filesystem::directory_options::skip_permission_denied, ec);
			 !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
			if (!it->is_regular_file(ec)) {
				continue;
			}
			const auto& path = it->path();
			auto        ext = path.extension().string();
			std::ranges::transform(ext, ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
			if (ext != ".hkx") {
				continue;
			}
			auto lower = path.generic_string();
			std::ranges::transform(lower, lower.begin(), [](unsigned char c) { return char(std::tolower(c)); });
			if (lower.find("/animations/") == std::string::npos) {
				continue;
			}
			++stats.scanned;
			if (lower.find("/animations/rdf/") != std::string::npos) {  // Reactive Dodge Framework drives its own dodges
				++stats.skippedRdf;
				continue;
			}
			std::ifstream in(path, std::ios::binary);
			if (!in) {
				++stats.failed;
				continue;
			}
			buffer.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
			static constexpr std::string_view needle = "animmotion";
			if (std::search(buffer.begin(), buffer.end(), needle.begin(), needle.end()) == buffer.end()) {
				continue;
			}
			++stats.withMotion;
			Key   key;
			Curve curve;
			bool  rdf = false;
			if (!Parse(buffer, key, curve, rdf)) {
				++stats.failed;
				continue;
			}
			if (rdf) {
				++stats.skippedRdf;
				continue;
			}
			curve.file = std::filesystem::relative(path, a_root.parent_path(), ec).generic_string();
			if (const auto existing = built.find(key); existing != built.end()) {
				if (!SamePath(existing->second, curve)) {
					ambiguous[key] = true;
				}
				continue;
			}
			built.emplace(key, std::move(curve));
		}
		for (const auto& [key, _] : ambiguous) {
			built.erase(key);
		}
		stats.entries = built.size();
		stats.ambiguous = ambiguous.size();
		stats.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
		{
			std::scoped_lock l{ catalogLock };
			catalog = std::move(built);
			ambiguousKeys = std::move(ambiguous);
		}
		ready = true;
		return stats;
	}

	bool Ready() { return ready.load(); }

	const Curve* Find(const Key& a_key)
	{
		if (!ready) {
			return nullptr;
		}
		std::scoped_lock l{ catalogLock };
		const auto       it = catalog.find(a_key);
		return it == catalog.end() ? nullptr : &it->second;
	}
}
