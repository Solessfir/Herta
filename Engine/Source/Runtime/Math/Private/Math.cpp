#include "Herta/Math/Math.h"

namespace Herta
{
static_assert(FVector3::Left().Cross(FVector3::Up()) == FVector3::Forward());
static_assert(FVector3::Left().Dot(FVector3::Right()) == -1.f);
}
