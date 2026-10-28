#include "DebugDraw.h"

#include "Diag.h"
#include "Hooks.h"
#include "Settings.h"

namespace SDS::DebugDraw
{
	namespace
	{
		constexpr auto        TrueHUDMenuName = "TrueHUD"sv;
		constexpr std::size_t MaxLines = 20000;  // if nothing renders them, do not grow forever

		struct DebugLine
		{
			RE::NiPoint3  from;
			RE::NiPoint3  to;
			std::uint32_t color;
			float         thickness;
			float         remaining;
		};

		std::mutex             lock;
		std::vector<DebugLine> lines;

		// --- projection (the same matrix and viewport TrueHUD uses) ---

		RE::NiPoint3 Rotate(const RE::NiPoint3& v, const RE::NiQuaternion& q)
		{
			const RE::NiPoint3 u{ q.x, q.y, q.z };
			const auto         t = u.Cross(v) * 2.0f;
			return v + t * q.w + u.Cross(t);
		}

		struct Camera
		{
			RE::NiPoint3 position;
			RE::NiPoint3 forward{ 0.0f, 1.0f, 0.0f };
			bool         valid{ false };
		};

		Camera CurrentCamera()
		{
			Camera camera;
			const auto playerCamera = RE::PlayerCamera::GetSingleton();
			if (!playerCamera) {
				return camera;
			}
			camera.position = playerCamera->pos;
			RE::NiQuaternion rotation{ 1.0f, 0.0f, 0.0f, 0.0f };
			if (const auto state = playerCamera->currentState.get()) {
				state->GetRotation(rotation);
			}
			camera.forward = Rotate({ 0.0f, 1.0f, 0.0f }, rotation);
			camera.valid = true;
			return camera;
		}

		bool InFront(const Camera& a_camera, const RE::NiPoint3& a_point)
		{
			return (a_point - a_camera.position).Dot(a_camera.forward) > 1.0f;
		}

		bool ToScreen(const RE::NiPoint3& a_point, const RE::GRectF& a_rect, float& a_x, float& a_y)
		{
			static const auto matrix = reinterpret_cast<const float(*)[4]>(REL::ID(519579).address());
			static const auto port = reinterpret_cast<const RE::NiRect<float>*>(REL::ID(519618).address());
			float             x, y, z;
			RE::NiCamera::WorldPtToScreenPt3(matrix, *port, a_point, x, y, z, 1e-5f);
			a_x = a_rect.left + (a_rect.right - a_rect.left) * x;
			a_y = a_rect.top + (a_rect.bottom - a_rect.top) * (1.0f - y);
			return std::isfinite(a_x) && std::isfinite(a_y);
		}

		// --- the drawing layer inside TrueHUD's movie ---

		struct Layer
		{
			RE::GPtr<RE::GFxMovieView> movie;
			RE::GFxValue               clip;

			bool Bind(RE::GFxMovieView* a_movie)
			{
				if (movie.get() != a_movie) {
					clip.SetUndefined();
					movie.reset(a_movie);
				}
				if (!movie) {
					return false;
				}
				if (clip.IsDisplayObject()) {
					return true;
				}
				RE::GFxValue root;
				if (!movie->GetVariable(&root, "_root") || !root.IsDisplayObject()) {
					return false;
				}
				if (!root.CreateEmptyMovieClip(&clip, "SDSCombatDebug", 16000) || !clip.IsDisplayObject()) {
					return false;
				}
				clip.SetMember("tabEnabled", RE::GFxValue(false));
				logs::info("debug drawing layer created in TrueHUD's movie");
				return true;
			}
		};

		Layer* layer = nullptr;  // never destroyed: Scaleform is gone by the time static destructors run

