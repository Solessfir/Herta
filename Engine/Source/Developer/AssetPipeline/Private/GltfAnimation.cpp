#include "GltfAnimation.h"

#include "Herta/Animation/Animation.h"

#include <fastgltf/tools.hpp>
#include <ozz/animation/offline/animation_builder.h>
#include <ozz/animation/offline/animation_optimizer.h>
#include <ozz/animation/offline/motion_extractor.h>
#include <ozz/animation/offline/raw_animation.h>
#include <ozz/animation/offline/raw_skeleton.h>
#include <ozz/animation/offline/raw_track.h>
#include <ozz/animation/offline/skeleton_builder.h>
#include <ozz/animation/offline/track_builder.h>
#include <ozz/animation/runtime/animation.h>
#include <ozz/animation/runtime/skeleton.h>
#include <ozz/animation/runtime/track.h>
#include <ozz/base/io/archive.h>
#include <ozz/base/io/stream.h>
#include <ozz/base/maths/simd_math.h>
#include <ozz/base/maths/soa_transform.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#include <map>
#include <optional>
#include <set>

namespace Herta
{
namespace
{
namespace Offline = ozz::animation::offline;

// Clips whose root moves less than this stay without root motion, so in-place loops carry no track.
inline constexpr float MinimumRootTravelMeters = 1.e-3f;

template <typename T> [[nodiscard]] std::vector<std::byte> SaveArchive(const T& Object)
{
	ozz::io::MemoryStream Stream;

	{
		ozz::io::OArchive Archive(&Stream);
		Archive << Object;
	}

	const int Size = Stream.Tell();
	std::vector<std::byte> Bytes(static_cast<std::size_t>(std::max(Size, 0)));
	Stream.Seek(0, ozz::io::Stream::kSet);
	Stream.Read(Bytes.data(), Bytes.size());
	return SealAnimationArchive(Bytes);
}

[[nodiscard]] ozz::math::Transform ToOzz(const fastgltf::math::fmat4x4& Matrix)
{
	fastgltf::math::fvec3 Scale;
	fastgltf::math::fquat Rotation;
	fastgltf::math::fvec3 Translation;
	fastgltf::math::decomposeTransformMatrix(Matrix, Scale, Rotation, Translation);
	return {
	    .translation = ozz::math::Float3(Translation[0], Translation[1], Translation[2]),
	    .rotation = ozz::math::Quaternion(Rotation[0], Rotation[1], Rotation[2], Rotation[3]),
	    .scale = ozz::math::Float3(Scale[0], Scale[1], Scale[2]),
	};
}

[[nodiscard]] ozz::math::Transform ToOzz(const fastgltf::Node& Node)
{
	if (const auto* TRS = std::get_if<fastgltf::TRS>(&Node.transform))
	{
		return {
		    .translation = ozz::math::Float3(TRS->translation[0], TRS->translation[1], TRS->translation[2]),
		    .rotation = ozz::math::Quaternion(TRS->rotation[0], TRS->rotation[1], TRS->rotation[2], TRS->rotation[3]),
		    .scale = ozz::math::Float3(TRS->scale[0], TRS->scale[1], TRS->scale[2]),
		};
	}

	return ToOzz(std::get<fastgltf::math::fmat4x4>(Node.transform));
}

// Static nodes above a skeleton root, such as an armature, fold into the root joint: world = Ancestor * Local keeps translation, rotation, and a uniform scale separable.
struct FAncestor
{
	ozz::math::Transform Transform = ozz::math::Transform::identity();

	[[nodiscard]] ozz::math::Float3 ApplyToTranslation(const ozz::math::Float3& Value) const
	{
		return Transform.translation + TransformVector(Transform.rotation, Value * Transform.scale.x);
	}

	[[nodiscard]] ozz::math::Quaternion ApplyToRotation(const ozz::math::Quaternion& Value) const
	{
		return Transform.rotation * Value;
	}

	[[nodiscard]] ozz::math::Float3 ApplyToScale(const ozz::math::Float3& Value) const
	{
		return Value * Transform.scale.x;
	}

