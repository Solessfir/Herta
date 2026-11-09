# Mannequin

`Mannequin.glb` is the Mannequin character and a subset of the clips from Quaternius's Universal Animation Library, Standard edition, released under CC0 1.0 (see `License.txt`).

- Source: https://opengameart.org/node/174563, file `universal_animation_librarystandard.zip`, SHA-256 `18ff1a7215f4852b320203e8aaf02a1578b5c8eef9027fbaedfcedc7b85a3ac2`.
- Copied from `Animation Library[Standard]/Godot/AnimationLibrary_Godot_Standard.glb` and `Animation Library[Standard]/License.txt`.
- Kept clips: `Idle_Loop`, `Walk_Loop`, `Jog_Fwd_Loop`, `Sprint_Loop`, `Jump_Start`, `Jump_Loop`, `Jump_Land`, `Crouch_Idle_Loop`, `Crouch_Fwd_Loop`, `Roll`, and `Roll_RM`.

The other 35 clips were removed without re-exporting: the binary chunk was repacked with only the accessors and buffer views the remaining mesh, skin, and clips reference, so the rig, mesh, and kept keyframes are byte-identical to the source. To update, download the archive, check its hash, and repeat the trim with the same clip list.
