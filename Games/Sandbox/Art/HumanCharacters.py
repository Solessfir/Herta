# Builds the Sandbox's human characters with MPFB (MakeHuman for Blender) and gives them the Mannequin's clips.
#
#   blender.exe --background --python Art/HumanCharacters.py -- [preview folder]
#
# Needs the MPFB extension with the CC0 MakeHuman system assets pack installed; writes Content/Characters/Humans/HumanMale.glb and
# HumanFemale.glb. Each body uses MPFB's game engine rig, whose joints carry the Mannequin's names, and takes every clip from
# Content/Characters/Mannequin/Mannequin.glb retargeted in world space by joint name, with the root and pelvis moving in proportion to the
# body's hip height.

import bisect
import json
import os
import struct
import sys

import bpy
from bl_ext.blender_org.mpfb.services.humanservice import HumanService
from mathutils import Quaternion, Vector

HERE = os.path.dirname(os.path.abspath(__file__))
CONTENT = os.path.join(HERE, "..", "Content", "Characters")
MANNEQUIN = os.path.join(CONTENT, "Mannequin", "Mannequin.glb")
OUTPUT = os.path.join(CONTENT, "Humans")

# Average adults: MakeHuman's macros run from 0 to 1, with 0.5 the population average; proportions above it read as idealized rather than
# stylized. Assets are fragments of the system assets pack.
CHARACTERS = {
    "HumanMale": {
        "phenotype": {"gender": 1.0, "age": 0.5, "muscle": 0.6, "weight": 0.5, "proportions": 0.7, "height": 0.6, "cupsize": 0.5, "firmness": 0.5, "race": {"asian": 0.1, "caucasian": 0.8, "african": 0.1}},
        "skin_mhmat": "young_caucasian_male/young_caucasian_male.mhmat",
        "hair": "short02/short02.mhclo",
        "eyebrows": "eyebrow001/eyebrow001.mhclo",
        "clothes": ["male_casualsuit04/male_casualsuit04.mhclo", "shoes02/shoes02.mhclo"],
    },
    "HumanFemale": {
        "phenotype": {"gender": 0.0, "age": 0.5, "muscle": 0.55, "weight": 0.45, "proportions": 0.7, "height": 0.45, "cupsize": 0.5, "firmness": 0.6, "race": {"asian": 0.1, "caucasian": 0.8, "african": 0.1}},
        "skin_mhmat": "young_caucasian_female/young_caucasian_female.mhmat",
        "hair": "ponytail01/ponytail01.mhclo",
        "eyebrows": "eyebrow010/eyebrow010.mhclo",
        "clothes": ["female_sportsuit01/female_sportsuit01.mhclo", "shoes06/shoes06.mhclo"],
    },
}

# Joints whose clip translation moves the body as a whole; every other joint keeps the body's own rest offset.
MOVING_JOINTS = {"root", "pelvis"}


def build(name, recipe):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    info = HumanService._create_default_human_info_dict()
    info.update({
        "name": name,
        "phenotype": recipe["phenotype"],
        "rig": "game_engine",
        "eyes": "low-poly/low-poly.mhclo",
        "eyelashes": "eyelashes01/eyelashes01.mhclo",
        "eyebrows": recipe["eyebrows"],
        "hair": recipe["hair"],
        "clothes": recipe["clothes"],
        "skin_mhmat": recipe["skin_mhmat"],
        "skin_material_type": "MAKESKIN",
        "eyes_material_type": "MAKESKIN",
        "alternative_materials": {},
    })

    settings = HumanService.get_default_deserialization_settings()
    # No subdivision: the game draws the base mesh's own resolution.
    settings["subdiv_levels"] = 0
    return HumanService.deserialize_from_dict(info, settings)


def export(path):
    for obj in bpy.context.scene.objects:
        obj.select_set(obj.type in {"MESH", "ARMATURE"})

    # Modifiers apply on export, so the masks MPFB puts on the body under clothes remove those faces from the game mesh.
    bpy.ops.export_scene.gltf(filepath=path, export_format="GLB", use_selection=True, export_apply=True, export_animations=False, export_skins=True, export_tangents=True, export_yup=True, export_image_format="AUTO")


def read_glb(path):
    data = open(path, "rb").read()
    json_length = struct.unpack_from("<I", data, 12)[0]
    document = json.loads(data[20:20 + json_length])
    binary = b""
    offset = 20 + json_length
    if offset < len(data):
        binary_length = struct.unpack_from("<I", data, offset)[0]
        binary = data[offset + 8:offset + 8 + binary_length]
    return document, bytearray(binary)


def write_glb(path, document, binary):
    while len(binary) % 4:
        binary.append(0)
    document["buffers"] = [{"byteLength": len(binary)}]
    text = json.dumps(document, separators=(",", ":")).encode()
    text += b" " * ((4 - len(text) % 4) % 4)
    with open(path, "wb") as file:
        file.write(struct.pack("<III", 0x46546C67, 2, 12 + 8 + len(text) + 8 + len(binary)))
        file.write(struct.pack("<II", len(text), 0x4E4F534A) + text)
        file.write(struct.pack("<II", len(binary), 0x004E4942) + bytes(binary))


