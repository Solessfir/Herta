# CLDR

- Source: https://github.com/unicode-org/cldr-json
- Revision: tag `49.0.0`, commit `a4b67247b446fd51b475991017a2481911210c60`
- License: Unicode License v3. See `LICENSE`, which `CldrData.h` repeats at its top.

`CldrData.h` is generated from these files of that tag, because the engine reads none of them in their JSON form:

| Upstream path | Used for |
|---|---|
| `cldr-json/cldr-core/supplemental/plurals.json` | Cardinal plural rules |
| `cldr-json/cldr-core/supplemental/parentLocales.json` | Fallback parents |
| `cldr-json/cldr-numbers-full/main/<culture>/numbers.json` | Number symbols, decimal and percent patterns, minimum grouping digits |
| `cldr-json/cldr-dates-full/main/<culture>/ca-gregorian.json` | Date, time, and date-time patterns; month, weekday, and day period names |

It covers the cultures Herta ships, currently `en`, `pl`, and `ru`, and the Localization module reads it. To add a culture or update CLDR, change `TAG` in `GenerateCldrData.py` if needed and run `python External/cldr/GenerateCldrData.py en pl ru <culture>` from the repository root, then update the revision above and run the localization tests.
