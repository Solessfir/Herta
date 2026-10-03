#include "Herta/Platform/Platform.h"

#include <doctest/doctest.h>

#include <ostream>

namespace Herta
{
TEST_CASE("Platform reports the compiled target")
{
#if defined(HERTA_PLATFORM_WINDOWS)
	CHECK(GetCurrentPlatform() == EPlatform::Windows);
	CHECK(GetPlatformName(GetCurrentPlatform()) == "Windows");
#else
	CHECK(GetCurrentPlatform() == EPlatform::Linux);
	CHECK(GetPlatformName(GetCurrentPlatform()) == "Linux");
#endif
}

TEST_CASE("Platform names are stable")
{
	CHECK(GetPlatformName(EPlatform::Windows) == "Windows");
	CHECK(GetPlatformName(EPlatform::Linux) == "Linux");
}
}
