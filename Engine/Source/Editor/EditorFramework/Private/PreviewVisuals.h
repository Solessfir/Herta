#pragma once

#include "PreviewLevel.h"

namespace Herta
{
void DrawPreviewVisuals(const FWorld& World, std::span<const FPreviewObject> Objects, const FPreviewSelection& Selection, bool bGameView);
}
