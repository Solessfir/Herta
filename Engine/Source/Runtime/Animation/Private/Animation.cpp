#include "Herta/Animation/Animation.h"

#include "Herta/Core/Hash.h"
#include "Herta/Math/AffineTransform.h"

#include <ozz/animation/runtime/animation.h>
#include <ozz/animation/runtime/blending_job.h>
#include <ozz/animation/runtime/local_to_model_job.h>
#include <ozz/animation/runtime/sampling_job.h>
#include <ozz/animation/runtime/skeleton.h>
#include <ozz/animation/runtime/track.h>
#include <ozz/animation/runtime/track_sampling_job.h>
#include <ozz/base/io/archive.h>
#include <ozz/base/io/stream.h>
#include <ozz/base/maths/simd_math.h>
#include <ozz/base/maths/soa_transform.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <cstring>
#include <format>
#include <type_traits>
#include <vector>

namespace Herta
{
namespace
{
struct FArchiveSeal
{
	std::array<char, 4> Magic{'H', 'A', 'N', '1'};
	std::uint32_t Reserved = 0;
	std::uint64_t PayloadBytes = 0;
	std::uint64_t HashLow = 0;
	std::uint64_t HashHigh = 0;
};

static_assert(std::is_trivially_copyable_v<FArchiveSeal> && sizeof(FArchiveSeal) == 32);

[[nodiscard]] std::optional<std::span<const std::byte>> OpenSeal(const std::span<const std::byte> Sealed)
{
	FArchiveSeal Seal;
	if (Sealed.size() < sizeof(Seal))
	{
		return std::nullopt;
	}

	std::memcpy(&Seal, Sealed.data(), sizeof(Seal));
	const std::span<const std::byte> Payload = Sealed.subspan(sizeof(Seal));
	const FHash128 Hash = HashBytes(Payload);
	if (Seal.Magic != FArchiveSeal{}.Magic || Seal.PayloadBytes != Payload.size() || Seal.HashLow != Hash.Low || Seal.HashHigh != Hash.High)
	{
		return std::nullopt;
	}

	return Payload;
}

// ozz archives check their own tag and version, so a stale or foreign blob fails to load instead of misreading.
template <typename T> [[nodiscard]] bool LoadArchive(const std::span<const std::byte> Sealed, T& Object)
{
	const auto Bytes = OpenSeal(Sealed);
	if (!Bytes)
	{
		return false;
	}

	ozz::io::MemoryStream Stream;
	if (Stream.Write(Bytes->data(), Bytes->size()) != Bytes->size() || Stream.Seek(0, ozz::io::Stream::kSet) != 0)
	{
		return false;
	}

	ozz::io::IArchive Archive(&Stream);
	if (!Archive.TestTag<T>())
	{
		return false;
	}

	Archive >> Object;
	return true;
}

[[nodiscard]] FMatrix4 ToMatrix(const ozz::math::Float4x4& Matrix)
{
	std::array<float, 16> Elements{};

	for (std::size_t Column = 0; Column < 4; ++Column)
	{
		ozz::math::StorePtrU(Matrix.cols[Column], Elements.data() + Column * 4);
	}

	return FMatrix4(Elements);
}

struct FClip
{
	std::string Name;
	ozz::animation::Animation Animation;
	std::optional<ozz::animation::Float3Track> RootMotion;
};
}

std::vector<std::byte> SealAnimationArchive(const std::span<const std::byte> Archive)
{
	const FHash128 Hash = HashBytes(Archive);
	const FArchiveSeal Seal{.PayloadBytes = Archive.size(), .HashLow = Hash.Low, .HashHigh = Hash.High};
	std::vector<std::byte> Sealed(sizeof(Seal) + Archive.size());
	std::memcpy(Sealed.data(), &Seal, sizeof(Seal));
	std::ranges::copy(Archive, Sealed.begin() + sizeof(Seal));
	return Sealed;
}

struct FAnimationLibrary::FImplementation
{
	ozz::animation::Skeleton Skeleton;
	std::vector<std::string> JointNames;
	std::vector<std::int16_t> JointParents;
	std::vector<FMatrix4> InverseBindMatrices;
	std::vector<FClip> Clips;
	// Joints at or below spine_01, the Unreal rig's lowest spine joint; without one, the whole skeleton counts as upper body.
	std::vector<bool> UpperBody;
};

FAnimationLibrary::FAnimationLibrary(std::unique_ptr<FImplementation> InImplementation)
    : Implementation(std::move(InImplementation))
{
}

FAnimationLibrary::~FAnimationLibrary() = default;

std::expected<std::shared_ptr<const FAnimationLibrary>, FAnimationError> FAnimationLibrary::Create(const FCookedSkeleton& Skeleton, const std::span<const FCookedAnimationClip> Clips)
{
	auto Implementation = std::make_unique<FImplementation>();
	if (!LoadArchive(Skeleton.Runtime, Implementation->Skeleton))
	{
		return std::unexpected(FAnimationError{"The cooked skeleton is not a supported animation archive"});
	}

	const ozz::animation::Skeleton& Runtime = Implementation->Skeleton;
	const auto JointCount = static_cast<std::size_t>(Runtime.num_joints());
	if (JointCount != Skeleton.JointNames.size() || JointCount != Skeleton.JointParents.size() || JointCount != Skeleton.InverseBindMatrices.size())
	{
		return std::unexpected(FAnimationError{"The cooked skeleton's joints do not match its runtime archive"});
	}

	for (std::size_t Joint = 0; Joint < JointCount; ++Joint)
	{
		if (Runtime.joint_parents()[Joint] != Skeleton.JointParents[Joint] || Skeleton.JointNames[Joint] != Runtime.joint_names()[Joint])
		{
			return std::unexpected(FAnimationError{std::format("Skeleton joint {} does not match its runtime archive", Joint)});
		}
	}

	Implementation->JointNames = Skeleton.JointNames;
	Implementation->JointParents = Skeleton.JointParents;
	const auto Spine = std::ranges::find(Skeleton.JointNames, std::string_view("spine_01"));
	Implementation->UpperBody.assign(JointCount, Spine == Skeleton.JointNames.end());
	for (std::size_t Joint = 0; Joint < JointCount && Spine != Skeleton.JointNames.end(); ++Joint)
	{
		// Parents come before their children, so a joint's flag follows its parent's.
		const std::int16_t Parent = Skeleton.JointParents[Joint];
		Implementation->UpperBody[Joint] = Joint == static_cast<std::size_t>(Spine - Skeleton.JointNames.begin()) || (Parent >= 0 && Implementation->UpperBody[static_cast<std::size_t>(Parent)]);
	}

	Implementation->InverseBindMatrices.reserve(JointCount);

	for (const std::array<float, 16>& Matrix : Skeleton.InverseBindMatrices)
	{
		Implementation->InverseBindMatrices.emplace_back(Matrix);
	}

	Implementation->Clips.reserve(Clips.size());

	for (const FCookedAnimationClip& Cooked : Clips)
	{
		FClip& Clip = Implementation->Clips.emplace_back();
		Clip.Name = Cooked.Name;
		if (!LoadArchive(Cooked.Animation, Clip.Animation) || Clip.Animation.num_tracks() != Runtime.num_joints())
		{
			return std::unexpected(FAnimationError{std::format("Animation clip '{}' is not a supported archive for this skeleton", Cooked.Name)});
		}

		if (!Cooked.RootMotion.empty() && !LoadArchive(Cooked.RootMotion, Clip.RootMotion.emplace()))
		{
			return std::unexpected(FAnimationError{std::format("Animation clip '{}' has unreadable root motion", Cooked.Name)});
		}
	}

	return std::shared_ptr<const FAnimationLibrary>(new FAnimationLibrary(std::move(Implementation)));
}

std::size_t FAnimationLibrary::GetJointCount() const noexcept
{
	return Implementation->JointNames.size();
}

std::span<const std::string> FAnimationLibrary::GetJointNames() const noexcept
{
	return Implementation->JointNames;
}

std::span<const std::int16_t> FAnimationLibrary::GetJointParents() const noexcept
{
	return Implementation->JointParents;
}

std::span<const FMatrix4> FAnimationLibrary::GetInverseBindMatrices() const noexcept
{
	return Implementation->InverseBindMatrices;
}

std::size_t FAnimationLibrary::GetClipCount() const noexcept
{
	return Implementation->Clips.size();
}

std::optional<std::size_t> FAnimationLibrary::FindClip(const std::string_view Name) const noexcept
{
	const auto Found = std::ranges::find(Implementation->Clips, Name, &FClip::Name);
	return Found == Implementation->Clips.end() ? std::nullopt : std::optional(static_cast<std::size_t>(Found - Implementation->Clips.begin()));
}

std::string_view FAnimationLibrary::GetClipName(const std::size_t Clip) const noexcept
{
	return Clip < Implementation->Clips.size() ? std::string_view(Implementation->Clips[Clip].Name) : std::string_view{};
}

float FAnimationLibrary::GetClipDuration(const std::size_t Clip) const noexcept
{
	return Clip < Implementation->Clips.size() ? Implementation->Clips[Clip].Animation.duration() : 0.f;
}

FVector3 FAnimationLibrary::GetRootMotion(const std::size_t Clip, const float FromSeconds, const float ToSeconds) const noexcept
{
	if (Clip >= Implementation->Clips.size() || !Implementation->Clips[Clip].RootMotion)
	{
		return {};
	}

	const FClip& Source = Implementation->Clips[Clip];
	const float Duration = Source.Animation.duration();
	const auto Sample = [&](const float Seconds)
	{
		ozz::math::Float3 Result;
		ozz::animation::Float3TrackSamplingJob Job;
		Job.track = &*Source.RootMotion;
		Job.ratio = std::clamp(Seconds / Duration, 0.f, 1.f);
		Job.result = &Result;
		return Job.Run() ? FVector3{Result.x, 0.f, Result.z} : FVector3{};
	};

	if (ToSeconds >= FromSeconds)
	{
		return Sample(ToSeconds) - Sample(FromSeconds);
	}

	return Sample(Duration) - Sample(FromSeconds) + Sample(ToSeconds) - Sample(0.f);
}

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wignored-attributes"
#endif
// ozz's SIMD vector type carries an alignment attribute that GCC drops from template arguments; vector storage is 16-byte aligned on every
// supported platform, which is all it needs.
using FJointWeights = std::vector<ozz::math::SimdFloat4>;
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

struct FAnimationPose::FImplementation
{
	std::shared_ptr<const FAnimationLibrary> Library;
	// One sampling context per layer slot: a context caches keyframes for the last animation it sampled, so sharing one would thrash it.
	std::vector<std::unique_ptr<ozz::animation::SamplingJob::Context>> LayerContexts;
	std::vector<std::vector<ozz::math::SoaTransform>> LayerLocals;
	// Per-joint weights for layers that weight the upper and lower body apart, four joints a vector.
	std::vector<FJointWeights> LayerJointWeights;
	std::vector<ozz::math::SoaTransform> Locals;
	std::vector<ozz::math::Float4x4> Models;
	std::vector<FMatrix4> ModelTransforms;
	std::vector<FMatrix4> SkinningMatrices;
	// OverrideJoints' scratch: the evaluated transforms and which joints moved.
	std::vector<FMatrix4> Evaluated;
	std::vector<bool> Moved;
};

FAnimationPose::FAnimationPose(std::shared_ptr<const FAnimationLibrary> Library)
    : Implementation(std::make_unique<FImplementation>())
{
	const auto& Skeleton = Library->Implementation->Skeleton;
	const auto SoaJoints = static_cast<std::size_t>(Skeleton.num_soa_joints());
	const auto Joints = static_cast<std::size_t>(Skeleton.num_joints());
	Implementation->Locals.resize(SoaJoints);
	Implementation->Models.resize(Joints);
	Implementation->ModelTransforms.resize(Joints);
	Implementation->SkinningMatrices.resize(Joints);
	Implementation->Library = std::move(Library);
	(void)Evaluate({});
}

FAnimationPose::FAnimationPose(FAnimationPose&&) noexcept = default;

FAnimationPose& FAnimationPose::operator=(FAnimationPose&&) noexcept = default;

FAnimationPose::~FAnimationPose() = default;

std::expected<void, FAnimationError> FAnimationPose::Evaluate(const std::span<const FAnimationLayer> Layers)
{
	if (Layers.size() > MaximumLayers)
	{
		return std::unexpected(FAnimationError{std::format("An animation pose blends at most {} layers", MaximumLayers)});
	}

	const FAnimationLibrary::FImplementation& Library = *Implementation->Library->Implementation;
	const ozz::animation::Skeleton& Skeleton = Library.Skeleton;
	std::array<ozz::animation::BlendingJob::Layer, MaximumLayers> Blends{};
	std::size_t BlendCount = 0;

	for (const FAnimationLayer& Layer : Layers)
	{
		if (Layer.Clip >= Library.Clips.size() || !std::isfinite(Layer.TimeSeconds) || !std::isfinite(Layer.Weight))
		{
			return std::unexpected(FAnimationError{"An animation layer names a missing clip or a nonfinite time or weight"});
		}

		if (Layer.Weight <= 0.f)
		{
			continue;
		}

		if (Implementation->LayerLocals.size() <= BlendCount)
		{
			Implementation->LayerLocals.emplace_back(static_cast<std::size_t>(Skeleton.num_soa_joints()));
			Implementation->LayerContexts.push_back(std::make_unique<ozz::animation::SamplingJob::Context>(Skeleton.num_joints()));
			Implementation->LayerJointWeights.emplace_back(static_cast<std::size_t>(Skeleton.num_soa_joints()));
		}

		const ozz::animation::Animation& Animation = Library.Clips[Layer.Clip].Animation;
		std::vector<ozz::math::SoaTransform>& Output = Implementation->LayerLocals[BlendCount];
		ozz::animation::SamplingJob Sampling;
		Sampling.animation = &Animation;
		Sampling.context = Implementation->LayerContexts[BlendCount].get();
		Sampling.ratio = std::clamp(Layer.TimeSeconds / Animation.duration(), 0.f, 1.f);
		Sampling.output = ozz::make_span(Output);
		if (!Sampling.Run())
		{
			return std::unexpected(FAnimationError{"Animation sampling failed"});
		}

		Blends[BlendCount] = {.weight = Layer.Weight, .transform = ozz::make_span(Output), .joint_weights = {}};
		if (Layer.UpperBodyWeight != 1.f || Layer.LowerBodyWeight != 1.f)
		{
			FJointWeights& Weights = Implementation->LayerJointWeights[BlendCount];
			const std::size_t Joints = Library.UpperBody.size();
			std::array<float, 4> Lanes{};
			for (std::size_t Soa = 0; Soa < Weights.size(); ++Soa)
			{
				for (std::size_t Lane = 0; Lane < 4; ++Lane)
				{
					const std::size_t Joint = Soa * 4 + Lane;
					Lanes[Lane] = Joint < Joints && Library.UpperBody[Joint] ? Layer.UpperBodyWeight : Layer.LowerBodyWeight;
				}

				Weights[Soa] = ozz::math::simd_float4::Load(Lanes[0], Lanes[1], Lanes[2], Lanes[3]);
			}

			Blends[BlendCount].joint_weights = ozz::make_span(Weights);
		}

		++BlendCount;
	}

	ozz::animation::BlendingJob Blending;
	Blending.layers = ozz::span<const ozz::animation::BlendingJob::Layer>(Blends.data(), BlendCount);
	Blending.rest_pose = Skeleton.joint_rest_poses();
	Blending.output = ozz::make_span(Implementation->Locals);
	// Any positive weight counts; below this total the blend falls back on the rest pose.
	Blending.threshold = 1.e-6f;

	ozz::animation::LocalToModelJob LocalToModel;
	LocalToModel.skeleton = &Skeleton;
	LocalToModel.input = ozz::make_span(Implementation->Locals);
	LocalToModel.output = ozz::make_span(Implementation->Models);
	if (!Blending.Run() || !LocalToModel.Run())
	{
		return std::unexpected(FAnimationError{"Animation blending failed"});
	}

	for (std::size_t Joint = 0; Joint < Implementation->Models.size(); ++Joint)
	{
		Implementation->ModelTransforms[Joint] = ToMatrix(Implementation->Models[Joint]);
		Implementation->SkinningMatrices[Joint] = Implementation->ModelTransforms[Joint] * Library.InverseBindMatrices[Joint];
	}

	return {};
}

const std::shared_ptr<const FAnimationLibrary>& FAnimationPose::GetLibrary() const noexcept
{
	return Implementation->Library;
}

std::span<const FMatrix4> FAnimationPose::GetModelTransforms() const noexcept
{
	return Implementation->ModelTransforms;
}

std::span<const FMatrix4> FAnimationPose::GetSkinningMatrices() const noexcept
{
	return Implementation->SkinningMatrices;
}

void FAnimationPose::CollapseJoint(const std::size_t Joint) noexcept
{
	const std::span<const std::int16_t> Parents = Implementation->Library->GetJointParents();
	if (Joint >= Parents.size())
	{
		return;
	}

	// Parents precede children, so one forward pass finds every descendant.
	const FVector3 Point = Implementation->ModelTransforms[Joint].TransformPosition({});
	const FMatrix4 Collapsed = FMatrix4::Translation(Point) * FMatrix4::Scale(FVector3{});
	std::vector<bool> Inside(Parents.size(), false);
	Inside[Joint] = true;

	for (std::size_t Index = Joint; Index < Parents.size(); ++Index)
	{
		Inside[Index] = Inside[Index] || (Parents[Index] >= 0 && Inside[static_cast<std::size_t>(Parents[Index])]);
		if (Inside[Index])
		{
			Implementation->SkinningMatrices[Index] = Collapsed;
		}
	}
}

namespace
{
// Translation and scale blend linearly and rotation by normalized lerp, which is close to slerp at the small angles a ragdoll blend covers.
[[nodiscard]] FMatrix4 BlendTransforms(const FMatrix4& From, const FMatrix4& To, const float Weight)
{
	const std::optional<FTransform> A = TryDecomposeTransform(From, 1.e-3f);
	const std::optional<FTransform> B = TryDecomposeTransform(To, 1.e-3f);
	if (!A || !B)
	{
		return Weight < 0.5f ? From : To;
	}

	const auto RotationA = A->Rotation.ToXYZW();
	auto RotationB = B->Rotation.ToXYZW();
	const float Dot = RotationA[0] * RotationB[0] + RotationA[1] * RotationB[1] + RotationA[2] * RotationB[2] + RotationA[3] * RotationB[3];
	std::array<float, 4> Rotation{};
	for (std::size_t Index = 0; Index < 4; ++Index)
	{
		Rotation[Index] = RotationA[Index] + ((Dot < 0.f ? -RotationB[Index] : RotationB[Index]) - RotationA[Index]) * Weight;
	}

	return FMatrix4::Transform(A->Translation + (B->Translation - A->Translation) * Weight, FQuaternion::FromXYZW(Rotation).NormalizedOrIdentity(), A->Scale3D + (B->Scale3D - A->Scale3D) * Weight);
}
}

void FAnimationPose::OverrideJoints(const std::span<const std::size_t> Joints, const std::span<const FMatrix4> Transforms, const float Weight)
{
	const float Blend = std::isfinite(Weight) ? std::clamp(Weight, 0.f, 1.f) : 0.f;
	FImplementation& Pose = *Implementation;
	const std::span<const std::int16_t> Parents = Pose.Library->GetJointParents();
	const std::span<const FMatrix4> InverseBinds = Pose.Library->GetInverseBindMatrices();
	Pose.Evaluated = Pose.ModelTransforms;
	Pose.Moved.assign(Parents.size(), false);
	for (std::size_t Index = 0; Index < std::min(Joints.size(), Transforms.size()); ++Index)
	{
		if (Joints[Index] < Parents.size())
		{
			Pose.ModelTransforms[Joints[Index]] = Blend >= 1.f ? Transforms[Index] : BlendTransforms(Pose.Evaluated[Joints[Index]], Transforms[Index], Blend);
			Pose.Moved[Joints[Index]] = true;
		}
	}

	// Parents precede children, so one forward pass carries each moved joint's subtree along.
	const auto CarryChildren = [&Pose, Parents]
	{
		for (std::size_t Joint = 0; Joint < Parents.size(); ++Joint)
		{
			const auto Parent = static_cast<std::size_t>(Parents[Joint]);
			if (!Pose.Moved[Joint] && Parents[Joint] >= 0 && Pose.Moved[Parent])
			{
				const std::optional<FMatrix4> ParentInverse = TryInverseAffine(Pose.Evaluated[Parent]);
				Pose.ModelTransforms[Joint] = ParentInverse ? Pose.ModelTransforms[Parent] * *ParentInverse * Pose.Evaluated[Joint] : Pose.ModelTransforms[Parent];
				Pose.Moved[Joint] = true;
			}
		}
	};

	CarryChildren();

	// A joint still left above the moved ones, such as the spine below a cut piece's chest, follows its moved child; left where it was, the
	// skin it carries would stretch between the two poses. Only joints no moved joint carries come here, after the children pass, so a
	// clavicle follows the chest, never the arm below it. A backward pass carries the follow up the chain, and the next forward pass brings
	// along their other children, such as a cut piece's legs. The skeleton root stays, since it carries no skin and callers read the clip's
	// root motion from it.
	for (std::size_t Joint = Parents.size(); Joint-- > 0;)
	{
		const auto Parent = static_cast<std::size_t>(Parents[Joint]);
		if (Parents[Joint] >= 0 && Parents[Parent] >= 0 && Pose.Moved[Joint] && !Pose.Moved[Parent])
		{
			const std::optional<FMatrix4> ChildInverse = TryInverseAffine(Pose.Evaluated[Joint]);
			Pose.ModelTransforms[Parent] = ChildInverse ? Pose.ModelTransforms[Joint] * *ChildInverse * Pose.Evaluated[Parent] : Pose.ModelTransforms[Joint];
			Pose.Moved[Parent] = true;
		}
	}

	CarryChildren();
	for (std::size_t Joint = 0; Joint < Parents.size(); ++Joint)
	{
		if (Pose.Moved[Joint])
		{
			Pose.SkinningMatrices[Joint] = Pose.ModelTransforms[Joint] * InverseBinds[Joint];
		}
	}
}

void FAnimationPose::SolveLimbs(const std::optional<std::size_t> Pelvis, const FVector3& PelvisOffset, const std::span<const FLimbIkTarget> Limbs)
{
	FImplementation& Pose = *Implementation;
	const std::span<const std::int16_t> Parents = Pose.Library->GetJointParents();
	const std::span<const FMatrix4> InverseBinds = Pose.Library->GetInverseBindMatrices();
	// Applies a transform to a joint and everything below it. Parents precede children, so one forward pass finds the subtree.
	const auto MoveSubtree = [&Pose, Parents](const std::size_t Root, const FMatrix4& Transform)
	{
		Pose.Moved.assign(Parents.size(), false);
		for (std::size_t Joint = Root; Joint < Parents.size(); ++Joint)
		{
			Pose.Moved[Joint] = Joint == Root || (Parents[Joint] >= 0 && Pose.Moved[static_cast<std::size_t>(Parents[Joint])]);
			if (Pose.Moved[Joint])
			{
				Pose.ModelTransforms[Joint] = Transform * Pose.ModelTransforms[Joint];
			}
		}
	};

	const auto Position = [&Pose](const std::size_t Joint)
	{
		return Pose.ModelTransforms[Joint].TransformPosition({});
	};

	const auto RotateAbout = [](const FVector3& Pivot, const FQuaternion& Rotation)
	{
		return FMatrix4::Translation(Pivot) * FMatrix4::Rotation(Rotation) * FMatrix4::Translation(-Pivot);
	};

	if (Pelvis && *Pelvis < Parents.size() && PelvisOffset.LengthSquared() > 0.f)
	{
		MoveSubtree(*Pelvis, FMatrix4::Translation(PelvisOffset));
	}

	for (const FLimbIkTarget& Limb : Limbs)
	{
		const float Weight = std::isfinite(Limb.Weight) ? std::clamp(Limb.Weight, 0.f, 1.f) : 0.f;
		if (Limb.Root >= Parents.size() || Limb.Middle >= Parents.size() || Limb.Tip >= Parents.size() || Weight <= 0.f)
		{
			continue;
		}

		const FVector3 Hip = Position(Limb.Root);
		const FVector3 Knee = Position(Limb.Middle);
		const FVector3 Ankle = Position(Limb.Tip);
		const FVector3 Wanted = Ankle + (Limb.TipTarget - Ankle) * Weight;
		const float Upper = (Knee - Hip).Length();
		const float Lower = (Ankle - Knee).Length();
		const FVector3 ToWanted = Wanted - Hip;
		if (Upper < 1.e-4f || Lower < 1.e-4f || ToWanted.LengthSquared() < 1.e-8f)
		{
			continue;
		}

		const FVector3 Aim = ToWanted.Normalized();
		// Just short of straight, so the knee's bend direction never degenerates.
		const float Reach = std::clamp(ToWanted.Length(), std::abs(Upper - Lower) + 1.e-3f, Upper + Lower - 1.e-3f);
		// The joint bends the way it already does; a nearly straight limb falls back on the direction it was given.
		const FVector3 Bend = (Knee - Hip) - Aim * (Knee - Hip).Dot(Aim);
		const FVector3 Forward = Limb.BendFallback - Aim * Limb.BendFallback.Dot(Aim);
		const FVector3 Pole = Bend.LengthSquared() > 1.e-6f && !Limb.bForceBend ? Bend.Normalized() : Forward.Normalized();
		const float Along = (Upper * Upper + Reach * Reach - Lower * Lower) / (2.f * Reach);
		const float Out = std::sqrt(std::max(Upper * Upper - Along * Along, 0.f));
		const FVector3 NewKnee = Hip + Aim * Along + Pole * Out;
		const FVector3 Reached = Hip + Aim * Reach;

		const FQuaternion Thigh = FQuaternion::Between((Knee - Hip).Normalized(), (NewKnee - Hip).Normalized());
		MoveSubtree(Limb.Root, RotateAbout(Hip, Thigh));
		const FVector3 AnkleAfter = Position(Limb.Tip);
		const FQuaternion Shin = FQuaternion::Between((AnkleAfter - NewKnee).Normalized(), (Reached - NewKnee).Normalized());
		MoveSubtree(Limb.Middle, RotateAbout(NewKnee, Shin));

		// Undo what the bend did to the tip, then make the turn asked for.
		const FVector3 TurnFrom = Limb.TipTurnFrom.LengthSquared() > 1.e-6f ? Limb.TipTurnFrom.Normalized() : FVector3::Up();
		const FVector3 TurnTo = Limb.TipTurnTo.LengthSquared() > 1.e-6f ? Limb.TipTurnTo.Normalized() : TurnFrom;
		const FQuaternion Turn = Limb.TipExtraTurn * FQuaternion::Between(TurnFrom, TurnTo);
		const FQuaternion Foot = Turn * (Shin * Thigh).Conjugated();
		MoveSubtree(Limb.Tip, RotateAbout(Position(Limb.Tip), Foot));
	}

	for (std::size_t Joint = 0; Joint < Parents.size(); ++Joint)
	{
		Pose.SkinningMatrices[Joint] = Pose.ModelTransforms[Joint] * InverseBinds[Joint];
	}
}

void FAnimationPose::RotateJoint(const std::size_t Joint, const FQuaternion& Rotation)
{
	FImplementation& Pose = *Implementation;
	const std::span<const std::int16_t> Parents = Pose.Library->GetJointParents();
	const std::span<const FMatrix4> InverseBinds = Pose.Library->GetInverseBindMatrices();
	if (Joint >= Parents.size())
	{
		return;
	}

	const FVector3 Pivot = Pose.ModelTransforms[Joint].TransformPosition({});
	const FMatrix4 Transform = FMatrix4::Translation(Pivot) * FMatrix4::Rotation(Rotation) * FMatrix4::Translation(-Pivot);
	Pose.Moved.assign(Parents.size(), false);
	for (std::size_t Index = Joint; Index < Parents.size(); ++Index)
	{
		Pose.Moved[Index] = Index == Joint || (Parents[Index] >= 0 && Pose.Moved[static_cast<std::size_t>(Parents[Index])]);
		if (Pose.Moved[Index])
		{
			Pose.ModelTransforms[Index] = Transform * Pose.ModelTransforms[Index];
			Pose.SkinningMatrices[Index] = Pose.ModelTransforms[Index] * InverseBinds[Index];
		}
	}
}

void SkinVertices(const std::span<const FMatrix4> SkinningMatrices, const std::span<const FCookedVertex> Bind, const std::span<const FCookedSkinInfluence> Influences, const std::span<FCookedVertex> Output) noexcept
{
	const std::size_t Count = std::min({Bind.size(), Influences.size(), Output.size()});
	constexpr float WeightScale = 1.f / static_cast<float>(CookedSkinWeightTotal);

	for (std::size_t Vertex = 0; Vertex < Count; ++Vertex)
	{
		const FCookedSkinInfluence& Influence = Influences[Vertex];
		std::array<float, 16> Blend{};

		for (std::size_t Slot = 0; Slot < 4; ++Slot)
		{
			if (Influence.Weights[Slot] == 0 || Influence.Joints[Slot] >= SkinningMatrices.size())
			{
				continue;
			}

			const float Weight = static_cast<float>(Influence.Weights[Slot]) * WeightScale;
			const std::array<float, 16>& Matrix = SkinningMatrices[Influence.Joints[Slot]].Data();

			for (std::size_t Element = 0; Element < 16; ++Element)
			{
				Blend[Element] += Matrix[Element] * Weight;
			}
		}

		// Column-major: element (Row, Column) is at Column * 4 + Row.
		const auto Transform = [&Blend](const std::array<float, 3>& Value, const float W)
		{
			return std::array<float, 3>{
			    Blend[0] * Value[0] + Blend[4] * Value[1] + Blend[8] * Value[2] + Blend[12] * W,
			    Blend[1] * Value[0] + Blend[5] * Value[1] + Blend[9] * Value[2] + Blend[13] * W,
			    Blend[2] * Value[0] + Blend[6] * Value[1] + Blend[10] * Value[2] + Blend[14] * W,
			};
		};

		const FCookedVertex& Source = Bind[Vertex];
		FCookedVertex& Target = Output[Vertex];
		Target.Position = Transform(Source.Position, 1.f);
		Target.UV = Source.UV;

		// Normalized() returns zero for a collapsed joint; those vertices keep their bind directions.
		const auto Skinned = Transform(Source.Normal, 0.f);
		const FVector3 Normal = FVector3{Skinned[0], Skinned[1], Skinned[2]}.Normalized();
		const auto Tangent = Transform({Source.Tangent[0], Source.Tangent[1], Source.Tangent[2]}, 0.f);
		const FVector3 Along = (FVector3{Tangent[0], Tangent[1], Tangent[2]} - Normal * Normal.Dot(FVector3{Tangent[0], Tangent[1], Tangent[2]})).Normalized();
		const bool bDegenerate = Normal == FVector3{} || Along == FVector3{};
		Target.Normal = bDegenerate ? Source.Normal : std::array<float, 3>{Normal.X, Normal.Y, Normal.Z};
		Target.Tangent = bDegenerate ? Source.Tangent : std::array<float, 4>{Along.X, Along.Y, Along.Z, Source.Tangent[3]};
	}
}
}
