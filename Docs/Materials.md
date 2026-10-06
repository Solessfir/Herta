# Materials

Materials are stable-ID `.hmat` assets containing canonical, versioned JSON. Mesh geometry is shared independently of material assignments. Imported glTF materials remain the default for each section; a level's Static Mesh component can override individual slots with material asset IDs.

## Authoring

Right-click a Game folder in the Content Browser and choose **New material**. Double-click a material to open its editor. Assign it through a Static Mesh slot in Details, or drag it onto a viewport mesh to assign slot zero. Engine materials are read-only.

The material editor provides tint, metallic, roughness, normal strength, occlusion strength, emissive color/intensity, UV scale/offset, and Opaque/Masked settings. Each texture binding has an explicit channel and color space. Base color and emissive normally use sRGB; normal, metallic, roughness, and occlusion use Linear. Packed maps can reuse one texture with different channels, such as R for occlusion, G for roughness, and B for metallic.

HDR textures store linear floating-point radiance and require Linear bindings, including base color and emissive. An sRGB HDR binding reports an error rather than silently changing its interpretation.

Changes preview on a studio-lit sphere without saving source files, using the HDR PBR, environment, tone-map, and SMAA path. Parameter-only changes reuse GPU textures; texture changes cook asynchronously. Shader publication refreshes previews from cached materials without recooking their textures. Save commits the asset atomically. Reload, closing a dirty editor, changing projects, and exiting prompt with Save, Discard, or Cancel. Ctrl+Z and redo operate on the focused material editor's own bounded history; level slot assignments use level history.

Missing or failed textures and shader updates retain the last valid preview and report an actionable error. Masked materials clip against the authored alpha cutoff in both surface and shadow passes. Transparent blending is not implemented.

## Shader iteration

An empty shader path uses `Engine/Shaders/TexturedMesh.slang`. A nonempty portable `.slang` path is relative to the material's own content mount. Prefix it with `Game/` or `Engine/` to select that content mount explicitly, including cross-mount references. These prefixes select content roots, not `Engine/Shaders`; include `VisualShared.slangh` from the separate engine shader include root. Headless cooks provide explicit Engine/Game roots to resolve mounted paths, and use the requested content root as Game when no separate Game root is configured.

- Entry points: `vertexMain`, `instancedVertexMain`, and `fragmentMain`.
- Vertex data: position, UV, normal, and tangent with handedness; instancing carries object-to-clip and object-to-view matrices.
- Bindings: six material textures at t0-t5, diffuse/specular environment at t6-t7, shadow atlas at t8, `FVisualUniforms` at Vulkan binding 64, linear sampler at 128, and the existing 128-byte push constant layout. Every entry point must reflect the exact current `FVisualUniforms` byte size; smaller, larger, or missing buffers are rejected before publication.

The editor watches source and literal includes/imports on background tasks. All three stages compile from the same immutable source snapshot through `HertaShaderWorker`. Compatible pipelines publish together; errors retain the last valid pipeline and appear beside the shader path and in the Output Log. Shader compilation is developer-only, not a runtime or Shipping dependency. The experimental Shipping editor disables shader iteration and uses engine PBR for custom-shader materials; portable custom-shader deployment belongs to the standalone runtime's cooking/deployment slice.

Changing the uniform ABI requires updating both the C++ and Slang definitions and recooking every affected shader. A node-based material graph is not implemented.
