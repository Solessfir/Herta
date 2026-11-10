#pragma once

#include <DetourNavMeshBuilder.h>
#include <DetourStatus.h>
#include <DetourTileCache.h>
#include <DetourTileCacheBuilder.h>

#include <cstring>

namespace Herta
{
// Poly flags navigation queries include; every walkable polygon carries it.
inline constexpr unsigned short NavigationWalkableFlag = 1;

// PackBits run-length coding. Layers are mostly runs of the same height and area, so this shrinks them several times over without a
// compression library.
class FNavigationCompressor final : public dtTileCacheCompressor
{
public:
	int maxCompressedSize(const int BufferSize) override
	{
		return BufferSize + (BufferSize + 127) / 128 + 1;
	}

	dtStatus compress(const unsigned char* const Buffer, const int BufferSize, unsigned char* const Compressed, const int MaximumSize, int* const CompressedSize) override
	{
		int Out = 0;
		int In = 0;
		while (In < BufferSize)
		{
			int Run = 1;
			while (In + Run < BufferSize && Run < 129 && Buffer[In + Run] == Buffer[In])
			{
				++Run;
			}

			if (Run >= 2)
			{
				if (Out + 2 > MaximumSize)
				{
					return DT_FAILURE | DT_BUFFER_TOO_SMALL;
				}

				Compressed[Out++] = static_cast<unsigned char>(126 + Run);
				Compressed[Out++] = Buffer[In];
				In += Run;
				continue;
			}

			const int Start = In;
			do
			{
				++In;
			} while (In < BufferSize && In - Start < 128 && !(In + 1 < BufferSize && Buffer[In + 1] == Buffer[In]));

			const int Length = In - Start;
			if (Out + 1 + Length > MaximumSize)
			{
				return DT_FAILURE | DT_BUFFER_TOO_SMALL;
			}

			Compressed[Out++] = static_cast<unsigned char>(Length - 1);
			std::memcpy(Compressed + Out, Buffer + Start, static_cast<std::size_t>(Length));
			Out += Length;
		}

		*CompressedSize = Out;
		return DT_SUCCESS;
	}

	dtStatus decompress(const unsigned char* const Compressed, const int CompressedSize, unsigned char* const Buffer, const int MaximumSize, int* const BufferSize) override
	{
		int Out = 0;
		int In = 0;
		while (In < CompressedSize)
		{
			const int Control = Compressed[In++];
			if (Control < 128)
			{
				const int Length = Control + 1;
				if (In + Length > CompressedSize || Out + Length > MaximumSize)
				{
					return DT_FAILURE | DT_INVALID_PARAM;
				}

				std::memcpy(Buffer + Out, Compressed + In, static_cast<std::size_t>(Length));
				In += Length;
				Out += Length;
				continue;
			}

			const int Length = Control - 126;
			if (In >= CompressedSize || Out + Length > MaximumSize)
			{
				return DT_FAILURE | DT_INVALID_PARAM;
			}

			std::memset(Buffer + Out, Compressed[In++], static_cast<std::size_t>(Length));
			Out += Length;
		}

		*BufferSize = Out;
		return DT_SUCCESS;
	}
};

class FNavigationMeshProcess final : public dtTileCacheMeshProcess
{
public:
	void process(dtNavMeshCreateParams* const Parameters, unsigned char* const PolyAreas, unsigned short* const PolyFlags) override
	{
		for (int Index = 0; Index < Parameters->polyCount; ++Index)
		{
			PolyFlags[Index] = PolyAreas[Index] == DT_TILECACHE_WALKABLE_AREA ? NavigationWalkableFlag : 0;
		}

		Parameters->offMeshConCount = 0;
	}
};
}
