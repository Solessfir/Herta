#include "Fullscreen.slangh"
[[vk::binding(0, 0)]] Texture2D<float4> SelectionDepth;
[[vk::binding(1, 0)]] Texture2D<float4> SceneDepth;

// Editor selection outline: a two-pixel band outside the union of the selected meshes' rasterized silhouettes.
[shader("fragment")]
float4 fragmentMain(FFullscreenOutput Input) : SV_Target
{
    int2 Pixel = int2(Input.Position.xy);
    int2 Last = int2(Visual.Viewport.zw) - 1;
    if (SelectionDepth.Load(int3(Pixel, 0)).r > 0.0) discard;

    bool bFound = false;
    float Visible = 0.0;
    for (int Y = -2; Y <= 2; ++Y)
    {
        for (int X = -2; X <= 2; ++X)
        {
            if (X * X + Y * Y > 5) continue;
            int2 Sample = clamp(Pixel + int2(X, Y), int2(0, 0), Last);
            float Selected = SelectionDepth.Load(int3(Sample, 0)).r;
            if (Selected > 0.0)
            {
                bFound = true;
                // Reversed-Z: the selection is visible where it is not farther than the scene. The tolerance absorbs instanced and per-draw transform rounding.
                Visible = max(Visible, Selected * 1.001 >= SceneDepth.Load(int3(Sample, 0)).r ? 1.0 : 0.0);
            }
        }
    }

    if (!bFound) discard;
    // Linear form of the editor selection color 0xc2b584; occluded edges stay readable but recede.
    return float4(0.539, 0.462, 0.231, lerp(0.45, 1.0, Visible));
}
