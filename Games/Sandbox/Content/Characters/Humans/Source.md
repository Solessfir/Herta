# Humans

`HumanMale.glb` is the player and every guard. It is built from `male-body-base-mesh-highpoly.zip` (SHA-256 `fee7174ff0f928b646928552b3fc97cda31853795d677667622c1e9d2b6bf846`), a CC0 ZBrush base mesh with one OBJ, `source/main1.zip` > `main1.obj`, of 475,650 triangles in an A-pose, with no UVs, textures, or rig; see `License.txt`.

`Art/HumanCharacters.py` builds it with Blender 5.3 and MPFB 2 with the MakeHuman system assets installed:

```
blender.exe --background --python Art/HumanCharacters.py -- <main1.obj> [preview folder]
```

The script scales the sculpt to an MPFB male's height and decimates it to 25,000 triangles. It swings the MPFB male's arms and legs onto the sculpt's, makes that pose the game engine rig's rest pose, and transfers the posed body's skin weights to the sculpt by nearest surface, four per vertex. Every clip of `../Mannequin/Mannequin.glb` is then retargeted in world space by joint name, with the root and pelvis travelling in proportion to the hip height. Without UVs the body is one untextured skin material. It uses the Mannequin's animation graph and severable joints through its metadata.
