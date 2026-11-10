# miniaudio

- Source: https://github.com/mackron/miniaudio
- Revision: tag `0.11.25`, commit `9634bedb5b5a2ca38c1ee7108a9358a4e233f14d`
- License: public domain (Unlicense) or MIT No Attribution, at the user's choice. See `LICENSE`.

| File | Upstream path | Used by |
|---|---|---|
| `miniaudio.h` | `miniaudio.h` | Audio, which opens playback devices and decodes WAV, FLAC, and MP3 sources through it |

The files are unmodified. Herta compiles the implementation once, in `Engine/Source/Runtime/Audio/Private/Miniaudio.cpp`, without miniaudio's engine, node graph, resource manager, encoders, or generators. To update, copy the same files from the new upstream revision, update the revision above, and run the audio tests.
