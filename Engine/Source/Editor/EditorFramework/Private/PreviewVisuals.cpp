#include "PreviewVisuals.h"

#include <im3d.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

namespace Herta
{
namespace
{
void DrawRectangle(const float HalfWidth, const float HalfHeight, const float Z = 0.f)
{
	const std::array<Im3d::Vec3, 4> Corners{{{-HalfWidth, -HalfHeight, Z}, {HalfWidth, -HalfHeight, Z}, {HalfWidth, HalfHeight, Z}, {-HalfWidth, HalfHeight, Z}}};
	for (std::size_t Index = 0; Index < Corners.size(); ++Index)
	{
		Im3d::DrawLine(Corners[Index], Corners[(Index + 1) % Corners.size()], Im3d::GetSize(), Im3d::GetColor());
	}
}

void DrawDome(const float Radius)
{
	Im3d::DrawCircle({0.f}, {0.f, 1.f, 0.f}, Radius, 24);

	for (const Im3d::Vec3 Axis : {Im3d::Vec3{1.f, 0.f, 0.f}, Im3d::Vec3{0.f, 0.f, 1.f}})
	{
		Im3d::Vec3 Previous = Axis * Radius;
		for (int Segment = 1; Segment <= 12; ++Segment)
		{
			const float Angle = std::numbers::pi_v<float> * static_cast<float>(Segment) / 12.f;
			const Im3d::Vec3 Point = Axis * (std::cos(Angle) * Radius) + Im3d::Vec3{0.f, std::sin(Angle) * Radius, 0.f};
			Im3d::DrawLine(Previous, Point, Im3d::GetSize(), Im3d::GetColor());
			Previous = Point;
		}
	}
}

void DrawGlyph(const EPreviewObjectKind Kind)
{
	switch (Kind)
	{
		case EPreviewObjectKind::DirectionalLight:
			Im3d::DrawCircle({0.f}, {0.f, 0.f, 1.f}, 0.5f, 20);

			for (int Ray = 0; Ray < 8; ++Ray)
			{
				const float Angle = std::numbers::pi_v<float> * static_cast<float>(Ray) / 4.f;
				const Im3d::Vec3 Direction{std::cos(Angle), std::sin(Angle), 0.f};
				Im3d::DrawLine(Direction * 0.7f, Direction, Im3d::GetSize(), Im3d::GetColor());
			}

			Im3d::DrawArrow({0.f}, {0.f, 0.f, 2.f}, 0.35f, 0.15f);
			break;
		case EPreviewObjectKind::SkyLight:
			DrawDome(1.f);
			Im3d::DrawArrow({0.f, 1.5f, 0.f}, {0.f, 0.5f, 0.f}, 0.3f, 0.15f);
			break;
		case EPreviewObjectKind::PointLight:
			Im3d::DrawSphere({0.f}, 0.55f, 12);

			for (const Im3d::Vec3 Axis : {Im3d::Vec3{1.f, 0.f, 0.f}, Im3d::Vec3{0.f, 1.f, 0.f}, Im3d::Vec3{0.f, 0.f, 1.f}})
			{
				Im3d::DrawLine(Axis * 0.75f, Axis * 1.2f, Im3d::GetSize(), Im3d::GetColor());
				Im3d::DrawLine(Axis * -0.75f, Axis * -1.2f, Im3d::GetSize(), Im3d::GetColor());
			}

			break;
		case EPreviewObjectKind::SpotLight:
			Im3d::DrawCircle({0.f}, {0.f, 0.f, 1.f}, 0.45f, 16);
			Im3d::DrawCone2({0.f, 0.f, -0.8f}, {0.f}, 0.15f, 0.45f, 12);
			Im3d::DrawArrow({0.f}, {0.f, 0.f, 1.6f}, 0.3f, 0.15f);
			break;
		case EPreviewObjectKind::RectLight:
			DrawRectangle(1.f, 0.6f);
			Im3d::DrawArrow({0.f}, {0.f, 0.f, 1.3f}, 0.3f, 0.15f);
			break;
		case EPreviewObjectKind::SkyAtmosphere:
			DrawDome(1.f);
			DrawDome(0.7f);
			Im3d::DrawCircle({0.65f, 1.2f, 0.f}, {0.f, 0.f, 1.f}, 0.25f, 12);
			break;
		case EPreviewObjectKind::HeightFog:
			for (int Row = 0; Row < 3; ++Row)
			{
				Im3d::Vec3 Previous{-1.f, static_cast<float>(Row - 1) * 0.6f, 0.f};
				for (int Segment = 1; Segment <= 12; ++Segment)
				{
					const float X = -1.f + static_cast<float>(Segment) / 6.f;
					const Im3d::Vec3 Point{X, static_cast<float>(Row - 1) * 0.6f + 0.1f * std::sin(X * 6.f), 0.f};
					Im3d::DrawLine(Previous, Point, Im3d::GetSize(), Im3d::GetColor());
					Previous = Point;
				}
			}

			break;
		default:
			break;
	}
}

void DrawLightBounds(const FLightComponent& Light)
{
	switch (Light.Type)
	{
		case ELightType::Point:
			for (const Im3d::Vec3 Normal : {Im3d::Vec3{1.f, 0.f, 0.f}, Im3d::Vec3{0.f, 1.f, 0.f}, Im3d::Vec3{0.f, 0.f, 1.f}})
			{
				Im3d::DrawCircle({0.f}, Normal, Light.Range, 48);
			}

			break;
		case ELightType::Spot:
			for (const float Angle : {Light.InnerConeAngle, Light.OuterConeAngle})
			{
				// Range is spherical distance, so the cone's far rim must stay inside it.
				const float Height = Light.Range * std::cos(Angle);
				const float Radius = Light.Range * std::sin(Angle);
				Im3d::DrawCircle({0.f, 0.f, Height}, {0.f, 0.f, 1.f}, Radius, 48);

				for (const Im3d::Vec3 Axis : {Im3d::Vec3{1.f, 0.f, 0.f}, Im3d::Vec3{0.f, 1.f, 0.f}, Im3d::Vec3{-1.f, 0.f, 0.f}, Im3d::Vec3{0.f, -1.f, 0.f}})
				{
					Im3d::DrawLine({0.f}, Axis * Radius + Im3d::Vec3{0.f, 0.f, Height}, Im3d::GetSize(), Im3d::GetColor());
				}
			}

			break;
		case ELightType::Rect:
			DrawRectangle(Light.Width * 0.5f, Light.Height * 0.5f);
			Im3d::DrawCircle({0.f}, {0.f, 0.f, 1.f}, Light.Range, 48);

			for (const Im3d::Vec3 Axis : {Im3d::Vec3{1.f, 0.f, 0.f}, Im3d::Vec3{0.f, 1.f, 0.f}})
			{
				Im3d::Vec3 Previous = Axis * Light.Range;
				for (int Segment = 1; Segment <= 24; ++Segment)
				{
					const float Angle = std::numbers::pi_v<float> * static_cast<float>(Segment) / 24.f;
					const Im3d::Vec3 Point = Axis * (std::cos(Angle) * Light.Range) + Im3d::Vec3{0.f, 0.f, std::sin(Angle) * Light.Range};
					Im3d::DrawLine(Previous, Point, Im3d::GetSize(), Im3d::GetColor());
					Previous = Point;
				}
			}

			break;
		case ELightType::Directional:
			Im3d::DrawArrow({0.f}, {0.f, 0.f, 3.f}, 0.5f, 0.2f);
			break;
		case ELightType::Sky:
			DrawDome(2.f);
			break;
	}
}

Im3d::Color LightColor(const FLightComponent& Light, const bool bSelected)
{
	const auto Display = [bSelected](const float Linear)
	{
		const float Srgb = Linear <= 0.0031308f ? Linear * 12.92f : 1.055f * std::pow(Linear, 1.f / 2.4f) - 0.055f;
		return std::clamp(Srgb * (bSelected ? 0.7f : 0.55f) + 0.3f, 0.f, 1.f);
	};

	return {Display(Light.Color.X), Display(Light.Color.Y), Display(Light.Color.Z), Light.bEnabled ? 1.f : 0.4f};
}
}

void DrawPreviewVisuals(const FWorld& World, const std::span<const FPreviewObject> Objects, const FPreviewSelection& Selection, const bool bGameView)
{
	if (bGameView)
	{
		return;
	}

	for (std::size_t Index = 0; Index < Objects.size(); ++Index)
	{
		const FPreviewObject& Object = Objects[Index];
		if (Object.Kind == EPreviewObjectKind::Entity || Object.Kind == EPreviewObjectKind::Mesh)
		{
			continue;
		}

		const auto Handle = World.FindEntity(Object.Id);
		const auto Entity = Handle ? World.GetEntity(*Handle) : std::nullopt;
		if (!Entity)
		{
			continue;
		}

		const bool bSelected = Selection.Contains(static_cast<int>(Index));
		const bool bEnabled = Entity->Light ? Entity->Light->bEnabled : Entity->SkyAtmosphere ? Entity->SkyAtmosphere->bEnabled
		                                                                                      : Entity->HeightFog && Entity->HeightFog->bEnabled;
		Im3d::Color Color = Entity->Light ? LightColor(*Entity->Light, bSelected) : Im3d::Color(bSelected ? 0xc2b584ff : 0x91b7cfb0);
		if (!bEnabled)
		{
			Color.setA(0.4f);
		}

		Im3d::PushColor(Color);
		Im3d::PushSize(bSelected ? 2.f : 1.5f);
		const float GlyphScale = std::clamp(Im3d::GetContext().pixelsToWorldSize(Object.Translation, 12.f), 0.08f, 0.8f);
		Im3d::PushMatrix(Im3d::Mat4(Object.Translation, Object.Rotation, Im3d::Vec3{GlyphScale}));
		DrawGlyph(Object.Kind);
		Im3d::PopMatrix();

		if (bSelected && bEnabled)
		{
			// Component extents use meters; entity scale must not distort physical light settings.
			Im3d::PushMatrix(Im3d::Mat4(Object.Translation, Object.Rotation, Im3d::Vec3{1.f}));
			Im3d::PushSize(1.f);

			if (Entity->Light)
			{
				DrawLightBounds(*Entity->Light);
			}
			else if (Entity->HeightFog)
			{
				Im3d::DrawCircle({0.f}, {0.f, 1.f, 0.f}, 3.f, 48);
				const float Height = Entity->HeightFog->HeightFalloff > 0.f ? std::min(10.f, 1.f / Entity->HeightFog->HeightFalloff) : 10.f;
				Im3d::DrawArrow({0.f}, {0.f, Height, 0.f}, 0.4f, 0.15f);
			}
			else if (Entity->SkyAtmosphere)
			{
				DrawDome(2.f);
			}

			Im3d::PopSize();
			Im3d::PopMatrix();
		}

		Im3d::PopSize();
		Im3d::PopColor();
	}
}
}
