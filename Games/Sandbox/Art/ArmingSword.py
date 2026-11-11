# Builds the Sandbox's arming sword and exports it as Content/Weapons/ArmingSword.glb.
#
#   blender.exe --background --factory-startup --python Art/ArmingSword.py -- Content/Weapons/ArmingSword.glb [preview.png]
#
# The model is in grip space: the origin is the centre of the grip, where the hand holds it, the blade runs along +Z in Blender (+Y in
# glTF and Herta) from the guard to the tip, its edges face +X and -X, and its flats face +Y and -Y. The blade ends at 1 m, where the
# game's hit sweep ends.

import math
import sys

import bmesh
import bpy
from mathutils import Matrix, Vector

BLADE_START = 0.10
BLADE_END = 1.0
GRIP_BOTTOM = -0.085
GRIP_TOP = 0.07


def lerp(a, b, t):
    return a + (b - a) * t


def smoothstep(a, b, x):
    t = min(max((x - a) / (b - a), 0.0), 1.0)
    return t * t * (3.0 - 2.0 * t)


def material(name, color, metallic, roughness):
    mat = bpy.data.materials.new(name)
    mat.use_nodes = True
    shader = mat.node_tree.nodes["Principled BSDF"]
    shader.inputs["Base Color"].default_value = (*color, 1.0)
    shader.inputs["Metallic"].default_value = metallic
    shader.inputs["Roughness"].default_value = roughness
    return mat


def loft(name, sections, mat, cap_start=True, cap_end=True):
    # Joins rings of equal size into a closed surface; each ring is a list of points wound the same way.
    mesh = bpy.data.meshes.new(name)
    bm = bmesh.new()
    rings = [[bm.verts.new(point) for point in ring] for ring in sections]
    count = len(rings[0])
    for a, b in zip(rings, rings[1:]):
        for i in range(count):
            j = (i + 1) % count
            bm.faces.new((a[i], a[j], b[j], b[i]))

    if cap_start:
        bm.faces.new(list(reversed(rings[0])))

    if cap_end:
        bm.faces.new(rings[-1])

    bmesh.ops.recalc_face_normals(bm, faces=bm.faces)
    bm.to_mesh(mesh)
    bm.free()
    obj = bpy.data.objects.new(name, mesh)
    bpy.context.collection.objects.link(obj)
    obj.data.materials.append(mat)
    return obj


def blade(mat):
    # A type XII-style blade: broad at the guard, tapering to a reinforced point, with a fuller down the first two thirds. Each station
    # is a lens-shaped section with a groove for the fuller; the edges keep a hair of thickness so they stay sharp without degenerating.
    stations = 96
    across = 28
    sections = []
    # The blade starts inside the guard, so no seam shows where they meet.
    root = BLADE_START - 0.02
    length = BLADE_END - root
    for s in range(stations + 1):
        t = s / stations
        z = root + length * t
        # Width tapers gently, then the last sixth curves into the point.
        width = lerp(0.052, 0.036, t)
        point = smoothstep(0.82, 1.0, t)
        width *= math.sqrt(max(1.0 - point, 0.0)) if t < 1.0 else 0.0
        width = max(width, 0.0012)
        thickness = lerp(0.0075, 0.0042, t) * (1.0 - 0.45 * point)
        fuller = (1.0 - smoothstep(0.55, 0.7, t)) * smoothstep(0.0, 0.03, t)
        ring = []
        for i in range(across):
            angle = 2.0 * math.pi * i / across
            x = 0.5 * width * math.cos(angle)
            u = abs(x) / (0.5 * width)
            # A flattened lens: thickest in the middle, tapering to the edge bevel.
            profile = 0.5 * thickness * (1.0 - u ** 1.6) ** 0.85 + 0.0002
            groove = fuller * 0.0016 * math.exp(-((u / 0.28) ** 2))
            y = math.copysign(max(profile - groove, 0.00025), math.sin(angle) if abs(math.sin(angle)) > 1e-6 else 1.0)
            if abs(math.sin(angle)) < 1e-6:
                y = 0.0
            ring.append(Vector((x, y, z)))
        sections.append(ring)

    obj = loft("Blade", sections, mat, cap_start=True, cap_end=False)
    # The point collapses to a line; merge it so the tip is clean.
    bm = bmesh.new()
    bm.from_mesh(obj.data)
    bmesh.ops.remove_doubles(bm, verts=bm.verts, dist=0.00005)
    bm.to_mesh(obj.data)
    bm.free()
    return obj


