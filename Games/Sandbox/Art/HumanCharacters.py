# Builds the Sandbox's human character from a sculpted base mesh and gives it the Mannequin's clips.
#
#   blender.exe --background --python Art/HumanCharacters.py -- <main1.obj> [preview folder]
#
# The sculpt is a CC0 high-poly ZBrush base mesh with no UVs or rig (see Content/Characters/Humans/Source.md). The script decimates it to a
# game budget, and rigs it with MPFB's game engine rig, whose joints carry the Mannequin's names: an MPFB male is posed onto the sculpt's
# arms and legs, that pose becomes the rest pose, and its skin weights transfer to the sculpt by nearest surface. Every clip from
# Content/Characters/Mannequin/Mannequin.glb is then retargeted in world space by joint name, with the root and pelvis moving in proportion
# to the hip height. Writes Content/Characters/Humans/HumanMale.glb.

import bisect
import json
import math
import os
import struct
import sys

import bpy
from bl_ext.blender_org.mpfb.services.humanservice import HumanService
from mathutils import Matrix, Quaternion, Vector

HERE = os.path.dirname(os.path.abspath(__file__))
CONTENT = os.path.join(HERE, "..", "Content", "Characters")
MANNEQUIN = os.path.join(CONTENT, "Mannequin", "Mannequin.glb")
OUTPUT = os.path.join(CONTENT, "Humans")
NAME = "HumanMale"
TRIANGLES = 25_000
# Untextured, since the sculpt has no UVs: one skin tone, linear.
SKIN = (0.56, 0.36, 0.27, 1.0)

# Joints whose clip translation moves the body as a whole; every other joint keeps the body's own rest offset.
MOVING_JOINTS = {"root", "pelvis"}


def bounds(obj):
    corners = [obj.matrix_world @ Vector(corner) for corner in obj.bound_box]
    return Vector([min(c[i] for c in corners) for i in range(3)]), Vector([max(c[i] for c in corners) for i in range(3)])


def world_vertices(obj):
    depsgraph = bpy.context.evaluated_depsgraph_get()
    evaluated = obj.evaluated_get(depsgraph)
    mesh = evaluated.to_mesh()
    points = [obj.matrix_world @ v.co for v in mesh.vertices]
    evaluated.to_mesh_clear()
    return points


def extremity(points, side, axis_test):
    # The mean of the outermost points on one side, such as the fingertips (farthest out) or the feet (lowest).
    chosen = sorted((p for p in points if p.x * side > 0.05), key=axis_test)[:200]
    return sum(chosen, Vector()) / len(chosen)


def swing(armature, bone_name, tip_now, tip_wanted):
    # Rotates a pose bone about the view axis so the limb it starts reaches toward tip_wanted, in the frontal plane only.
    bone = armature.pose.bones[bone_name]
    head = armature.matrix_world @ bone.head
    now = Vector((tip_now.x - head.x, tip_now.z - head.z))
    wanted = Vector((tip_wanted.x - head.x, tip_wanted.z - head.z))
    angle = math.atan2(now.x * wanted.y - now.y * wanted.x, now.dot(wanted))
    # A rotation about world Y by angle, applied in the bone's pose space.
    world = Matrix.Rotation(-angle, 4, "Y")
    rest = armature.matrix_world @ bone.matrix
    bone.matrix = armature.matrix_world.inverted() @ (Matrix.Translation(head) @ world @ Matrix.Translation(-head) @ rest)
    bpy.context.view_layer.update()


