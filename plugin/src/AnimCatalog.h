#pragma once

// Catalog of the root motion that animations declare with AMR annotations ("animmotion x y z", "animrotation deg").
//
// At runtime the boss clips' annotation texts cannot be read (the strings are gone), but the animation data itself
// is intact, so every .hkx is identified by a key taken from its compressed data, the same way on disk and in memory:
//   spline compressed:  numFrames, numBlocks, data size, CRC-32 of the first 256 data bytes
//   interleaved:        transform tracks, transform count, CRC-32 of the first 256 bytes of the transforms
// No CommonLib here: the parser is plain C++ so it can be tested on its own.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace SDS::AnimCatalog
{
	struct Key
	{
		char          kind{ 0 };  // 'S' spline, 'I' interleaved
		std::int32_t  a{ 0 };
		std::int32_t  b{ 0 };
		std::int32_t  c{ 0 };
		std::uint32_t crc{ 0 };

		bool operator==(const Key&) const = default;
	};

	struct KeyHash
	{
		std::size_t operator()(const Key& k) const noexcept
		{
			std::uint64_t h = 1469598103934665603ull;
			for (const std::uint64_t v : { std::uint64_t(k.kind), std::uint64_t(std::uint32_t(k.a)), std::uint64_t(std::uint32_t(k.b)),
					 std::uint64_t(std::uint32_t(k.c)), std::uint64_t(k.crc) }) {
				h = (h ^ v) * 1099511628211ull;
			}
			return static_cast<std::size_t>(h);
		}
	};

	// the annotated path of one animation: cumulative position (+Y forward, game units) and yaw (degrees) by time
	struct Curve
	{
		std::vector<float> moveTime, x, y;
		std::vector<float> rotTime, rot;
		std::string        file;  // data-relative path, for the log

		struct Sample
		{
			float x, y, rot;
		};
		Sample At(float a_time) const;
	};

	std::uint32_t Crc32(const std::uint8_t* a_data, std::size_t a_size);

	// a packfile's key and annotated path; false if it is not an animation packfile or has no animmotion
	bool Parse(const std::vector<std::uint8_t>& a_file, Key& a_key, Curve& a_curve, bool& a_rdf);

	struct BuildStats
	{
		std::size_t scanned{ 0 }, withMotion{ 0 }, entries{ 0 }, ambiguous{ 0 }, skippedRdf{ 0 }, failed{ 0 };
		double      seconds{ 0.0 };
	};

	// walks a_root (Data/meshes) for animation .hkx with animmotion; replaces the current catalog
	BuildStats Build(const std::filesystem::path& a_root);

	bool         Ready();
	const Curve* Find(const Key& a_key);  // nullptr: unknown, or two files share the key with different paths
}