def rounded_rect(width, height, radius, segments=4):
    points = []
    hw = width / 2 - radius
    hh = height / 2 - radius
    for cx, cy, start in ((hw, hh, 0.0), (-hw, hh, 90.0), (-hw, -hh, 180.0), (hw, -hh, 270.0)):
        for i in range(segments + 1):
            a = math.radians(start + 90.0 * i / segments)
            points.append((cx + radius * math.cos(a), cy + radius * math.sin(a)))
    return points


def guard(mat):
    # A straight cross with quillons that thin outward and curl slightly toward the blade, ending in small knobs.
    stations = 60
    span = 0.21
    sections = []
    for s in range(stations + 1):
        t = s / stations
        x = lerp(-span / 2, span / 2, t)
        u = abs(2.0 * t - 1.0)
        # Swells at the centre block and at each terminal.
        knob = math.exp(-(((u - 0.97) / 0.035) ** 2))
        block = math.exp(-((u / 0.12) ** 2))
        depth = 0.016 + 0.008 * block + 0.006 * knob - 0.004 * u
        height = 0.015 + 0.007 * block + 0.007 * knob - 0.004 * u
        lift = 0.010 * u ** 2.2
        ring = []
        for py, pz in rounded_rect(depth, height, min(depth, height) * 0.38):
            ring.append(Vector((x, py, BLADE_START - 0.012 + pz + lift)))
        sections.append(ring)

    return loft("Guard", sections, mat)


def grip(leather):
    # An oval leather grip that swells slightly in the middle, laid over cord risers at both ends and the middle.
    stations = 96
    around = 40
    sections = []
    for s in range(stations + 1):
        t = s / stations
        z = lerp(GRIP_BOTTOM, GRIP_TOP, t)
        swell = 1.0 + 0.10 * math.sin(math.pi * t)
        risers = sum(math.exp(-(((t - c) / 0.03) ** 2)) for c in (0.06, 0.5, 0.94))
        rx = 0.0145 * swell + 0.0013 * risers
        ry = 0.0118 * swell + 0.0013 * risers
        ring = []
        for i in range(around):
            a = 2.0 * math.pi * i / around
            ring.append(Vector((rx * math.cos(a), ry * math.sin(a), z)))
        sections.append(ring)

    return loft("Grip", sections, leather)


def pommel(mat):
    # A wheel pommel with a raised boss on each face, and the peen block that holds it on the tang.
    bpy.ops.mesh.primitive_cylinder_add(vertices=48, radius=0.03, depth=0.024, location=(0.0, 0.0, GRIP_BOTTOM - 0.026), rotation=(math.radians(90), 0.0, 0.0))
    wheel = bpy.context.object
    wheel.name = "Pommel"
    bevel = wheel.modifiers.new("Bevel", "BEVEL")
    bevel.width = 0.004
    bevel.segments = 4
    bevel.limit_method = "ANGLE"

    for side in (-1.0, 1.0):
        bpy.ops.mesh.primitive_cylinder_add(vertices=40, radius=0.016, depth=0.006, location=(0.0, side * 0.0135, GRIP_BOTTOM - 0.026), rotation=(math.radians(90), 0.0, 0.0))
        boss = bpy.context.object
        boss_bevel = boss.modifiers.new("Bevel", "BEVEL")
        boss_bevel.width = 0.0025
        boss_bevel.segments = 3
        boss.select_set(True)

    bpy.ops.mesh.primitive_uv_sphere_add(segments=24, ring_count=12, radius=0.0065, location=(0.0, 0.0, GRIP_BOTTOM - 0.056))
    bpy.context.object.scale = (1.0, 1.0, 0.7)

    for obj in [o for o in bpy.context.scene.objects if o.type == "MESH" and o.name.startswith(("Pommel", "Cylinder", "Sphere"))]:
        bpy.context.view_layer.objects.active = obj
        for modifier in list(obj.modifiers):
            bpy.ops.object.modifier_apply(modifier=modifier.name)
        obj.data.materials.clear()
        obj.data.materials.append(mat)

    return wheel