		void Render(RE::GFxMovieView* a_movie, float a_interval)
		{
			++Diag::renders;
			Hooks::OnFrame(a_interval);
			if (Settings::Get().testDraw) {
				if (const auto player = RE::PlayerCharacter::GetSingleton()) {
					const auto p = player->GetPosition();
					Point(RE::NiPoint3{ p.x, p.y, p.z + 140.0f }, 0xFF00FF, 40.0f);
					Line(RE::NiPoint3{ p.x, p.y, p.z }, RE::NiPoint3{ p.x, p.y, p.z + 140.0f }, 0xFF00FF, 3.0f);
				}
			}
			if (!layer) {
				layer = new Layer;
			}
			if (!layer->Bind(a_movie)) {
				return;
			}
			auto& clip = layer->clip;
			clip.Invoke("clear", nullptr);

			const auto camera = CurrentCamera();
			const auto rect = a_movie->GetVisibleFrameRect();

			std::scoped_lock l{ lock };
			if (!camera.valid) {
				lines.clear();
				return;
			}
			for (auto& line : lines) {
				float x0, y0, x1, y1;
				if (!InFront(camera, line.from) || !InFront(camera, line.to) || !ToScreen(line.from, rect, x0, y0) ||
					!ToScreen(line.to, rect, x1, y1)) {
					continue;
				}
				const RE::GFxValue style[]{ RE::GFxValue(static_cast<double>(line.thickness)), RE::GFxValue(static_cast<double>(line.color)),
					RE::GFxValue(100.0) };
				clip.Invoke("lineStyle", nullptr, style, 3);
				const RE::GFxValue start[]{ RE::GFxValue(static_cast<double>(x0)), RE::GFxValue(static_cast<double>(y0)) };
				clip.Invoke("moveTo", nullptr, start, 2);
				const RE::GFxValue end[]{ RE::GFxValue(static_cast<double>(x1)), RE::GFxValue(static_cast<double>(y1)) };
				clip.Invoke("lineTo", nullptr, end, 2);
				++Diag::linesDrawn;
			}
			for (auto& line : lines) {
				line.remaining -= a_interval;
			}
			std::erase_if(lines, [](const DebugLine& a_line) { return a_line.remaining <= 0.0f; });
		}

		struct AdvanceMovie
		{
			static void Thunk(RE::IMenu* a_menu, float a_interval, std::uint32_t a_currentTime)
			{
				func(a_menu, a_interval, a_currentTime);
				if (a_menu && a_menu->uiMovie) {
					Render(a_menu->uiMovie.get(), std::clamp(a_interval, 0.0f, 0.1f));
				}
			}
			static inline REL::Relocation<decltype(Thunk)> func;
		};

		bool attached = false;

		void Push(const DebugLine& a_line)
		{
			std::scoped_lock l{ lock };
			if (lines.size() >= MaxLines) {
				lines.erase(lines.begin(), lines.begin() + MaxLines / 4);
			}
			lines.push_back(a_line);
		}
	}

	void Line(const RE::NiPoint3& a_from, const RE::NiPoint3& a_to, std::uint32_t a_color, float a_thickness, float a_duration)
	{
		// a 0-duration line still has to survive until the next render
		Push({ a_from, a_to, a_color, a_thickness, (std::max)(a_duration, 1e-4f) });
	}

	void Arrow(const RE::NiPoint3& a_from, const RE::NiPoint3& a_to, std::uint32_t a_color, float a_thickness, float a_duration)
	{
		Line(a_from, a_to, a_color, a_thickness, a_duration);
		RE::NiPoint3 d = a_to - a_from;
		d.z = 0.0f;
		const float length = d.Length();
		if (length < 1.0f) {
			return;
		}
		d = d / length;
		const float  head = (std::min)(18.0f, length * 0.3f);
		const RE::NiPoint3 side{ -d.y, d.x, 0.0f };
		Line(a_to, a_to - d * head + side * (head * 0.6f), a_color, a_thickness, a_duration);
		Line(a_to, a_to - d * head - side * (head * 0.6f), a_color, a_thickness, a_duration);
	}

	void Point(const RE::NiPoint3& a_at, std::uint32_t a_color, float a_size, float a_duration)
	{
		const float r = a_size * 0.5f;
		Line(a_at - RE::NiPoint3{ r, 0, 0 }, a_at + RE::NiPoint3{ r, 0, 0 }, a_color, 2.0f, a_duration);
		Line(a_at - RE::NiPoint3{ 0, r, 0 }, a_at + RE::NiPoint3{ 0, r, 0 }, a_color, 2.0f, a_duration);
		Line(a_at - RE::NiPoint3{ 0, 0, r }, a_at + RE::NiPoint3{ 0, 0, r }, a_color, 2.0f, a_duration);
	}

	void TryAttach()
	{
		if (attached) {
			return;
		}
		const auto ui = RE::UI::GetSingleton();
		const auto menu = ui ? ui->GetMenu(TrueHUDMenuName) : nullptr;
		if (!menu || !menu->uiMovie) {
			return;
		}
		// TrueHUD's menu class lives in TrueHUD.dll; IMenu::AdvanceMovie is slot 5 (SDSCombatHUD hooks it too, the two chain)
		const auto                      vtable = *reinterpret_cast<std::uintptr_t*>(menu.get());
		REL::Relocation<std::uintptr_t> vtbl{ vtable };
		AdvanceMovie::func = vtbl.write_vfunc(0x5, AdvanceMovie::Thunk);
		attached = true;
		logs::info("debug drawing attached to the TrueHUD menu");
	}

	void Clear()
	{
		std::scoped_lock l{ lock };
		lines.clear();
	}
}