	[[nodiscard]] static ozz::math::Float3 TransformVector(const ozz::math::Quaternion& Rotation, const ozz::math::Float3& Vector)
	{
		const ozz::math::Float3 Axis(Rotation.x, Rotation.y, Rotation.z);
		const ozz::math::Float3 Twice = Cross(Axis, Vector) * 2.f;
		return Vector + Twice * Rotation.w + Cross(Axis, Twice);
	}
};

struct FSkinLayout
{
	std::vector<std::size_t> Parents;
	std::set<std::size_t> Joints;
	std::vector<std::size_t> Roots;
	std::map<std::size_t, FAncestor> Ancestors;
	// Unique ozz joint names per node, so the runtime skeleton maps back to glTF nodes.
	std::map<std::size_t, std::string> Names;
};

[[nodiscard]] std::expected<FSkinLayout, FAssetError> LayOutSkin(const fastgltf::Asset& Asset, const fastgltf::Skin& Skin)
{
	constexpr std::size_t NoParent = std::numeric_limits<std::size_t>::max();
	FSkinLayout Layout;
	Layout.Parents.assign(Asset.nodes.size(), NoParent);

	for (std::size_t Node = 0; Node < Asset.nodes.size(); ++Node)
	{
		for (const std::size_t Child : Asset.nodes[Node].children)
		{
			Layout.Parents[Child] = Node;
		}
	}

	Layout.Joints.insert(Skin.joints.begin(), Skin.joints.end());
	if (Layout.Joints.size() != Skin.joints.size() || Layout.Joints.size() > MaximumCookedJoints)
	{
		return std::unexpected(FAssetError{std::format("glTF skin must list between 1 and {} distinct joints", MaximumCookedJoints)});
	}

	std::set<std::string> UsedNames;

	for (const std::size_t Joint : Skin.joints)
	{
		std::string Name(Asset.nodes[Joint].name.begin(), Asset.nodes[Joint].name.end());
		Name = Name.substr(0, 200);
		if (Name.empty() || UsedNames.contains(Name))
		{
			Name = std::format("Joint{}", Joint);
		}

		UsedNames.insert(Name);
		Layout.Names[Joint] = std::move(Name);

		if (Layout.Joints.contains(Layout.Parents[Joint]))
		{
			continue;
		}

		fastgltf::math::fmat4x4 Ancestors;

		for (std::size_t Node = Layout.Parents[Joint]; Node != NoParent; Node = Layout.Parents[Node])
		{
			if (Layout.Joints.contains(Node))
			{
				return std::unexpected(FAssetError{"glTF skin joints must form connected hierarchies without non-joint nodes between them"});
			}

			Ancestors = fastgltf::getLocalTransformMatrix(Asset.nodes[Node]) * Ancestors;
		}

		FAncestor Ancestor{.Transform = ToOzz(Ancestors)};
		const ozz::math::Float3& Scale = Ancestor.Transform.scale;
		if (std::abs(Scale.x - Scale.y) > 1.e-4f * Scale.x || std::abs(Scale.x - Scale.z) > 1.e-4f * Scale.x || Scale.x <= 0.f)
		{
			return std::unexpected(FAssetError{"Nodes above a glTF skeleton may only scale uniformly"});
		}

		Layout.Roots.push_back(Joint);
		Layout.Ancestors[Joint] = Ancestor;
	}

	return Layout;
}

void AddJoint(const fastgltf::Asset& Asset, const FSkinLayout& Layout, const std::size_t Node, Offline::RawSkeleton::Joint& Joint)
{
	Joint.name = Layout.Names.at(Node);
	Joint.transform = ToOzz(Asset.nodes[Node]);
	if (const auto Ancestor = Layout.Ancestors.find(Node); Ancestor != Layout.Ancestors.end())
	{
		Joint.transform.translation = Ancestor->second.ApplyToTranslation(Joint.transform.translation);
		Joint.transform.rotation = Ancestor->second.ApplyToRotation(Joint.transform.rotation);
		Joint.transform.scale = Ancestor->second.ApplyToScale(Joint.transform.scale);
	}

	for (const std::size_t Child : Asset.nodes[Node].children)
	{
		if (Layout.Joints.contains(Child))
		{
			AddJoint(Asset, Layout, Child, Joint.children.emplace_back());
		}
	}
}

template <typename TValue> [[nodiscard]] std::vector<TValue> ReadValues(const fastgltf::Asset& Asset, const std::size_t AccessorIndex)
{
	std::vector<TValue> Values;
	const fastgltf::Accessor& Accessor = Asset.accessors[AccessorIndex];
	Values.reserve(Accessor.count);
	fastgltf::iterateAccessor<TValue>(Asset, Accessor, [&](const TValue& Value)
	{
		Values.push_back(Value);
	});

	return Values;
}

// Converts one glTF sampler to ozz keys. ozz interpolates linearly, so a step becomes a pair of keys and a cubic spline keeps only its vertices.
template <typename TKey, typename TValue, typename TConvert> [[nodiscard]] bool AppendKeys(const fastgltf::Asset& Asset, const fastgltf::AnimationSampler& Sampler, const float StartSeconds, ozz::vector<TKey>& Keys, const TConvert& Convert)
{
	const std::vector<float> Times = ReadValues<float>(Asset, Sampler.inputAccessor);
	const std::vector<TValue> Values = ReadValues<TValue>(Asset, Sampler.outputAccessor);
	const std::size_t Stride = Sampler.interpolation == fastgltf::AnimationInterpolation::CubicSpline ? 3 : 1;
	const std::size_t Offset = Stride == 3 ? 1 : 0;
	if (Times.empty() || Values.size() != Times.size() * Stride)
	{
		return false;
	}

	for (std::size_t Index = 0; Index < Times.size(); ++Index)
	{
		const float Time = Times[Index] - StartSeconds;
		const auto Value = Convert(Values[Index * Stride + Offset]);
		if (Sampler.interpolation == fastgltf::AnimationInterpolation::Step && Index > 0)
		{
			Keys.push_back({.time = std::nextafter(Time, -std::numeric_limits<float>::infinity()), .value = Convert(Values[(Index - 1) * Stride + Offset])});
		}

		Keys.push_back({.time = Time, .value = Value});
	}

	return true;
}
}

std::expected<FGltfSkeleton, FAssetError> CookGltfSkeleton(const fastgltf::Asset& Asset, const std::size_t SkinIndex, std::vector<std::string>& Warnings)
{
	const fastgltf::Skin& Skin = Asset.skins[SkinIndex];
	auto Layout = LayOutSkin(Asset, Skin);
	if (!Layout)
	{
		return std::unexpected(std::move(Layout.error()));
	}

	Offline::RawSkeleton RawSkeleton;

	for (const std::size_t Root : Layout->Roots)
	{
		AddJoint(Asset, *Layout, Root, RawSkeleton.roots.emplace_back());
	}

	const ozz::unique_ptr<ozz::animation::Skeleton> Skeleton = Offline::SkeletonBuilder()(RawSkeleton);
	if (!Skeleton)
	{
		return std::unexpected(FAssetError{"glTF skin does not form a valid skeleton"});
	}

	FGltfSkeleton Result;
	const auto JointCount = static_cast<std::size_t>(Skeleton->num_joints());
	std::map<std::string, std::uint16_t> JointByName;

	for (std::size_t Joint = 0; Joint < JointCount; ++Joint)
	{
		Result.Skeleton.JointNames.emplace_back(Skeleton->joint_names()[Joint]);
		Result.Skeleton.JointParents.push_back(Skeleton->joint_parents()[Joint]);
		JointByName[Result.Skeleton.JointNames.back()] = static_cast<std::uint16_t>(Joint);
	}

	std::map<std::size_t, std::uint16_t> JointByNode;
	Result.SkinJointToSkeleton.reserve(Skin.joints.size());

	for (const std::size_t Node : Skin.joints)
	{
		const std::uint16_t Joint = JointByName.at(Layout->Names.at(Node));
		JointByNode[Node] = Joint;
		Result.SkinJointToSkeleton.push_back(Joint);
	}

	// The cooker keeps skinned vertices in glTF scene space, which the bind matrices already map into each joint.
	Result.Skeleton.InverseBindMatrices.assign(JointCount, {1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 1.f});
	if (Skin.inverseBindMatrices)
	{
		const auto Matrices = ReadValues<fastgltf::math::fmat4x4>(Asset, *Skin.inverseBindMatrices);
		if (Matrices.size() != Skin.joints.size())
		{
			return std::unexpected(FAssetError{"glTF skin needs one inverse bind matrix per joint"});
		}

		for (std::size_t Slot = 0; Slot < Matrices.size(); ++Slot)
		{
			std::array<float, 16>& Target = Result.Skeleton.InverseBindMatrices[Result.SkinJointToSkeleton[Slot]];

			for (std::size_t Column = 0; Column < 4; ++Column)
			{
				for (std::size_t Row = 0; Row < 4; ++Row)
				{
					Target[Column * 4 + Row] = Matrices[Slot][Column][Row];
				}
			}
		}
	}

	Result.Skeleton.Runtime = SaveArchive(*Skeleton);

	for (const fastgltf::Animation& Source : Asset.animations)
	{
		const std::string Name = std::string(Source.name.begin(), Source.name.end()).substr(0, 200);
		float StartSeconds = std::numeric_limits<float>::max();
		float EndSeconds = std::numeric_limits<float>::lowest();

		for (const fastgltf::AnimationChannel& Channel : Source.channels)
		{
			if (!Channel.nodeIndex || !JointByNode.contains(*Channel.nodeIndex) || Channel.path == fastgltf::AnimationPath::Weights)
			{
				continue;
			}

			// Bounds come from the keys themselves rather than the accessor's optional min and max.
			for (const float Time : ReadValues<float>(Asset, Source.samplers[Channel.samplerIndex].inputAccessor))
			{
				StartSeconds = std::min(StartSeconds, Time);
				EndSeconds = std::max(EndSeconds, Time);
			}
		}

		if (Name.empty() || EndSeconds <= StartSeconds)
		{
			if (Warnings.size() < 32)
			{
				Warnings.push_back(std::format("Animation '{}' moves no skeleton joint over time and was skipped", Name));
			}

			continue;
		}

		Offline::RawAnimation Raw;
		Raw.duration = EndSeconds - StartSeconds;
		Raw.name = Name;
		Raw.tracks.resize(JointCount);

		for (const fastgltf::AnimationChannel& Channel : Source.channels)
		{
			if (!Channel.nodeIndex || !JointByNode.contains(*Channel.nodeIndex) || Channel.path == fastgltf::AnimationPath::Weights)
			{
				continue;
			}

			Offline::RawAnimation::JointTrack& Track = Raw.tracks[JointByNode.at(*Channel.nodeIndex)];
			const fastgltf::AnimationSampler& Sampler = Source.samplers[Channel.samplerIndex];
			const auto Ancestor = Layout->Ancestors.find(*Channel.nodeIndex);
			const FAncestor* const Root = Ancestor == Layout->Ancestors.end() ? nullptr : &Ancestor->second;
			bool bRead = false;
			if (Channel.path == fastgltf::AnimationPath::Translation)
			{
				bRead = AppendKeys<Offline::RawAnimation::TranslationKey, fastgltf::math::fvec3>(Asset, Sampler, StartSeconds, Track.translations, [Root](const fastgltf::math::fvec3& Value)
				{
					const ozz::math::Float3 Local(Value[0], Value[1], Value[2]);
					return Root ? Root->ApplyToTranslation(Local) : Local;
				});
			}
			else if (Channel.path == fastgltf::AnimationPath::Rotation)
			{
				bRead = AppendKeys<Offline::RawAnimation::RotationKey, fastgltf::math::fvec4>(Asset, Sampler, StartSeconds, Track.rotations, [Root](const fastgltf::math::fvec4& Value)
				{
					const ozz::math::Quaternion Local = Normalize(ozz::math::Quaternion(Value[0], Value[1], Value[2], Value[3]));
					return Root ? Root->ApplyToRotation(Local) : Local;
				});
			}
			else
			{
				bRead = AppendKeys<Offline::RawAnimation::ScaleKey, fastgltf::math::fvec3>(Asset, Sampler, StartSeconds, Track.scales, [Root](const fastgltf::math::fvec3& Value)
				{
					const ozz::math::Float3 Local(Value[0], Value[1], Value[2]);
					return Root ? Root->ApplyToScale(Local) : Local;
				});
			}

			if (!bRead)
			{
				return std::unexpected(FAssetError{std::format("Animation '{}' has a sampler whose times and values do not match", Name)});
			}

			if (Sampler.interpolation == fastgltf::AnimationInterpolation::CubicSpline && Warnings.size() < 32)
			{
				Warnings.push_back(std::format("Animation '{}' uses cubic splines, which cook as linear keys", Name));
			}
		}

		// Joints the clip leaves alone hold their rest pose; an empty ozz track would mean identity instead.
		for (std::size_t Joint = 0; Joint < JointCount; ++Joint)
		{
			const ozz::math::SoaTransform& Soa = Skeleton->joint_rest_poses()[Joint / 4];
			const std::size_t Lane = Joint % 4;
			std::array<float, 4> X{};
			std::array<float, 4> Y{};
			std::array<float, 4> Z{};
			std::array<float, 4> W{};
			Offline::RawAnimation::JointTrack& Track = Raw.tracks[Joint];
			if (Track.translations.empty())
			{
				ozz::math::StorePtrU(Soa.translation.x, X.data());
				ozz::math::StorePtrU(Soa.translation.y, Y.data());
				ozz::math::StorePtrU(Soa.translation.z, Z.data());
				Track.translations.push_back({.time = 0.f, .value = ozz::math::Float3(X[Lane], Y[Lane], Z[Lane])});
			}

			if (Track.rotations.empty())
			{
				ozz::math::StorePtrU(Soa.rotation.x, X.data());
				ozz::math::StorePtrU(Soa.rotation.y, Y.data());
				ozz::math::StorePtrU(Soa.rotation.z, Z.data());
				ozz::math::StorePtrU(Soa.rotation.w, W.data());
				Track.rotations.push_back({.time = 0.f, .value = ozz::math::Quaternion(X[Lane], Y[Lane], Z[Lane], W[Lane])});
			}

			if (Track.scales.empty())
			{
				ozz::math::StorePtrU(Soa.scale.x, X.data());
				ozz::math::StorePtrU(Soa.scale.y, Y.data());
				ozz::math::StorePtrU(Soa.scale.z, Z.data());
				Track.scales.push_back({.time = 0.f, .value = ozz::math::Float3(X[Lane], Y[Lane], Z[Lane])});
			}
		}

		if (!Raw.Validate())
		{
			return std::unexpected(FAssetError{std::format("Animation '{}' has keys outside its duration or out of order", Name)});
		}

		// Root motion becomes its own track so clips play in place and gameplay decides how far the character travels.
		Offline::MotionExtractor Extractor;
		Extractor.rotation_settings = {.x = false, .y = false, .z = false, .reference = Offline::MotionExtractor::Reference::kSkeleton, .bake = false, .loop = false};
		Offline::RawFloat3Track Motion;
		Offline::RawQuaternionTrack Unused;
		Offline::RawAnimation InPlace;
		if (!Extractor(Raw, *Skeleton, &Motion, &Unused, &InPlace))
		{
			return std::unexpected(FAssetError{std::format("Animation '{}' root motion could not be extracted", Name)});
		}

		Offline::RawAnimation Optimized;
		if (!Offline::AnimationOptimizer()(InPlace, *Skeleton, &Optimized))
		{
			return std::unexpected(FAssetError{std::format("Animation '{}' could not be optimized", Name)});
		}

		const ozz::unique_ptr<ozz::animation::Animation> Built = Offline::AnimationBuilder()(Optimized);
		if (!Built)
		{
			return std::unexpected(FAssetError{std::format("Animation '{}' could not be built", Name)});
		}

		FCookedAnimationClip& Clip = Result.Clips.emplace_back();
		Clip.Name = Name;
		Clip.DurationSeconds = Raw.duration;
		Clip.Animation = SaveArchive(*Built);

		const bool bTravels = std::ranges::any_of(Motion.keyframes, [](const auto& Key)
		{
			return std::hypot(Key.value.x, Key.value.z) > MinimumRootTravelMeters;
		});

		if (bTravels)
		{
			const ozz::unique_ptr<ozz::animation::Float3Track> Track = Offline::TrackBuilder()(Motion);
			if (!Track)
			{
				return std::unexpected(FAssetError{std::format("Animation '{}' root motion could not be built", Name)});
			}

			Clip.RootMotion = SaveArchive(*Track);
		}

		if (Result.Clips.size() > MaximumCookedClips)
		{
			return std::unexpected(FAssetError{std::format("glTF has more than {} animations", MaximumCookedClips)});
		}
	}

	return Result;
}
}
