// Standalone check of AnimCatalog::Parse against the Python catalog: prints key and path ends for each file given.
#include "../../plugin/src/AnimCatalog.h"
#include <cstdio>
#include <fstream>
#include <iterator>

int main(int argc, char** argv)
{
	if (argc > 2 && std::string(argv[1]) == "--build") {
		const auto s = SDS::AnimCatalog::Build(argv[2]);
		std::printf("scanned %zu withMotion %zu entries %zu ambiguous %zu rdf %zu failed %zu in %.1f s\n", s.scanned, s.withMotion, s.entries,
			s.ambiguous, s.skippedRdf, s.failed, s.seconds);
		return 0;
	}
	for (int i = 1; i < argc; ++i) {
		std::ifstream             in(argv[i], std::ios::binary);
		std::vector<std::uint8_t> b((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
		SDS::AnimCatalog::Key     k;
		SDS::AnimCatalog::Curve   c;
		bool                      rdf = false;
		if (!SDS::AnimCatalog::Parse(b, k, c, rdf)) {
			std::printf("FAIL %s\n", argv[i]);
			continue;
		}
		const auto end = c.At(1000.0f);
		const auto mid = c.At(0.5f);
		std::printf("%c,%d,%d,%d,%u keys=%zu rot=%zu mid=(%.2f,%.2f) end=(%.2f,%.2f) %s\n", k.kind, k.a, k.b, k.c, k.crc, c.moveTime.size(),
			c.rotTime.size(), mid.x, mid.y, end.x, end.y, argv[i]);
	}
}