def collar(mat):
    # A thin metal band where the grip meets the guard.
    bpy.ops.mesh.primitive_cylinder_add(vertices=40, radius=0.0175, depth=0.01, location=(0.0, 0.0, GRIP_TOP + 0.004))
    band = bpy.context.object
    band.name = "Collar"
    band.scale = (1.0, 0.82, 1.0)
    bevel = band.modifiers.new("Bevel", "BEVEL")
    bevel.width = 0.0015
    bevel.segments = 2
    bpy.context.view_layer.objects.active = band
    bpy.ops.object.modifier_apply(modifier="Bevel")
    band.data.materials.append(mat)
    return band


def build():
    bpy.ops.wm.read_factory_settings(use_empty=True)
    steel = material("Steel", (0.86, 0.87, 0.89), 1.0, 0.18)
    iron = material("Guard", (0.36, 0.36, 0.38), 1.0, 0.34)
    leather = material("Leather", (0.045, 0.02, 0.011), 0.0, 0.58)

    parts = [blade(steel), guard(iron), grip(leather), collar(iron), pommel(iron)]
    for obj in bpy.context.scene.objects:
        if obj.type == "MESH":
            obj.select_set(True)
            # Smooth shading with hard edges kept where faces meet sharply, such as the blade's edge and the pommel's rim.
            for polygon in obj.data.polygons:
                polygon.use_smooth = True
            bpy.context.view_layer.objects.active = obj
            bpy.ops.object.shade_smooth_by_angle(angle=math.radians(40))

    bpy.context.view_layer.objects.active = parts[0]
    bpy.ops.object.join()
    sword = bpy.context.object
    sword.name = "ArmingSword"
    bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)
    return sword


def preview(path, sword):
    # Two shots, the whole sword and its hilt, with the sword laid along X, one flat facing the camera and tilted a little toward it.
    scene = bpy.context.scene
    scene.render.engine = "BLENDER_EEVEE"
    scene.render.resolution_x = 1600
    scene.render.resolution_y = 700
    world = bpy.data.worlds.new("World")
    world.use_nodes = True
    world.node_tree.nodes["Background"].inputs["Color"].default_value = (0.05, 0.055, 0.065, 1.0)
    scene.world = world
    sword.rotation_euler = (math.radians(20.0), math.radians(90.0), 0.0)
    for energy, location in ((60.0, (0.4, -1.0, 0.8)), (25.0, (-0.6, -0.4, -0.6)), (40.0, (0.2, 0.8, 0.6))):
        light = bpy.data.lights.new("Light", "AREA")
        light.energy = energy
        light.size = 1.5
        obj = bpy.data.objects.new("Light", light)
        obj.location = location
        obj.rotation_euler = (Vector((0.45, 0.0, 0.0)) - Vector(location)).to_track_quat("-Z", "Y").to_euler()
        scene.collection.objects.link(obj)

    camera = bpy.data.cameras.new("Camera")
    camera.type = "ORTHO"
    obj = bpy.data.objects.new("Camera", camera)
    obj.rotation_euler = (math.radians(90.0), 0.0, 0.0)
    scene.collection.objects.link(obj)
    scene.camera = obj
    for suffix, center, scale in (("", 0.45, 1.25), ("-hilt", 0.0, 0.32)):
        camera.ortho_scale = scale
        obj.location = (center, -2.0, 0.0)
        scene.render.filepath = path.replace(".png", suffix + ".png")
        bpy.ops.render.render(write_still=True)


def main():
    arguments = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    if not arguments:
        raise SystemExit("Pass the output .glb after --")

    sword = build()
    bpy.ops.export_scene.gltf(filepath=arguments[0], export_format="GLB", export_tangents=True, export_yup=True, use_selection=False, export_apply=True)
    if len(arguments) > 1:
        preview(arguments[1], sword)


main()
