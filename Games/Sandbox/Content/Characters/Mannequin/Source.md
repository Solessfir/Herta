# Mannequin

`Mannequin.glb` combines Quaternius's Universal Animation Library and Universal Animation Library 2, Standard editions, both released under CC0 1.0 (see `License.txt`). Both libraries ship the same Unreal-style 65-joint rig, so every clip plays on one skeleton.

Sources:

- Universal Animation Library 2: https://opengameart.org/content/universal-animation-library-2, file `universal_animation_library_2standard.zip`, SHA-256 `ec0e40d6d78fe9aaad59e322f40865a8675c22f0745e291622e54520391a9217`. The mesh, skin, and the clips `Slide_Start`, `Slide_Loop`, `Slide_Exit`, `ClimbUp_1m_RM`, `NinjaJump_Start`, `NinjaJump_Idle_Loop`, `NinjaJump_Land`, `Sword_Regular_A`, `Sword_Regular_A_Rec`, `Sword_Regular_B`, `Sword_Regular_B_Rec`, `Sword_Regular_C`, `Sword_Regular_Combo`, `Sword_Block`, `Sword_Dash_RM`, `Hit_Knockback`, and `Hit_Knockback_RM` come from `Unreal-Godot/UAL2_Standard.glb`.
- Universal Animation Library: https://opengameart.org/node/174563, file `universal_animation_librarystandard.zip`, SHA-256 `18ff1a7215f4852b320203e8aaf02a1578b5c8eef9027fbaedfcedc7b85a3ac2`. Its Unreal-rig clips ship only as `Unreal Engine/AL_Standard.fbx`, converted once to glTF with Blender 5.2 (`--background --factory-startup`, FBX import without automatic bone orientation, glTF export with Y up and one animation per action). The clips `Idle_Loop`, `Walk_Loop`, `Jog_Fwd_Loop`, `Sprint_Loop`, `Jump_Start`, `Jump_Loop`, `Jump_Land`, `Crouch_Idle_Loop`, `Crouch_Fwd_Loop`, `Roll`, `Roll_RM`, `Sword_Idle`, `Sword_Attack`, `Hit_Chest`, `Hit_Head`, and `Death01` come from it.

`Dash` and `Dash_Back` are Herta's own, also CC0: `Games/Sandbox/Tools/AuthorDashClips.py` poses them in Blender 5.2 from `Idle_Loop`'s first frame and appends their channels by joint name. Run it again after any update below.

The two exports agree on every weighted joint's rest pose to within 1e-5; they differ only in the names of unweighted fingertip and toe end bones, whose keys are dropped. The merge copies each kept clip's keyframe bytes unchanged, targets channels by joint name, and keeps only the data the mesh, skin, and kept clips reference. To update, download both archives, check their hashes, convert the FBX the same way, and merge with the same clip lists.
