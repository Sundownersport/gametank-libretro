# gametank-libretro

A libretro core for the [GameTank](https://gametank.zone), built from Clyde Shaffer's [GameTankEmulator](https://github.com/clydeshaffer/GameTankEmulator).

## Features

- Save states, rewind, run-ahead and netplay (states are deterministic)
- Cartridge save RAM (`SAVE` flash carts) as a standard `.srm`
- Flash cart writes saved as `<rom>.xor` in the save directory, same format as the standalone emulator
- Two controllers
- Core options: palette, 4:3 side borders, instant blitter

## Controls

| GameTank | RetroPad |
|----------|----------|
| D-pad    | D-pad    |
| A        | B        |
| B        | A        |
| C        | Y or X   |
| Start    | Start    |

## Building

```
make
```

Other targets: `make platform=<osx|ios-arm64|tvos-arm64|win|libnx|vita|ctr|emscripten>`. Android builds with `ndk-build -C jni`.

CI builds:

- Linux x86_64, arm64 and armhf with the current Ubuntu GCC toolchains
- Linux arm64 and armhf against glibc 2.23 with zig, for older handhelds
- Windows x86_64 with mingw-w64
- macOS universal (x86_64 + arm64)
- Android arm64-v8a and armeabi-v7a with the NDK

## Upstream

The emulation code is taken from GameTankEmulator commit `9896544`:

- `src/mos6502/` is upstream's CPU core with `GetState`/`SetState` added.
- `src/blitter.cpp`, `src/audio_coprocessor.cpp` and `src/gametank.cpp` are upstream's blitter, audio coprocessor and memory map (`gte.cpp`) with SDL, devtools and the profiler removed.
- `src/gametank_palette.h` is unchanged.

## License

MIT. See `LICENSE` and `src/mos6502/LICENSE.txt`.
