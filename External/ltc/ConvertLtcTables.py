"""Converts three.js RectAreaLightTexturesLib.js into LtcTables.h. See UPSTREAM.md."""

import pathlib
import re
import sys

Root = pathlib.Path(__file__).resolve().parent


def ReadTable(Source, Name):
    Match = re.search(r"const " + Name + r" = \[(.*?)\];", Source, re.S)
    if not Match:
        sys.exit(f"{Name} not found")

    # The source writes some negatives with a space after the minus sign.
    Values = [float(Value.replace(" ", "")) for Value in Match.group(1).split(",") if Value.strip()]
    if len(Values) != 64 * 64 * 4:
        sys.exit(f"{Name} has {len(Values)} values instead of 16384")

    return Values


def FormatTable(Values):
    Literals = [repr(Value).replace(".0e", ".e") + "f" if "e" in repr(Value) else repr(Value).rstrip("0") + "f" for Value in Values]
    return "\n".join("\t" + ", ".join(Literals[Index:Index + 16]) + "," for Index in range(0, len(Literals), 16))


def Main():
    Source = pathlib.Path(sys.argv[1]).read_text(encoding="utf-8")
    License = (Root / "LICENSE").read_text(encoding="utf-8").strip().splitlines()
    Header = [
        "// Linearly transformed cosine fit for GGX area lights (Heitz, Dupuy, Hill and Neubelt 2016, https://github.com/selfshadow/ltc_code).",
        "// Converted from three.js examples/jsm/lights/RectAreaLightTexturesLib.js by ConvertLtcTables.py; see UPSTREAM.md. Do not edit by hand.",
        "//",
        *[("// " + Line).rstrip() for Line in License],
        "",
        "#pragma once",
        "",
        "#include <array>",
        "",
        "namespace Ltc",
        "{",
        "// 64x64 RGBA tables addressed by (perceptual roughness, sqrt(1 - N.V)), each scaled by 63/64 and offset half a texel.",
        "// Inverse transform terms m00, m20, m02, m22; the other terms are 0 except m11 = 1.",
        "inline constexpr std::array<float, 64 * 64 * 4> InverseMatrix{",
        FormatTable(ReadTable(Source, "LTC_MAT_1")),
        "};",
        "",
        "// Fresnel scale and bias: specular = (F0 * x + (1 - F0) * y) * form factor.",
        "inline constexpr std::array<float, 64 * 64 * 4> Amplitude{",
        FormatTable(ReadTable(Source, "LTC_MAT_2")),
        "};",
        "} // namespace Ltc",
        "",
    ]
    (Root / "LtcTables.h").write_text("\n".join(Header), encoding="utf-8", newline="\n")


if __name__ == "__main__":
    Main()
