# Humans

`HumanMale.glb` and `HumanFemale.glb` are generated with MPFB 2 (MakeHuman for Blender) from MakeHuman's system assets, released under CC0 1.0 (see `License.txt`): average adult bodies, skins, low-poly eyes, eyebrows, eyelashes, hair, and clothes.

Source: https://static.makehumancommunity.org/assets/assetpacks/makehuman_system_assets.html, file `makehuman_system_assets_cc0.zip`, SHA-256 `b542127a8e25547c7c29c19f2d1d2adb9a664c80396ecd694095dbc8028a0107`, installed into MPFB.

`Art/HumanCharacters.py` builds both with Blender 5.3 and MPFB:

```
blender.exe --background --python Art/HumanCharacters.py -- [preview folder]
```

The script sets each body's phenotype, skin, hair, and clothes, rigs it with MPFB's game engine rig, whose joints carry the Mannequin's names, and exports without subdivision. It then retargets every clip of `../Mannequin/Mannequin.glb` in world space: each joint follows the Mannequin joint's world rotation through a fixed offset taken at rest, since the rigs differ in rest pose, pelvis and clavicle orientation, and twist, and the root and pelvis travel in proportion to the hip height. Hair, eyebrow, and eyelash cards become alpha-tested. Both models use the Mannequin's animation graph and severable joints through their metadata.
