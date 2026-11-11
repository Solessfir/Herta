"""Authors the Mannequin's Dash and Dash_Back clips in Blender and adds them to Content/Characters/Mannequin/Mannequin.glb.

Each clip holds one pose built from Idle_Loop's first frame by turning joints about the character's own axes, eased in over two frames and
held, then planted so the lowest toe stands on the ground. Blender only carries the keyframes: the clips go out through its glTF exporter,
and their channels are copied into the model by joint name, so the mesh, skin, and every other clip keep their bytes. Run it from this
folder with Blender 5.2:

    blender --background --factory-startup --python AuthorDashClips.py

Running it again replaces the two clips.
"""

import json
import math
import struct
import tempfile
from pathlib import Path

import bpy
from mathutils import Matrix, Vector

MODEL = Path(__file__).resolve().parent.parent / "Content" / "Characters" / "Mannequin" / "Mannequin.glb"
CLIPS = ("Dash", "Dash_Back")
# The pose eases in, holds, and lets go a little: (frame at 24 fps, strength).
KEYS = ((0, 0.55), (2, 1.0), (6, 1.0), (8, 0.8))
COMPONENTS = {"SCALAR": 1, "VEC3": 3, "VEC4": 4}


def dash(pitch, forward, s):
    # Leaned into a long stride, arms swept back.
    return {
        "spine_01": [(pitch, 22 * s)], "spine_02": [(pitch, 14 * s)], "spine_03": [(pitch, 8 * s)],
        "neck_01": [(pitch, -22 * s)], "Head": [(pitch, -12 * s)],
        "thigh_r": [(pitch, -72 * s)], "calf_r": [(pitch, 62 * s)], "foot_r": [(pitch, 8 * s)],
        "thigh_l": [(pitch, 58 * s)], "calf_l": [(pitch, 10 * s)], "foot_l": [(pitch, 35 * s)],
        "upperarm_l": [(pitch, 32 * s), (forward, 22 * s)], "upperarm_r": [(pitch, 32 * s), (forward, -22 * s)],
        "lowerarm_l": [(pitch, 8 * s)], "lowerarm_r": [(pitch, 8 * s)],
    }


def dash_back(pitch, forward, s):
    # A crouched backstep: knees bent, chest over them, guard up.
    return {
        "spine_01": [(pitch, 14 * s)], "spine_02": [(pitch, 8 * s)], "spine_03": [(pitch, 4 * s)],
        "neck_01": [(pitch, -14 * s)], "Head": [(pitch, -6 * s)],
        "thigh_l": [(pitch, -48 * s)], "calf_l": [(pitch, 70 * s)], "foot_l": [(pitch, -15 * s)],
        "thigh_r": [(pitch, -20 * s)], "calf_r": [(pitch, 75 * s)], "foot_r": [(pitch, 20 * s)],
        "upperarm_l": [(pitch, -40 * s), (forward, 18 * s)], "upperarm_r": [(pitch, -40 * s), (forward, -18 * s)],
        "lowerarm_l": [(pitch, -45 * s)], "lowerarm_r": [(pitch, -45 * s)],
    }


def read_glb(path):
    data = Path(path).read_bytes()
    length = struct.unpack_from("<I", data, 12)[0]
    gltf = json.loads(data[20:20 + length])
    offset = 20 + length
    size = struct.unpack_from("<I", data, offset)[0]
    return gltf, bytearray(data[offset + 8:offset + 8 + size])


def write_glb(path, gltf, binary):
    while len(binary) % 4:
        binary.append(0)
    gltf["buffers"][0]["byteLength"] = len(binary)
    text = json.dumps(gltf, separators=(",", ":")).encode()
    while len(text) % 4:
        text += b" "
    with open(path, "wb") as out:
        out.write(struct.pack("<III", 0x46546C67, 2, 12 + 8 + len(text) + 8 + len(binary)))
        out.write(struct.pack("<II", len(text), 0x4E4F534A) + text)
        out.write(struct.pack("<II", len(binary), 0x004E4942) + binary)


def accessor_values(gltf, binary, index):
    accessor = gltf["accessors"][index]
    view = gltf["bufferViews"][accessor["bufferView"]]
    width = COMPONENTS[accessor["type"]]
    stride = view.get("byteStride", 4 * width)
    start = view.get("byteOffset", 0) + accessor.get("byteOffset", 0)
    return [struct.unpack_from(f"<{width}f", binary, start + i * stride) for i in range(accessor["count"])]


def channels(gltf, binary, name):
    animation = next(a for a in gltf["animations"] if a["name"] == name)
    result = {}
    for channel in animation["channels"]:
        node = gltf["nodes"][channel["target"]["node"]].get("name")
        sampler = animation["samplers"][channel["sampler"]]
        result[(node, channel["target"]["path"])] = (accessor_values(gltf, binary, sampler["input"]), accessor_values(gltf, binary, sampler["output"]), sampler.get("interpolation", "LINEAR"))
    return result


