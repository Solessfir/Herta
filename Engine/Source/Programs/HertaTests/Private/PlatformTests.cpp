#include "Herta/Platform/FileDialog.h"
#include "Herta/Platform/Platform.h"
#include "TestFiles.h"

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

TEST_CASE("File dialogs reject invalid initial directories before showing UI")
{
	const Tests::FScratchDirectory Scratch("HertaFileDialog");
	std::filesystem::path Directory;
	SUBCASE("The starting directory does not exist")
	{
		Directory = Scratch.GetPath() / "Missing";
	}
	SUBCASE("A regular file is not a starting directory")
	{
		Directory = Scratch.GetPath() / "File.txt";
		Tests::WriteText(Directory, "not a directory");
	}

	const auto Result = OpenFilesDialog("Level", {}, Directory);
	REQUIRE_FALSE(Result);
	CHECK(Result.error().Message.find("initial directory is unavailable") != std::string::npos);
}

TEST_CASE("File manager rejects unavailable directories before starting UI")
{
	const Tests::FScratchDirectory Scratch("HertaFileManager");
	std::filesystem::path Directory;

	SUBCASE("An empty path does not open the current directory")
	{
	}

	SUBCASE("The directory does not exist")
	{
		Directory = Scratch.GetPath() / "Missing";
	}

	SUBCASE("A regular file is not a directory")
	{
		Directory = Scratch.GetPath() / "File.txt";
		Tests::WriteText(Directory, "not a directory");
	}

	const auto Result = OpenDirectoryInFileManager(Directory);
	REQUIRE_FALSE(Result);
	CHECK(Result.error().Message.find("directory is unavailable") != std::string::npos);
}
}
