#include "Herta/Core/Build.h"

namespace Herta
{
std::string_view GetBuildConfigurationName(const EBuildConfiguration Configuration) noexcept
{
	switch (Configuration)
	{
		case EBuildConfiguration::Debug:
			return "Debug";
		case EBuildConfiguration::Development:
			return "Development";
		case EBuildConfiguration::Shipping:
			return "Shipping";
	}

	return "Unknown";
}

std::string_view GetBuildRevision() noexcept
{
#ifdef HERTA_BUILD_REVISION
	return HERTA_BUILD_REVISION;
#else
	return "unknown";
#endif
}
}
