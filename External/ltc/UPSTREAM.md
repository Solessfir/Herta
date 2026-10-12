# LTC tables

`LtcTables.h` holds the GGX linearly transformed cosine fit from "Real-Time Polygonal-Light Shading with Linearly Transformed Cosines" (Heitz, Dupuy, Hill and Neubelt, 2016; fit code at [selfshadow/ltc_code](https://github.com/selfshadow/ltc_code)). The Renderer uses it for Rect light shading.

- Source: [three.js](https://github.com/mrdoob/three.js) `examples/jsm/lights/RectAreaLightTexturesLib.js` at tag `r170`, commit `beab9e845f9e5ae11d648f55b24a0e910b56a85a`, SHA-256 `438c9aa76f101f1f515e09ce674fb062a3ea04d33fc2d0320b0b9755fd17f5d2`.
- License: three.js MIT, copied unmodified as `LICENSE` and repeated at the top of `LtcTables.h`.
- Converted, not copied: C++ cannot consume the JavaScript file, so `ConvertLtcTables.py` extracts `LTC_MAT_1` and `LTC_MAT_2` into two `constexpr` arrays. Values are written with Python's shortest round-trip representation, so they match the source exactly. The JavaScript file itself is not kept.

## Updating

1. Download `RectAreaLightTexturesLib.js` and `LICENSE` from the new three.js tag, and update the tag, commit, and hash above.
2. Run `python ConvertLtcTables.py <path to RectAreaLightTexturesLib.js>` from this folder.
3. Check the three.js `lights_physical_pars_fragment` LTC code still indexes the tables the same way, then rebuild and run the visual test.
