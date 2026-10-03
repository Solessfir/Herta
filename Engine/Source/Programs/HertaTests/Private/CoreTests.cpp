#include "Herta/Core/Build.h"

#include <doctest/doctest.h>

#include <ostream>

namespace Herta
{
TEST_CASE("Build configuration macros match the compiled target")
{
#if defined(HERTA_DEBUG)
	CHECK(GetBuildConfiguration() == EBuildConfiguration::Debug);
#elif defined(HERTA_DEVELOPMENT)
	CHECK(GetBuildConfiguration() == EBuildConfiguration::Development);
#elif defined(HERTA_SHIPPING)
	CHECK(GetBuildConfiguration() == EBuildConfiguration::Shipping);
#endif
}

TEST_CASE("Build configuration names are stable")
{
	CHECK(GetBuildConfigurationName(EBuildConfiguration::Debug) == "Debug");
	CHECK(GetBuildConfigurationName(EBuildConfiguration::Development) == "Development");
	CHECK(GetBuildConfigurationName(EBuildConfiguration::Shipping) == "Shipping");
}

TEST_CASE("Build revision is available without Git at runtime")
{
	CHECK_FALSE(GetBuildRevision().empty());
}
}
