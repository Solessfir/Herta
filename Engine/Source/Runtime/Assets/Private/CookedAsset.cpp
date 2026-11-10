#include "Herta/Assets/CookedAsset.h"

#include "Herta/Core/BinaryStream.h"

#include "ParticleSerialization.h"
#include "SoundSerialization.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <fstream>

namespace Herta
{
namespace
{
inline constexpr std::uint32_t CookedAssetMagic = 0x53414348; // "HCAS"
inline constexpr std::uint8_t TextureTag = 1;
inline constexpr std::uint8_t ModelTag = 2;
inline constexpr std::uint8_t MaterialTag = 3;
inline constexpr std::uint8_t SoundTag = 4;
inline constexpr std::uint8_t SoundGraphTag = 5;
inline constexpr std::uint8_t ParticleSystemTag = 6;

inline constexpr std::size_t MaximumVertices = MaximumCookedBufferBytes / sizeof(FCookedVertex);
inline constexpr std::size_t MaximumIndices = MaximumCookedBufferBytes / sizeof(std::uint32_t);
inline constexpr std::size_t MaximumMips = 13;
inline constexpr std::size_t MaximumSections = 4096;
inline constexpr std::size_t MaximumMaterials = 256;
inline constexpr std::size_t MaximumTextures = 256;
inline constexpr std::size_t MaximumNameLength = 256;
inline constexpr std::size_t MaximumStateMachineParameters = 64;
inline constexpr std::size_t MaximumStateMachineStates = 256;
inline constexpr std::size_t MaximumStateSamples = 16;
inline constexpr std::size_t MaximumStateEvents = 32;
inline constexpr std::size_t MaximumStateMachineTransitions = 1024;
inline constexpr std::size_t MaximumTransitionConditions = 8;
inline constexpr std::uint32_t NoIndex = 0xffffffffu;

[[nodiscard]] std::unexpected<FAssetError> Invalid(const std::string_view Message)
{
	return std::unexpected(FAssetError{std::string(Message)});
}

void WriteTexture(FBinaryWriter& Writer, const FCookedTexture& Texture)
{
	Writer.Write(static_cast<std::uint8_t>(Texture.ColorSpace));
	Writer.Write(static_cast<std::uint8_t>(Texture.PixelFormat));
	Writer.Write(static_cast<std::uint32_t>(Texture.Mips.size()));

	for (const FCookedTextureMip& Mip : Texture.Mips)
	{
		Writer.Write(Mip.Width);
		Writer.Write(Mip.Height);
		Writer.WriteBytes(Mip.Pixels);
	}
}

[[nodiscard]] FCookedTexture ReadTexture(FBinaryReader& Reader)
{
	FCookedTexture Texture;
	Texture.ColorSpace = static_cast<ETextureColorSpace>(Reader.Read<std::uint8_t>());
	Texture.PixelFormat = static_cast<ETexturePixelFormat>(Reader.Read<std::uint8_t>());
	const auto MipCount = Reader.Read<std::uint32_t>();
	std::size_t TotalBytes = 0;
	if (MipCount > MaximumMips)
	{
		Reader.Invalidate();
		return Texture;
	}

	for (std::uint32_t Index = 0; Index < MipCount && Reader.IsValid(); ++Index)
	{
		FCookedTextureMip Mip;
		Mip.Width = Reader.Read<std::uint32_t>();
		Mip.Height = Reader.Read<std::uint32_t>();
		if (Mip.Width == 0 || Mip.Height == 0 || Mip.Width > MaximumCookedTextureDimension || Mip.Height > MaximumCookedTextureDimension)
		{
			Reader.Invalidate();
			break;
		}

		const std::size_t PixelSize = Texture.PixelFormat == ETexturePixelFormat::Rgba32Float ? sizeof(float) * 4 : 4;
		const std::size_t ByteCount = std::size_t{Mip.Width} * Mip.Height * PixelSize;
		if (ByteCount > MaximumCookedBufferBytes - TotalBytes)
		{
			Reader.Invalidate();
			break;
		}

		TotalBytes += ByteCount;

		const std::span<const std::byte> Pixels = Reader.ReadBytes(ByteCount);
		Mip.Pixels.assign(Pixels.begin(), Pixels.end());
		Texture.Mips.push_back(std::move(Mip));
	}

	return Texture;
}

[[nodiscard]] bool IsFinite(const std::span<const float> Values)
{
	return std::ranges::all_of(Values, [](const float Value)
	{
		return std::isfinite(Value);
	});
}

void WriteBlob(FBinaryWriter& Writer, const std::span<const std::byte> Bytes)
{
	Writer.Write(static_cast<std::uint32_t>(Bytes.size()));
	Writer.WriteBytes(Bytes);
}

[[nodiscard]] std::vector<std::byte> ReadBlob(FBinaryReader& Reader, std::size_t& Budget)
{
	const auto Size = Reader.Read<std::uint32_t>();
	if (Size > Budget || !Reader.CanRead(Size, 1))
	{
		Reader.Invalidate();
		return {};
	}

	Budget -= Size;
	const std::span<const std::byte> Bytes = Reader.ReadBytes(Size);
	return {Bytes.begin(), Bytes.end()};
}

void WriteSkeletalData(FBinaryWriter& Writer, const FCookedModel& Model)
{
	Writer.Write(static_cast<std::uint8_t>(Model.Skeleton ? 1 : 0));
	if (!Model.Skeleton)
	{
		return;
	}

	const FCookedSkeleton& Skeleton = *Model.Skeleton;
	Writer.Write(static_cast<std::uint32_t>(Skeleton.JointNames.size()));

	for (std::size_t Joint = 0; Joint < Skeleton.JointNames.size(); ++Joint)
	{
		Writer.WriteString(Skeleton.JointNames[Joint]);
		Writer.Write(static_cast<std::uint16_t>(Skeleton.JointParents[Joint]));

		for (const float Value : Skeleton.InverseBindMatrices[Joint])
		{
			Writer.WriteFloat(Value);
		}
	}

	WriteBlob(Writer, Skeleton.Runtime);
	Writer.Write(static_cast<std::uint32_t>(Model.Influences.size()));

	for (const FCookedSkinInfluence& Influence : Model.Influences)
	{
		for (const std::uint16_t Joint : Influence.Joints)
		{
			Writer.Write(Joint);
		}

		for (const std::uint16_t Weight : Influence.Weights)
		{
			Writer.Write(Weight);
		}
	}

	Writer.Write(static_cast<std::uint32_t>(Model.Clips.size()));

	for (const FCookedAnimationClip& Clip : Model.Clips)
	{
		Writer.WriteString(Clip.Name);
		Writer.WriteFloat(Clip.DurationSeconds);
		WriteBlob(Writer, Clip.Animation);
		WriteBlob(Writer, Clip.RootMotion);
	}

	Writer.Write(static_cast<std::uint32_t>(Model.SeverableJoints.size()));
	for (const std::string& Joint : Model.SeverableJoints)
	{
		Writer.WriteString(Joint);
	}

	Writer.Write(static_cast<std::uint8_t>(Model.StateMachine ? 1 : 0));
	if (!Model.StateMachine)
	{
		return;
	}

	const auto Index = [](const std::size_t Value)
	{
		return Value == AnyAnimationState ? NoIndex : static_cast<std::uint32_t>(Value);
	};

	const FAnimationStateMachineDesc& Machine = *Model.StateMachine;
	Writer.Write(static_cast<std::uint32_t>(Machine.Parameters.size()));
	for (const FAnimationParameterDesc& Parameter : Machine.Parameters)
	{
		Writer.WriteString(Parameter.Name);
		Writer.Write(static_cast<std::uint8_t>(Parameter.Type));
		Writer.WriteFloat(Parameter.Default);
	}

	Writer.Write(static_cast<std::uint32_t>(Machine.States.size()));
	for (const FAnimationStateDesc& State : Machine.States)
	{
		Writer.WriteString(State.Name);
		Writer.Write(static_cast<std::uint32_t>(State.Samples.size()));
		for (const FAnimationBlendSample& Sample : State.Samples)
		{
			Writer.WriteString(Sample.Clip);
			Writer.WriteFloat(Sample.Position);
		}

		Writer.Write(State.BlendParameter ? Index(*State.BlendParameter) : NoIndex);
		Writer.Write(static_cast<std::uint8_t>(State.bLoop ? 1 : 0));
		Writer.WriteFloat(State.Speed);
		Writer.Write(static_cast<std::uint32_t>(State.Events.size()));
		for (const FAnimationEventDesc& Event : State.Events)
		{
			Writer.WriteString(Event.Name);
			Writer.WriteFloat(Event.Phase);
		}
	}

	Writer.Write(static_cast<std::uint32_t>(Machine.Transitions.size()));
	for (const FAnimationTransitionDesc& Transition : Machine.Transitions)
	{
		Writer.Write(Index(Transition.From));
		Writer.Write(Index(Transition.To));
		Writer.WriteFloat(Transition.DurationSeconds);
		Writer.Write(static_cast<std::uint8_t>(Transition.bAtEnd ? 1 : 0));
		Writer.Write(static_cast<std::uint32_t>(Transition.Conditions.size()));
		for (const FAnimationConditionDesc& Condition : Transition.Conditions)
		{
			Writer.Write(Index(Condition.Parameter));
			Writer.Write(static_cast<std::uint8_t>(Condition.Test));
			Writer.WriteFloat(Condition.Value);
		}
	}

	Writer.Write(Index(Machine.Entry));
}

// Reads a count and rejects it above Maximum, so a corrupt count cannot drive a large allocation.
[[nodiscard]] std::uint32_t ReadCount(FBinaryReader& Reader, const std::size_t Maximum)
{
	const auto Count = Reader.Read<std::uint32_t>();
	if (Count > Maximum)
	{
		Reader.Invalidate();
		return 0;
	}

	return Count;
}

[[nodiscard]] std::size_t ReadIndex(FBinaryReader& Reader)
{
	const auto Value = Reader.Read<std::uint32_t>();
	return Value == NoIndex ? AnyAnimationState : Value;
}

void ReadStateMachine(FBinaryReader& Reader, FCookedModel& Model)
{
	if (Reader.Read<std::uint8_t>() == 0)
	{
		return;
	}

	FAnimationStateMachineDesc& Machine = Model.StateMachine.emplace();
	Machine.Parameters.resize(ReadCount(Reader, MaximumStateMachineParameters));
	for (FAnimationParameterDesc& Parameter : Machine.Parameters)
	{
		Parameter.Name = Reader.ReadString(MaximumNameLength);
		Parameter.Type = static_cast<EAnimationParameterType>(Reader.Read<std::uint8_t>());
		Parameter.Default = Reader.ReadFloat();
	}

	Machine.States.resize(ReadCount(Reader, MaximumStateMachineStates));
	for (FAnimationStateDesc& State : Machine.States)
	{
		State.Name = Reader.ReadString(MaximumNameLength);
		State.Samples.resize(ReadCount(Reader, MaximumStateSamples));
		for (FAnimationBlendSample& Sample : State.Samples)
		{
			Sample.Clip = Reader.ReadString(MaximumNameLength);
			Sample.Position = Reader.ReadFloat();
		}

		const std::size_t Blend = ReadIndex(Reader);
		State.BlendParameter = Blend == AnyAnimationState ? std::nullopt : std::optional(Blend);
		State.bLoop = Reader.Read<std::uint8_t>() != 0;
		State.Speed = Reader.ReadFloat();
		State.Events.resize(ReadCount(Reader, MaximumStateEvents));
		for (FAnimationEventDesc& Event : State.Events)
		{
			Event.Name = Reader.ReadString(MaximumNameLength);
			Event.Phase = Reader.ReadFloat();
		}
	}

	Machine.Transitions.resize(ReadCount(Reader, MaximumStateMachineTransitions));
	for (FAnimationTransitionDesc& Transition : Machine.Transitions)
	{
		Transition.From = ReadIndex(Reader);
		Transition.To = ReadIndex(Reader);
		Transition.DurationSeconds = Reader.ReadFloat();
		Transition.bAtEnd = Reader.Read<std::uint8_t>() != 0;
		Transition.Conditions.resize(ReadCount(Reader, MaximumTransitionConditions));
		for (FAnimationConditionDesc& Condition : Transition.Conditions)
		{
			Condition.Parameter = ReadIndex(Reader);
			Condition.Test = static_cast<EAnimationCondition>(Reader.Read<std::uint8_t>());
			Condition.Value = Reader.ReadFloat();
		}
	}

	Machine.Entry = ReadIndex(Reader);
}

void ReadSkeletalData(FBinaryReader& Reader, FCookedModel& Model)
{
	if (Reader.Read<std::uint8_t>() == 0)
	{
		return;
	}

	FCookedSkeleton& Skeleton = Model.Skeleton.emplace();
	const auto JointCount = Reader.Read<std::uint32_t>();
	if (JointCount > MaximumCookedJoints || !Reader.CanRead(JointCount, sizeof(std::uint32_t) + sizeof(std::int16_t) + sizeof(float) * 16))
	{
		Reader.Invalidate();
		return;
	}

	Skeleton.JointNames.resize(JointCount);
	Skeleton.JointParents.resize(JointCount);
	Skeleton.InverseBindMatrices.resize(JointCount);

	for (std::uint32_t Joint = 0; Joint < JointCount && Reader.IsValid(); ++Joint)
	{
		Skeleton.JointNames[Joint] = Reader.ReadString(MaximumNameLength);
		Skeleton.JointParents[Joint] = static_cast<std::int16_t>(Reader.Read<std::uint16_t>());

		for (float& Value : Skeleton.InverseBindMatrices[Joint])
		{
			Value = Reader.ReadFloat();
		}
	}

	std::size_t Budget = MaximumCookedBufferBytes;
	Skeleton.Runtime = ReadBlob(Reader, Budget);

	const auto InfluenceCount = Reader.Read<std::uint32_t>();
	if (InfluenceCount > MaximumVertices || !Reader.CanRead(InfluenceCount, sizeof(std::uint16_t) * 8))
	{
		Reader.Invalidate();
		return;
	}

	Model.Influences.resize(InfluenceCount);

	for (FCookedSkinInfluence& Influence : Model.Influences)
	{
		for (std::uint16_t& Joint : Influence.Joints)
		{
			Joint = Reader.Read<std::uint16_t>();
		}

		for (std::uint16_t& Weight : Influence.Weights)
		{
			Weight = Reader.Read<std::uint16_t>();
		}
	}

	const auto ClipCount = Reader.Read<std::uint32_t>();
	if (ClipCount > MaximumCookedClips)
	{
		Reader.Invalidate();
		return;
	}

	Model.Clips.reserve(ClipCount);
	Budget = MaximumCookedBufferBytes;

	for (std::uint32_t Index = 0; Index < ClipCount && Reader.IsValid(); ++Index)
	{
		FCookedAnimationClip& Clip = Model.Clips.emplace_back();
		Clip.Name = Reader.ReadString(MaximumNameLength);
		Clip.DurationSeconds = Reader.ReadFloat();
		Clip.Animation = ReadBlob(Reader, Budget);
		Clip.RootMotion = ReadBlob(Reader, Budget);
	}

	Model.SeverableJoints.resize(ReadCount(Reader, MaximumCookedJoints));
	for (std::string& Joint : Model.SeverableJoints)
	{
		Joint = Reader.ReadString(MaximumNameLength);
	}

	ReadStateMachine(Reader, Model);
}
}

std::expected<void, FAssetError> ValidateCookedTexture(const FCookedTexture& Texture)
{
	if (Texture.PixelFormat != ETexturePixelFormat::Rgba8 && Texture.PixelFormat != ETexturePixelFormat::Rgba32Float)
	{
		return Invalid("Cooked texture has an unknown pixel format");
	}

	if (Texture.PixelFormat == ETexturePixelFormat::Rgba32Float && Texture.ColorSpace != ETextureColorSpace::Linear)
	{
		return Invalid("HDR textures require linear color space");
	}

	if (Texture.ColorSpace != ETextureColorSpace::Linear && Texture.ColorSpace != ETextureColorSpace::Srgb)
	{
		return Invalid("Cooked texture has an unknown color space");
	}

	if (Texture.Mips.empty() || Texture.Mips.size() > MaximumMips)
	{
		return Invalid("Cooked texture requires between 1 and 13 mips");
	}

	const FCookedTextureMip& Top = Texture.Mips.front();
	if (Top.Width == 0 || Top.Height == 0 || Top.Width > MaximumCookedTextureDimension || Top.Height > MaximumCookedTextureDimension)
	{
		return Invalid(std::format("Cooked texture dimensions must be in [1, {}]", MaximumCookedTextureDimension));
	}

	std::uint32_t Width = Top.Width;
	std::uint32_t Height = Top.Height;
	std::size_t TotalBytes = 0;

	for (std::size_t Index = 0; Index < Texture.Mips.size(); ++Index)
	{
		const FCookedTextureMip& Mip = Texture.Mips[Index];
		const std::size_t PixelSize = Texture.PixelFormat == ETexturePixelFormat::Rgba32Float ? sizeof(float) * 4 : 4;
		if (Mip.Width != Width || Mip.Height != Height || Mip.Pixels.size() != std::size_t{Width} * Height * PixelSize)
		{
			return Invalid(std::format("Cooked texture mip {} does not follow the mip chain", Index));
		}

		TotalBytes += Mip.Pixels.size();
		if (TotalBytes > MaximumCookedBufferBytes)
		{
			return Invalid("Cooked texture exceeds the 64 MiB upload limit");
		}

		if (Texture.PixelFormat == ETexturePixelFormat::Rgba32Float)
		{
			for (std::size_t Offset = 0; Offset < Mip.Pixels.size(); Offset += sizeof(float))
			{
				float Value = 0.f;
				std::memcpy(&Value, Mip.Pixels.data() + Offset, sizeof(float));
				if (!std::isfinite(Value) || Value < 0.f || Value > 1.e12f)
				{
					return Invalid("HDR texture pixels must be finite, nonnegative, and bounded");
				}
			}
		}

		Width = std::max(1u, Width / 2);
		Height = std::max(1u, Height / 2);
	}

	const FCookedTextureMip& Bottom = Texture.Mips.back();
	if (Bottom.Width != 1 || Bottom.Height != 1)
	{
		return Invalid("Cooked texture mip chain must end at 1x1");
	}

	return {};
}

std::expected<void, FAssetError> ValidateCookedModel(const FCookedModel& Model)
{
	if (Model.Vertices.empty() || Model.Vertices.size() > MaximumVertices || Model.Indices.empty() || Model.Indices.size() > MaximumIndices || Model.Indices.size() % 3 != 0)
	{
		return Invalid("Cooked model requires vertices and a triangle list within the 64 MiB buffer limits");
	}

	if (!std::ranges::all_of(Model.Vertices, [](const FCookedVertex& Vertex)
	{
		const auto LengthSquared = [](const auto& Values)
		{
			return Values[0] * Values[0] + Values[1] * Values[1] + Values[2] * Values[2];
		};
		const float Dot = Vertex.Normal[0] * Vertex.Tangent[0] + Vertex.Normal[1] * Vertex.Tangent[1] + Vertex.Normal[2] * Vertex.Tangent[2];
		return IsFinite(Vertex.Position) && IsFinite(Vertex.UV) && IsFinite(Vertex.Normal) && IsFinite(Vertex.Tangent)
		       && std::abs(LengthSquared(Vertex.Normal) - 1.f) < 0.01f && std::abs(LengthSquared(Vertex.Tangent) - 1.f) < 0.01f
		       && std::abs(Dot) < 0.01f && (Vertex.Tangent[3] == 1.f || Vertex.Tangent[3] == -1.f);
	}))
	{
		return Invalid("Cooked model vertices must be finite");
	}

	const std::size_t VertexCount = Model.Vertices.size();
	if (!std::ranges::all_of(Model.Indices, [VertexCount](const std::uint32_t Index)
	{
		return Index < VertexCount;
	}))
	{
		return Invalid("Cooked model index references a missing vertex");
	}

	if (Model.Sections.empty() || Model.Sections.size() > MaximumSections || Model.Materials.empty() || Model.Materials.size() > MaximumMaterials || Model.Textures.size() > MaximumTextures)
	{
		return Invalid("Cooked model requires sections, materials, and textures within their limits");
	}

	for (const FCookedMeshSection& Section : Model.Sections)
	{
		if (Section.IndexCount == 0 || Section.IndexCount % 3 != 0 || Section.FirstIndex > Model.Indices.size() || Section.IndexCount > Model.Indices.size() - Section.FirstIndex || Section.Material >= Model.Materials.size())
		{
			return Invalid("Cooked model section is outside its index or material range");
		}
	}

	for (const FCookedMaterial& Material : Model.Materials)
	{
		FMaterialAsset Descriptor{.Name = Material.Name.empty() ? "Default" : Material.Name, .Parameters = Material.Parameters};

		for (std::size_t Slot = 0; Slot < MaterialTextureSlotCount; ++Slot)
		{
			Descriptor.Textures[Slot].Channel = Material.Channels[Slot];
		}

		if (Material.Name.empty() || Material.Name.size() > MaximumNameLength || !ValidateMaterial(Descriptor))
		{
			return Invalid("Cooked model material has an invalid name or parameters");
		}

		for (std::size_t Slot = 0; Slot < MaterialTextureSlotCount; ++Slot)
		{
			if ((Material.Textures[Slot] != NoCookedTexture && Material.Textures[Slot] >= Model.Textures.size()) || Material.Channels[Slot] > EMaterialChannel::Rgb)
			{
				return Invalid("Cooked model material has an invalid texture or channel");
			}

			if (Material.Textures[Slot] != NoCookedTexture && Slot != 0 && Slot != 5 && Model.Textures[Material.Textures[Slot]].ColorSpace != ETextureColorSpace::Linear)
			{
				return Invalid("Cooked numerical material maps must use linear color space");
			}
		}
	}

	for (const FCookedTexture& Texture : Model.Textures)
	{
		if (std::expected<void, FAssetError> Valid = ValidateCookedTexture(Texture); !Valid)
		{
			return Valid;
		}
	}

	return ValidateCookedSkeletalData(Model);
}

std::expected<void, FAssetError> ValidateCookedSkeletalData(const FCookedModel& Model)
{
	if (!Model.Skeleton)
	{
		if (!Model.Influences.empty() || !Model.Clips.empty() || Model.StateMachine || !Model.SeverableJoints.empty())
		{
			return Invalid("Cooked skin influences, clips, and state machines require a skeleton");
		}

		return {};
	}

	const FCookedSkeleton& Skeleton = *Model.Skeleton;
	const std::size_t JointCount = Skeleton.JointNames.size();
	if (JointCount == 0 || JointCount > MaximumCookedJoints || Skeleton.JointParents.size() != JointCount || Skeleton.InverseBindMatrices.size() != JointCount || Skeleton.Runtime.empty()
	    || Skeleton.Runtime.size() > MaximumCookedBufferBytes)
	{
		return Invalid(std::format("Cooked skeleton requires between 1 and {} joints with parents, bind matrices, and runtime data", MaximumCookedJoints));
	}

	for (std::size_t Joint = 0; Joint < JointCount; ++Joint)
	{
		const std::string& Name = Skeleton.JointNames[Joint];
		const std::int16_t Parent = Skeleton.JointParents[Joint];
		if (Name.empty() || Name.size() > MaximumNameLength || (Parent != -1 && (Parent < 0 || static_cast<std::size_t>(Parent) >= Joint)) || !IsFinite(Skeleton.InverseBindMatrices[Joint]))
		{
			return Invalid(std::format("Cooked skeleton joint {} has an invalid name, parent, or bind matrix", Joint));
		}
	}

	if (!Model.Influences.empty() && Model.Influences.size() != Model.Vertices.size())
	{
		return Invalid("Cooked skin requires one influence per vertex");
	}

	for (const FCookedSkinInfluence& Influence : Model.Influences)
	{
		std::uint32_t Total = 0;

		for (std::size_t Slot = 0; Slot < 4; ++Slot)
		{
			Total += Influence.Weights[Slot];
			if (Influence.Joints[Slot] >= JointCount)
			{
				return Invalid("Cooked skin influence references a missing joint");
			}
		}

		if (Total != CookedSkinWeightTotal)
		{
			return Invalid("Cooked skin influence weights must sum to one");
		}
	}

	if (Model.Clips.size() > MaximumCookedClips)
	{
		return Invalid(std::format("Cooked models hold at most {} animation clips", MaximumCookedClips));
	}

	std::size_t ClipBytes = 0;

	for (const FCookedAnimationClip& Clip : Model.Clips)
	{
		ClipBytes += Clip.Animation.size() + Clip.RootMotion.size();
		if (Clip.Name.empty() || Clip.Name.size() > MaximumNameLength || !std::isfinite(Clip.DurationSeconds) || Clip.DurationSeconds <= 0.f || Clip.Animation.empty() || ClipBytes > MaximumCookedBufferBytes)
		{
			return Invalid("Cooked animation clip has an invalid name, duration, or data size");
		}

		if (std::ranges::count(Model.Clips, Clip.Name, &FCookedAnimationClip::Name) != 1)
		{
			return Invalid(std::format("Cooked animation clip name '{}' is not unique", Clip.Name));
		}
	}

	if (!std::ranges::all_of(Model.SeverableJoints, [&Model](const std::string& Joint)
	{
		return std::ranges::contains(Model.Skeleton->JointNames, Joint);
	}))
	{
		return Invalid("Cooked severable joints must name joints of the skeleton");
	}

	if (!Model.StateMachine)
	{
		return {};
	}

	// Structure only: the Animation module checks clips, parameter types, and names when it creates the machine.
	const FAnimationStateMachineDesc& Machine = *Model.StateMachine;
	const auto ValidName = [](const std::string& Name)
	{
		return !Name.empty() && Name.size() <= MaximumNameLength;
	};

	const std::size_t Parameters = Machine.Parameters.size();
	const std::size_t States = Machine.States.size();
	const bool bParameters = Parameters <= MaximumStateMachineParameters && std::ranges::all_of(Machine.Parameters, [&](const FAnimationParameterDesc& Parameter)
	{
		return ValidName(Parameter.Name) && Parameter.Type <= EAnimationParameterType::Trigger && std::isfinite(Parameter.Default);
	});

	const bool bStates = States > 0 && States <= MaximumStateMachineStates && Machine.Entry < States && std::ranges::all_of(Machine.States, [&](const FAnimationStateDesc& State)
	{
		return ValidName(State.Name) && !State.Samples.empty() && State.Samples.size() <= MaximumStateSamples && State.Events.size() <= MaximumStateEvents && std::isfinite(State.Speed) && (!State.BlendParameter || *State.BlendParameter < Parameters)
		       && std::ranges::all_of(State.Samples, [&](const FAnimationBlendSample& Sample)
		{
			return ValidName(Sample.Clip) && std::isfinite(Sample.Position);
		}) && std::ranges::all_of(State.Events, [&](const FAnimationEventDesc& Event)
		{
			return ValidName(Event.Name) && std::isfinite(Event.Phase);
		});
	});

	const bool bTransitions = Machine.Transitions.size() <= MaximumStateMachineTransitions && std::ranges::all_of(Machine.Transitions, [&](const FAnimationTransitionDesc& Transition)
	{
		return (Transition.From == AnyAnimationState || Transition.From < States) && Transition.To < States && std::isfinite(Transition.DurationSeconds) && Transition.Conditions.size() <= MaximumTransitionConditions
		       && std::ranges::all_of(Transition.Conditions, [&](const FAnimationConditionDesc& Condition)
		{
			return Condition.Parameter < Parameters && Condition.Test <= EAnimationCondition::Triggered && std::isfinite(Condition.Value);
		});
	});

	if (!bParameters || !bStates || !bTransitions)
	{
		return Invalid("Cooked animation state machine has an out-of-range count, index, name, or value");
	}

	return {};
}

std::expected<std::vector<std::byte>, FAssetError> SerializeCookedAsset(const FCookedAsset& Asset)
{
	FBinaryWriter Writer;
	Writer.Write(CookedAssetMagic);
	Writer.Write(CookedAssetFormatVersion);

	if (const auto* Texture = std::get_if<FCookedTexture>(&Asset))
	{
		if (std::expected<void, FAssetError> Valid = ValidateCookedTexture(*Texture); !Valid)
		{
			return std::unexpected(std::move(Valid.error()));
		}

		Writer.Write(TextureTag);
		WriteTexture(Writer, *Texture);
		return Writer.TakeBytes();
	}

	if (const auto* Material = std::get_if<FMaterialAsset>(&Asset))
	{
		const auto Text = SerializeMaterial(*Material);
		if (!Text)
		{
			return std::unexpected(Text.error());
		}

		Writer.Write(MaterialTag);
		Writer.WriteString(*Text);
		return Writer.TakeBytes();
	}

	if (const auto* Sound = std::get_if<FCookedSound>(&Asset))
	{
		if (auto Valid = ValidateCookedSound(*Sound); !Valid)
		{
			return std::unexpected(std::move(Valid.error()));
		}

		Writer.Write(SoundTag);
		WriteCookedSound(Writer, *Sound);
		return Writer.TakeBytes();
	}

	if (const auto* Graph = std::get_if<FCookedSoundGraph>(&Asset))
	{
		if (auto Valid = ValidateSoundPlayback(Graph->Playback); !Valid)
		{
			return std::unexpected(std::move(Valid.error()));
		}

		if (auto Valid = ValidateSoundGraph(Graph->Graph); !Valid)
		{
			return std::unexpected(std::move(Valid.error()));
		}

		Writer.Write(SoundGraphTag);
		WriteCookedSoundGraph(Writer, *Graph);
		return Writer.TakeBytes();
	}

	if (const auto* Particles = std::get_if<FCookedParticleSystem>(&Asset))
	{
		if (auto Valid = ValidateParticleSystem(*Particles); !Valid)
		{
			return std::unexpected(std::move(Valid.error()));
		}

		Writer.Write(ParticleSystemTag);
		WriteParticleSystem(Writer, *Particles);
		return Writer.TakeBytes();
	}

	const FCookedModel& Model = std::get<FCookedModel>(Asset);

	if (std::expected<void, FAssetError> Valid = ValidateCookedModel(Model); !Valid)
	{
		return std::unexpected(std::move(Valid.error()));
	}

	Writer.Write(ModelTag);
	Writer.Write(static_cast<std::uint32_t>(Model.Vertices.size()));

	for (const FCookedVertex& Vertex : Model.Vertices)
	{
		for (const float Value : Vertex.Position)
		{
			Writer.WriteFloat(Value);
		}

		for (const float Value : Vertex.UV)
		{
			Writer.WriteFloat(Value);
		}

		for (const float Value : Vertex.Normal)
		{
			Writer.WriteFloat(Value);
		}

		for (const float Value : Vertex.Tangent)
		{
			Writer.WriteFloat(Value);
		}
	}

	Writer.Write(static_cast<std::uint32_t>(Model.Indices.size()));

	for (const std::uint32_t Index : Model.Indices)
	{
		Writer.Write(Index);
	}

	Writer.Write(static_cast<std::uint32_t>(Model.Sections.size()));

	for (const FCookedMeshSection& Section : Model.Sections)
	{
		Writer.Write(Section.FirstIndex);
		Writer.Write(Section.IndexCount);
		Writer.Write(Section.Material);
	}

	Writer.Write(static_cast<std::uint32_t>(Model.Materials.size()));

	for (const FCookedMaterial& Material : Model.Materials)
	{
		const auto Text = SerializeMaterial(FMaterialAsset{.Name = Material.Name.empty() ? "Default" : Material.Name, .Parameters = Material.Parameters});
		if (!Text)
		{
			return std::unexpected(Text.error());
		}

		Writer.WriteString(*Text);

		for (std::size_t Slot = 0; Slot < MaterialTextureSlotCount; ++Slot)
		{
			Writer.Write(Material.Textures[Slot]);
			Writer.Write(static_cast<std::uint8_t>(Material.Channels[Slot]));
		}
	}

	Writer.Write(static_cast<std::uint32_t>(Model.Textures.size()));

	for (const FCookedTexture& Texture : Model.Textures)
	{
		WriteTexture(Writer, Texture);
	}

	WriteSkeletalData(Writer, Model);
	return Writer.TakeBytes();
}

std::expected<FCookedAsset, FAssetError> DeserializeCookedAsset(const std::span<const std::byte> Bytes)
{
	FBinaryReader Reader(Bytes);

	if (Reader.Read<std::uint32_t>() != CookedAssetMagic || Reader.Read<std::uint32_t>() != CookedAssetFormatVersion)
	{
		return Invalid("Unsupported cooked asset format or version");
	}

	const auto Tag = Reader.Read<std::uint8_t>();
	if (Tag == MaterialTag)
	{
		const std::string Text = Reader.ReadString(std::size_t{1024} * 1024);
		if (!Reader.IsValid() || !Reader.IsAtEnd())
		{
			return Invalid("Truncated or oversized cooked material");
		}

		const auto Material = DeserializeMaterial(Text);
		return Material ? std::expected<FCookedAsset, FAssetError>(*Material) : std::unexpected(Material.error());
	}

	if (Tag == TextureTag)
	{
		FCookedTexture Texture = ReadTexture(Reader);
		if (!Reader.IsValid() || !Reader.IsAtEnd())
		{
			return Invalid("Truncated or oversized cooked texture");
		}

		if (std::expected<void, FAssetError> Valid = ValidateCookedTexture(Texture); !Valid)
		{
			return std::unexpected(std::move(Valid.error()));
		}

		return Texture;
	}

	if (Tag == SoundTag)
	{
		FCookedSound Sound = ReadCookedSound(Reader);
		if (!Reader.IsValid() || !Reader.IsAtEnd())
		{
			return Invalid("Truncated or oversized cooked sound");
		}

		if (auto Valid = ValidateCookedSound(Sound); !Valid)
		{
			return std::unexpected(std::move(Valid.error()));
		}

		return Sound;
	}

	if (Tag == SoundGraphTag)
	{
		FCookedSoundGraph Graph = ReadCookedSoundGraph(Reader);
		if (!Reader.IsValid() || !Reader.IsAtEnd())
		{
			return Invalid("Truncated or oversized cooked sound graph");
		}

		if (auto Valid = ValidateSoundPlayback(Graph.Playback); !Valid)
		{
			return std::unexpected(std::move(Valid.error()));
		}

		if (auto Valid = ValidateSoundGraph(Graph.Graph); !Valid)
		{
			return std::unexpected(std::move(Valid.error()));
		}

		return Graph;
	}

	if (Tag == ParticleSystemTag)
	{
		FCookedParticleSystem Particles = ReadParticleSystem(Reader);
		if (!Reader.IsValid() || !Reader.IsAtEnd())
		{
			return Invalid("Truncated or oversized cooked particle system");
		}

		if (auto Valid = ValidateParticleSystem(Particles); !Valid)
		{
			return std::unexpected(std::move(Valid.error()));
		}

		return Particles;
	}

	if (Tag != ModelTag)
	{
		return Invalid("Unknown cooked asset type");
	}

	FCookedModel Model;
	const auto VertexCount = Reader.Read<std::uint32_t>();
	if (VertexCount <= MaximumVertices && Reader.CanRead(VertexCount, sizeof(float) * 12))
	{
		Model.Vertices.resize(VertexCount);

		for (FCookedVertex& Vertex : Model.Vertices)
		{
			for (float& Value : Vertex.Position)
			{
				Value = Reader.ReadFloat();
			}

			for (float& Value : Vertex.UV)
			{
				Value = Reader.ReadFloat();
			}

			for (float& Value : Vertex.Normal)
			{
				Value = Reader.ReadFloat();
			}

			for (float& Value : Vertex.Tangent)
			{
				Value = Reader.ReadFloat();
			}
		}
	}

	const auto IndexCount = Reader.Read<std::uint32_t>();
	if (IndexCount <= MaximumIndices && Reader.CanRead(IndexCount, sizeof(std::uint32_t)))
	{
		Model.Indices.resize(IndexCount);

		for (std::uint32_t& Index : Model.Indices)
		{
			Index = Reader.Read<std::uint32_t>();
		}
	}

	const auto SectionCount = Reader.Read<std::uint32_t>();
	if (SectionCount <= MaximumSections && Reader.CanRead(SectionCount, sizeof(std::uint32_t) * 3))
	{
		Model.Sections.resize(SectionCount);

		for (FCookedMeshSection& Section : Model.Sections)
		{
			Section.FirstIndex = Reader.Read<std::uint32_t>();
			Section.IndexCount = Reader.Read<std::uint32_t>();
			Section.Material = Reader.Read<std::uint32_t>();
		}
	}

	const auto MaterialCount = Reader.Read<std::uint32_t>();
	if (MaterialCount <= MaximumMaterials && Reader.CanRead(MaterialCount, sizeof(std::uint32_t) * 2))
	{
		Model.Materials.resize(MaterialCount);

		for (FCookedMaterial& Material : Model.Materials)
		{
			const auto Parsed = DeserializeMaterial(Reader.ReadString(std::size_t{1024} * 1024));
			if (!Parsed)
			{
				return std::unexpected(Parsed.error());
			}

			Material.Name = Parsed->Name;
			Material.Parameters = Parsed->Parameters;

			for (std::size_t Slot = 0; Slot < MaterialTextureSlotCount; ++Slot)
			{
				Material.Textures[Slot] = Reader.Read<std::uint32_t>();
				Material.Channels[Slot] = static_cast<EMaterialChannel>(Reader.Read<std::uint8_t>());
			}
		}
	}

	const auto TextureCount = Reader.Read<std::uint32_t>();
	if (TextureCount <= MaximumTextures && Reader.CanRead(TextureCount, sizeof(std::uint32_t) + 1))
	{
		for (std::uint32_t Index = 0; Index < TextureCount && Reader.IsValid(); ++Index)
		{
			Model.Textures.push_back(ReadTexture(Reader));
		}
	}

	if (Reader.IsValid())
	{
		ReadSkeletalData(Reader, Model);
	}

	if (!Reader.IsValid() || !Reader.IsAtEnd() || VertexCount > MaximumVertices || IndexCount > MaximumIndices || SectionCount > MaximumSections || MaterialCount > MaximumMaterials || TextureCount > MaximumTextures)
	{
		return Invalid("Truncated or oversized cooked model");
	}

	if (std::expected<void, FAssetError> Valid = ValidateCookedModel(Model); !Valid)
	{
		return std::unexpected(std::move(Valid.error()));
	}

	return Model;
}

std::expected<FCookedSoundStream, FAssetError> ReadCookedSoundStream(const std::filesystem::path& File, const std::uint64_t Offset)
{
	// Everything before the samples is a few dozen bytes, but the bus name may run longer.
	std::ifstream Stream(File, std::ios::binary);
	std::vector<std::byte> Header(256);
	Stream.seekg(static_cast<std::streamoff>(Offset));
	Stream.read(reinterpret_cast<char*>(Header.data()), static_cast<std::streamsize>(Header.size()));
	if (!Stream && !Stream.eof())
	{
		return Invalid("Cannot read the cooked sound");
	}

	Header.resize(static_cast<std::size_t>(Stream.gcount()));
	FBinaryReader Reader(Header);
	if (Reader.Read<std::uint32_t>() != CookedAssetMagic || Reader.Read<std::uint32_t>() != CookedAssetFormatVersion || Reader.Read<std::uint8_t>() != SoundTag)
	{
		return Invalid("Not a cooked sound of this format version");
	}

	FCookedSound Sound;
	const std::uint64_t Samples = ReadCookedSoundHeader(Reader, Sound);
	if (!Reader.IsValid() || Sound.Channels == 0 || Sound.Channels > MaximumSoundChannels || Samples == 0 || Samples > MaximumCookedSoundSamples || Samples % Sound.Channels != 0)
	{
		return Invalid("Truncated or out-of-range cooked sound header");
	}

	// Validation needs samples, so it checks one frame of placeholders against the header's fields.
	Sound.Samples.assign(Sound.Channels, 0);
	if (auto Valid = ValidateCookedSound(Sound); !Valid)
	{
		return std::unexpected(std::move(Valid.error()));
	}

	std::error_code Error;
	const std::uint64_t Size = std::filesystem::file_size(File, Error);
	const std::uint64_t DataOffset = Offset + Reader.GetOffset();
	if (Error || Size < DataOffset || (Size - DataOffset) / sizeof(std::int16_t) < Samples)
	{
		return Invalid("The cooked sound's file ends before its samples do");
	}

	return FCookedSoundStream{.Playback = std::move(Sound.Playback), .SampleRate = Sound.SampleRate, .Channels = Sound.Channels, .bLoop = Sound.bLoop, .bStream = Sound.bStream, .Frames = Samples / Sound.Channels, .File = File, .Offset = DataOffset};
}
}
