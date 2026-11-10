# Unicode Character Database

- Source: https://www.unicode.org/Public/17.0.0/ucd/
- Revision: Unicode 17.0.0
- License: Unicode License v3. See `LICENSE`, which `UnicodeData.h` repeats at its top.

`UnicodeData.h` is generated from these files, because the engine reads none of them in their text form:

| Upstream path | Used for |
|---|---|
| `ucd/auxiliary/GraphemeBreakProperty.txt` | Grapheme_Cluster_Break |
| `ucd/emoji/emoji-data.txt` | Extended_Pictographic |
| `ucd/DerivedCoreProperties.txt` | Indic_Conjunct_Break |

The TextLayout module reads it to segment extended grapheme clusters by UAX #29. To update Unicode, change `VERSION` in `GenerateUnicodeData.py`, run `python External/unicode/GenerateUnicodeData.py` from the repository root, update the revision above, and run the text layout tests.
