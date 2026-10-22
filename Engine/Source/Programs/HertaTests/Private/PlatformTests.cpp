#include "Herta/Platform/FileDialog.h"
#include "Herta/Platform/FileSystem.h"
#include "Herta/Platform/Platform.h"
#include "TestFiles.h"

#include <doctest/doctest.h>

#include <chrono>
#include <fstream>
#include <iterator>
#include <ostream>
#include <thread>

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

TEST_CASE("Atomic moves wait out a briefly held destination and never replace in fail mode")
{
	const Tests::FScratchDirectory Scratch("HertaMoveAtomically");
	const auto Destination = Scratch.GetPath() / "Asset.txt";
	const auto Source = Scratch.GetPath() / "Asset.txt.tmp";
	Tests::WriteText(Destination, "old");
	Tests::WriteText(Source, "new");

	SUBCASE("A reader without delete sharing, like an antivirus scan, delays the replace")
	{
		// The standard library opens files without FILE_SHARE_DELETE on Windows, which blocks a replace until the handle closes.
		std::ifstream Reader(Destination);
		REQUIRE(Reader);
		std::jthread Release([&Reader]
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
			Reader.close();
		});

		REQUIRE(MoveAtomically(Source, Destination, EMoveMode::ReplaceExisting));
		Release.join();
		std::ifstream Published(Destination);
		CHECK(std::string(std::istreambuf_iterator<char>(Published), {}) == "new");
		CHECK_FALSE(std::filesystem::exists(Source));
	}

	SUBCASE("Fail mode keeps an existing destination")
	{
		CHECK_FALSE(MoveAtomically(Source, Destination, EMoveMode::FailIfExists));
		std::ifstream Kept(Destination);
		CHECK(std::string(std::istreambuf_iterator<char>(Kept), {}) == "old");
	}

	SUBCASE("Directories move when the destination is absent")
	{
		const auto Folder = Scratch.GetPath() / "Staging";
		Tests::WriteText(Folder / "File.txt", "content");
		REQUIRE(MoveAtomically(Folder, Scratch.GetPath() / "Published", EMoveMode::FailIfExists));
		CHECK(std::filesystem::exists(Scratch.GetPath() / "Published" / "File.txt"));
	}
}
}