def build(sculpt_path):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    info = HumanService._create_default_human_info_dict()
    info.update({"name": NAME, "rig": "game_engine", "alternative_materials": {}})
    info["phenotype"]["gender"] = 1.0
    info["phenotype"]["muscle"] = 0.6
    settings = HumanService.get_default_deserialization_settings()
    settings["subdiv_levels"] = 0
    human = HumanService.deserialize_from_dict(info, settings)
    armature = next(obj for obj in bpy.data.objects if obj.type == "ARMATURE")
    for modifier in list(human.modifiers):
        if modifier.type == "MASK":
            human.modifiers.remove(modifier)

    # The sculpt, scaled to the MPFB body's height with its feet on the ground at the origin.
    bpy.ops.wm.obj_import(filepath=sculpt_path)
    sculpt = bpy.context.selected_objects[0]
    sculpt.name = NAME + "Body"
    low, high = bounds(sculpt)
    human_low, human_high = bounds(human)
    scale = (human_high.z - human_low.z) / (high.z - low.z)
    sculpt.scale = (scale, scale, scale)
    bpy.context.view_layer.update()
    low, high = bounds(sculpt)
    sculpt.location = (-(low.x + high.x) * 0.5, -(low.y + high.y) * 0.5, -low.z)
    bpy.context.view_layer.update()
    bpy.context.view_layer.objects.active = sculpt
    bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)

    # Swing the MPFB arms and legs onto the sculpt's in the frontal plane, so the joints sit inside its limbs.
    sculpt_points = world_vertices(sculpt)
    bpy.context.view_layer.objects.active = armature
    bpy.ops.object.mode_set(mode="POSE")
    for side, suffix in ((1.0, "l"), (-1.0, "r")):
        human_points = world_vertices(human)
        swing(armature, "upperarm_" + suffix, extremity(human_points, side, lambda p: -abs(p.x)), extremity(sculpt_points, side, lambda p: -abs(p.x)))
        human_points = world_vertices(human)
        swing(armature, "thigh_" + suffix, extremity(human_points, side, lambda p: p.z), extremity(sculpt_points, side, lambda p: p.z))

    # The posed body becomes the weight source and the pose becomes the rest pose.
    bpy.ops.object.mode_set(mode="OBJECT")
    # The body carries its shape keys, which block applying a modifier, so the evaluated mesh, posed and shaped, replaces its data.
    depsgraph = bpy.context.evaluated_depsgraph_get()
    posed = bpy.data.meshes.new_from_object(human.evaluated_get(depsgraph), preserve_all_data_layers=True, depsgraph=depsgraph)
    human.modifiers.clear()
    human.shape_key_clear()
    human.data = posed
    bpy.context.view_layer.objects.active = armature
    bpy.ops.object.mode_set(mode="POSE")
    bpy.ops.pose.armature_apply(selected=False)
    bpy.ops.object.mode_set(mode="OBJECT")

    # Decimate to the game budget before weights, so the transfer samples the final vertices.
    decimate = sculpt.modifiers.new("Decimate", "DECIMATE")
    decimate.ratio = min(1.0, TRIANGLES / max(len(sculpt.data.polygons), 1))
    bpy.context.view_layer.objects.active = sculpt
    bpy.ops.object.modifier_apply(modifier=decimate.name)
    for polygon in sculpt.data.polygons:
        polygon.use_smooth = True

    for group in human.vertex_groups:
        if armature.data.bones.get(group.name):
            sculpt.vertex_groups.new(name=group.name)
    transfer = sculpt.modifiers.new("Weights", "DATA_TRANSFER")
    transfer.object = human
    transfer.use_vert_data = True
    transfer.data_types_verts = {"VGROUP_WEIGHTS"}
    transfer.vert_mapping = "POLYINTERP_NEAREST"
    transfer.layers_vgroup_select_src = "ALL"
    transfer.layers_vgroup_select_dst = "NAME"
    bpy.ops.object.modifier_apply(modifier=transfer.name)
    bpy.ops.object.vertex_group_limit_total(group_select_mode="ALL", limit=4)
    bpy.ops.object.vertex_group_normalize_all(group_select_mode="ALL", lock_active=False)

    deform = sculpt.modifiers.new("Armature", "ARMATURE")
    deform.object = armature
    sculpt.parent = armature
    material = bpy.data.materials.new("Skin")
    material.use_nodes = True
    shader = material.node_tree.nodes["Principled BSDF"]
    shader.inputs["Base Color"].default_value = SKIN
    shader.inputs["Roughness"].default_value = 0.55
    sculpt.data.materials.clear()
    sculpt.data.materials.append(material)

    # Only the sculpt and the rig leave Blender.
    for obj in list(bpy.data.objects):
        if obj not in (sculpt, armature):
            bpy.data.objects.remove(obj, do_unlink=True)
    print("%s: %d triangles" % (NAME, sum(len(p.vertices) - 2 for p in sculpt.data.polygons)), flush=True)


def export(path):
    for obj in bpy.context.scene.objects:
        obj.select_set(obj.type in {"MESH", "ARMATURE"})

    bpy.ops.export_scene.gltf(filepath=path, export_format="GLB", use_selection=True, export_apply=True, export_animations=False, export_skins=True, export_tangents=False, export_yup=True)


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
    # A full-body and a face shot in the rest pose, to check the body and its shading before cooking.
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
    if not arguments:
        raise SystemExit("Pass the sculpt's .obj after --")

    os.makedirs(OUTPUT, exist_ok=True)
    build(arguments[0])
    path = os.path.join(OUTPUT, NAME + ".glb")
    export(path)
    print("%s: %d clips" % (NAME, transfer_clips(path)), flush=True)
    if len(arguments) > 1:
        preview(arguments[1], NAME)


main()
