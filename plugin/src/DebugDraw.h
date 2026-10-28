#pragma once

namespace SDS::DebugDraw
{
	// World-space debug lines drawn by this plugin itself, in its own clip on top of TrueHUD's movie.
	// TrueHUD's DrawLine API did not show anything in SDS (neither this plugin's lines nor SCAR's), while
	// code-drawn clips in that movie do (SDSCombatHUD), so the drawing is done the same way here.
	// Safe to call from any thread.

	// colours are 0xRRGGBB; a duration of 0 shows the line for one frame
	void Line(const RE::NiPoint3& a_from, const RE::NiPoint3& a_to, std::uint32_t a_color, float a_thickness, float a_duration = 0.0f);
	void Arrow(const RE::NiPoint3& a_from, const RE::NiPoint3& a_to, std::uint32_t a_color, float a_thickness, float a_duration = 0.0f);
	void Point(const RE::NiPoint3& a_at, std::uint32_t a_color, float a_size, float a_duration = 0.0f);

	void TryAttach();  // hooks TrueHUD's menu once it exists; call again on menu open / game load
	void Clear();
}
