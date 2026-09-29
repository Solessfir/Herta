#pragma once

namespace Im3d
{
struct Vec3;
struct Mat3;
}

namespace Herta
{
bool DrawPreviewTranslationGizmo(Im3d::Vec3& Translation, const Im3d::Mat3& Rotation, bool bLocal);
}