def accessor_floats(document, binary, index):
    accessor = document["accessors"][index]
    view = document["bufferViews"][accessor["bufferView"]]
    width = {"SCALAR": 1, "VEC3": 3, "VEC4": 4}[accessor["type"]]
    start = view.get("byteOffset", 0) + accessor.get("byteOffset", 0)
    stride = view.get("byteStride", 4 * width)
    return [list(struct.unpack_from("<%df" % width, binary, start + i * stride)) for i in range(accessor["count"])], accessor["type"]


def append_accessor(document, binary, rows, kind):
    while len(binary) % 4:
        binary.append(0)
    offset = len(binary)
    for row in rows:
        binary.extend(struct.pack("<%df" % len(row), *row))
    document["bufferViews"].append({"buffer": 0, "byteOffset": offset, "byteLength": len(binary) - offset})
    accessor = {"bufferView": len(document["bufferViews"]) - 1, "componentType": 5126, "count": len(rows), "type": kind}
    if kind == "SCALAR":
        accessor["min"] = [min(row[0] for row in rows)]
        accessor["max"] = [max(row[0] for row in rows)]
    document["accessors"].append(accessor)
    return len(document["accessors"]) - 1


def node_parents(document):
    parents = {}
    for index, node in enumerate(document["nodes"]):
        for child in node.get("children", []):
            parents[child] = index
    return parents


def rest_world(document, parents):
    # World rest rotation and position of every node, through every ancestor, including the armature's own node.
    cache = {}

    def world(index):
        if index not in cache:
            node = document["nodes"][index]
            x, y, z, w = node.get("rotation", [0.0, 0.0, 0.0, 1.0])
            rotation = Quaternion((w, x, y, z))
            translation = Vector(node.get("translation", [0.0, 0.0, 0.0]))
            if index in parents:
                parent_rotation, parent_translation = world(parents[index])
                cache[index] = (parent_rotation @ rotation, parent_translation + parent_rotation @ translation)
            else:
                cache[index] = (rotation, translation)
        return cache[index]

    return {index: world(index) for index in range(len(document["nodes"]))}


def sample(times, values, time, rotation):
    if len(times) == 1 or time <= times[0]:
        index, alpha = 0, 0.0
    elif time >= times[-1]:
        index, alpha = len(times) - 2, 1.0
    else:
        index = bisect.bisect_right(times, time) - 1
        alpha = (time - times[index]) / max(times[index + 1] - times[index], 1e-9)
    a = values[index]
    b = values[min(index + 1, len(values) - 1)]
    if rotation:
        return Quaternion((a[3], a[0], a[1], a[2])).slerp(Quaternion((b[3], b[0], b[1], b[2])), alpha)
    return Vector(a).lerp(Vector(b), alpha)