def author(source):
    """Imports the model, keys the clips, and exports them through a temporary .glb, whose path it returns."""
    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.ops.import_scene.gltf(filepath=str(source))
    scene = bpy.context.scene
    armature = next(o for o in bpy.data.objects if o.type == "ARMATURE")
    bones = armature.pose.bones
    order = [bone.name for bone in armature.data.bones]
    up = Vector((0, 0, 1))
    forward = armature.data.bones["ball_l"].head_local - armature.data.bones["foot_l"].head_local
    forward.z = 0
    forward.normalize()
    # Turning about this axis tips the top of a joint toward the front and swings a hanging limb back.
    pitch = up.cross(forward)
    ground = min(armature.data.bones[name].head_local.z for name in ("ball_l", "ball_r"))

    def use(action):
        armature.animation_data.action = action
        if action.slots:
            armature.animation_data.action_slot = action.slots[0]

    idle = bpy.data.actions["Idle_Loop"]
    use(idle)
    scene.frame_set(int(idle.frame_range[0]))
    bpy.context.view_layer.update()
    base = {name: bones[name].matrix.copy() for name in order}

    for clip, pose in zip(CLIPS, (dash, dash_back)):
        action = bpy.data.actions.new(clip)
        use(action)
        for frame, strength in KEYS:
            for name in order:
                bones[name].matrix = base[name]
                bpy.context.view_layer.update()

            # Turns in armature space about each joint's head, parents first, so the axes mean the same on every bone.
            edits = pose(pitch, forward, strength)
            for name in order:
                if name in edits:
                    bone = bones[name]
                    head = bone.matrix.translation.copy()
                    turn = Matrix.Identity(4)
                    for axis, degrees in edits[name]:
                        turn = Matrix.Rotation(math.radians(degrees), 4, axis) @ turn
                    bone.matrix = Matrix.Translation(head) @ turn @ Matrix.Translation(-head) @ bone.matrix
                    bpy.context.view_layer.update()

            lowest = min(bones[name].matrix.translation.z for name in ("ball_l", "ball_r", "foot_l", "foot_r"))
            pelvis = bones["pelvis"].matrix.copy()
            pelvis.translation.z += ground - lowest
            bones["pelvis"].matrix = pelvis
            bpy.context.view_layer.update()
            for name in order:
                bones[name].rotation_mode = "QUATERNION"
                bones[name].keyframe_insert("rotation_quaternion", frame=frame, group=name)
                bones[name].keyframe_insert("location", frame=frame, group=name)

    for action in bpy.data.actions:
        action.use_fake_user = action.name in CLIPS

    armature.animation_data.action = None
    exported = Path(tempfile.mkdtemp()) / "Clips.glb"
    bpy.ops.export_scene.gltf(filepath=str(exported), export_animations=True, export_animation_mode="ACTIONS", export_yup=True, export_force_sampling=True, export_optimize_animation_size=False)
    return exported


def merge(exported):
    gltf, binary = read_glb(MODEL)
    clips, clips_binary = read_glb(exported)
    joints = {gltf["nodes"][index]["name"]: index for index in gltf["skins"][0]["joints"]}

    # Every joint's first key of a clip both files hold must agree, which proves the export's joint frames are the model's. Later keys are not
    # compared: Blender resamples clips from zero at its own frame rate, so they fall between the model's keys.
    worst = 0.0
    original, roundtrip = channels(gltf, binary, "Idle_Loop"), channels(clips, clips_binary, "Idle_Loop")
    for key, (_, outputs, _) in original.items():
        if key[0] in joints and key in roundtrip:
            value, other = outputs[0], roundtrip[key][1][0]
            difference = 2 * math.acos(min(abs(sum(a * b for a, b in zip(value, other))), 1.0)) if key[1] == "rotation" else max(abs(a - b) for a, b in zip(value, other))
            worst = max(worst, difference)

    if worst > 1e-3:
        raise RuntimeError(f"The export's joint frames differ from the model's by {worst}")

    # Rerunning replaces the clips; their old keyframe bytes stay unreferenced in the buffer until the next full merge.
    gltf["animations"] = [animation for animation in gltf["animations"] if animation["name"] not in CLIPS]
    for clip in CLIPS:
        animation = {"name": clip, "channels": [], "samplers": []}
        for (node, path), (times, outputs, interpolation) in sorted(channels(clips, clips_binary, clip).items()):
            if node not in joints or path not in ("rotation", "translation"):
                continue

            indices = []
            for data, kind in ((times, "SCALAR"), (outputs, "VEC4" if path == "rotation" else "VEC3")):
                while len(binary) % 4:
                    binary.append(0)
                offset = len(binary)
                for item in data:
                    binary += struct.pack(f"<{len(item)}f", *item)
                gltf["bufferViews"].append({"buffer": 0, "byteOffset": offset, "byteLength": len(binary) - offset})
                accessor = {"bufferView": len(gltf["bufferViews"]) - 1, "componentType": 5126, "count": len(data), "type": kind}
                if kind == "SCALAR":
                    accessor["min"], accessor["max"] = [min(t[0] for t in data)], [max(t[0] for t in data)]
                gltf["accessors"].append(accessor)
                indices.append(len(gltf["accessors"]) - 1)

            animation["samplers"].append({"input": indices[0], "output": indices[1], "interpolation": interpolation})
            animation["channels"].append({"sampler": len(animation["samplers"]) - 1, "target": {"node": joints[node], "path": path}})
        gltf["animations"].append(animation)
        print(f"{clip}: {len(animation['channels'])} channels")

    write_glb(MODEL, gltf, binary)
    print(f"Wrote {MODEL} (joint frames agree to {worst:.6f})")


merge(author(MODEL))