def transfer_clips(path):
    # Retargets in world space: each target joint takes the source joint's world rotation times a fixed offset that maps the source's rest
    # frame onto the target's rest frame aimed along the source bone. The rigs differ in rest pose (T against A), in how the pelvis and
    # clavicles are oriented, and in twist, so copying local rotations by name would bend and twist the body.
    target, target_binary = read_glb(path)
    source, source_binary = read_glb(MANNEQUIN)
    source_parents, target_parents = node_parents(source), node_parents(target)
    source_rest, target_rest = rest_world(source, source_parents), rest_world(target, target_parents)
    source_joints = {source["nodes"][joint]["name"].lower(): joint for joint in source["skins"][0]["joints"]}
    order = target["skins"][0]["joints"]
    names = {joint: target["nodes"][joint]["name"].lower() for joint in order}
    for joint in order:
        node = target["nodes"][joint]
        node["name"] = {"Root": "root", "head": "Head"}.get(node.get("name"), node.get("name"))

    # Hair, eyebrows, and eyelashes export as blended cards; Herta draws cutouts, so they become alpha-tested at the same edge.
    for material in target["materials"]:
        if material.get("alphaMode") == "BLEND":
            material["alphaMode"] = "MASK"
            material["alphaCutoff"] = 0.5

    def direction(rotation):
        return rotation @ Vector((0.0, 1.0, 0.0))

    offsets = {}
    for joint in order:
        if names[joint] in source_joints:
            source_rotation = source_rest[source_joints[names[joint]]][0]
            target_rotation = target_rest[joint][0]
            aim = direction(target_rotation).rotation_difference(direction(source_rotation))
            offsets[joint] = source_rotation.inverted() @ aim @ target_rotation

    pelvis = next(joint for joint in order if names[joint] == "pelvis")
    # Hip height above the root sets how far the body travels; glTF is Y-up.
    scale = target_rest[pelvis][1].y / max(source_rest[source_joints["pelvis"]][1].y, 1e-6)
    target["animations"] = []
    for clip in source["animations"]:
        tracks = {}
        times = set()
        for channel in clip["channels"]:
            sampler = clip["samplers"][channel["sampler"]]
            keys, _ = accessor_floats(source, source_binary, sampler["input"])
            values, _ = accessor_floats(source, source_binary, sampler["output"])
            tracks[(channel["target"]["node"], channel["target"]["path"])] = ([key[0] for key in keys], values)
            times.update(key[0] for key in keys)

        times = sorted(times)
        rotations = {joint: [] for joint in order}
        translations = {joint: [] for joint in order if names[joint] in MOVING_JOINTS and names[joint] in source_joints}
        for time in times:
            source_world = {}

            def world(index):
                if index not in source_world:
                    node = source["nodes"][index]
                    track = tracks.get((index, "rotation"))
                    x, y, z, w = node.get("rotation", [0.0, 0.0, 0.0, 1.0])
                    rotation = sample(track[0], track[1], time, True) if track else Quaternion((w, x, y, z))
                    track = tracks.get((index, "translation"))
                    translation = sample(track[0], track[1], time, False) if track else Vector(node.get("translation", [0.0, 0.0, 0.0]))
                    if index in source_parents:
                        parent_rotation, parent_translation = world(source_parents[index])
                        source_world[index] = (parent_rotation @ rotation, parent_translation + parent_rotation @ translation)
                    else:
                        source_world[index] = (rotation, translation)
                return source_world[index]

            target_world = {}
            for joint in order:
                parent = target_parents.get(joint)
                if parent in target_world:
                    parent_rotation, parent_translation = target_world[parent]
                elif parent is not None:
                    parent_rotation, parent_translation = target_rest[parent]
                else:
                    parent_rotation, parent_translation = Quaternion(), Vector()

                rotation = world(source_joints[names[joint]])[0] @ offsets[joint] if joint in offsets else parent_rotation @ target_rest[joint][0]
                if joint in translations:
                    position = world(source_joints[names[joint]])[1] * scale
                    translations[joint].append(list(parent_rotation.inverted() @ (position - parent_translation)))
                else:
                    position = parent_translation + parent_rotation @ Vector(target["nodes"][joint].get("translation", [0.0, 0.0, 0.0]))

                target_world[joint] = (rotation, position)
                local = parent_rotation.inverted() @ rotation
                rotations[joint].append([local.x, local.y, local.z, local.w])

        keys = append_accessor(target, target_binary, [[time] for time in times], "SCALAR")
        channels = []
        samplers = []
        for joint in order:
            samplers.append({"input": keys, "output": append_accessor(target, target_binary, rotations[joint], "VEC4"), "interpolation": "LINEAR"})
            channels.append({"sampler": len(samplers) - 1, "target": {"node": joint, "path": "rotation"}})
            if joint in translations:
                samplers.append({"input": keys, "output": append_accessor(target, target_binary, translations[joint], "VEC3"), "interpolation": "LINEAR"})
                channels.append({"sampler": len(samplers) - 1, "target": {"node": joint, "path": "translation"}})

        target["animations"].append({"name": clip["name"], "channels": channels, "samplers": samplers})

    write_glb(path, target, target_binary)
    return len(source["animations"])


def preview(directory, name):
    # A full-body and a face shot in the rest pose, to check the body, skin, hair, and clothes before cooking.
    scene = bpy.context.scene
    scene.render.engine = "BLENDER_EEVEE"
    scene.render.resolution_x = 900
    scene.render.resolution_y = 1100
    world = bpy.data.worlds.new("World")
    world.use_nodes = True
    world.node_tree.nodes["Background"].inputs["Color"].default_value = (0.2, 0.21, 0.23, 1.0)
    scene.world = world
    for energy, location in ((300.0, (1.5, -2.5, 2.5)), (120.0, (-2.0, -1.5, 1.5)), (200.0, (0.0, 2.5, 2.5))):
        light = bpy.data.lights.new("Light", "AREA")
        light.energy = energy
        light.size = 2.0
        obj = bpy.data.objects.new("Light", light)
        obj.location = location
        obj.rotation_euler = (Vector((0.0, 0.0, 1.0)) - Vector(location)).to_track_quat("-Z", "Y").to_euler()
        scene.collection.objects.link(obj)

    camera = bpy.data.cameras.new("Camera")
    camera.lens = 60
    obj = bpy.data.objects.new("Camera", camera)
    scene.collection.objects.link(obj)
    scene.camera = obj
    for suffix, location, target in (("front", (0.0, -4.2, 0.95), (0.0, 0.0, 0.9)), ("face", (0.35, -1.1, 1.6), (0.0, 0.0, 1.55))):
        obj.location = location
        obj.rotation_euler = (Vector(target) - Vector(location)).to_track_quat("-Z", "Y").to_euler()
        scene.render.filepath = os.path.join(directory, "%s-%s.png" % (name, suffix))
        bpy.ops.render.render(write_still=True)


def main():
    arguments = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    os.makedirs(OUTPUT, exist_ok=True)
    for name, recipe in CHARACTERS.items():
        build(name, recipe)
        path = os.path.join(OUTPUT, name + ".glb")
        export(path)
        print("%s: %d clips" % (name, transfer_clips(path)), flush=True)
        if arguments:
            preview(arguments[0], name)


main()
