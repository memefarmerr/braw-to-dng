# braw to dng

converts blackmagic raw (braw) stills into dng files, or into png previews.

blackmagic camera firmware 8.1 and later save stills as single frame braw clips instead of dng files. this tool reads every `.braw` file in a folder, takes one frame from each clip and writes a dng (or a png preview) for it.

## this is not an exact raw dng

the dng written by this tool is a linear, already demosaiced image. it is not the raw sensor data the camera would have written, so:

- there is no bayer data, which means no real highlight recovery and no camera specific raw profiles
- colour is converted by the blackmagic raw sdk into the widest gamut the installed sdk supports (rec.2020 where available)
- 16 bit linear data cannot hold values above 1.0, so highlights past the sdk white point are clipped. the tool prints a note when that happens

if you want the best possible quality, open the clips in davinci resolve and export frames from there. converting with this tool will lose some data.

## using the prebuilt windows build

the exe is self contained: 64 bit windows 10 or 11, no compiler needed, no visual c++ runtime to install, no dlls to put next to it. the one thing it needs is the blackmagic raw sdk, because the codec comes from there.

1. install the blackmagic raw sdk
   - download it from blackmagic, either from [blackmagicdesign.com/support](https://www.blackmagicdesign.com/support) or the [blackmagic raw page](https://www.blackmagicdesign.com/products/blackmagicraw). look for "blackmagic raw" and pick the windows installer
   - run the installer with the default options. this puts the codec in `C:\Program Files (x86)\Blackmagic Design\Blackmagic RAW\Blackmagic RAW SDK\Win\Libraries`
   - davinci resolve does not have to be installed for this
2. download `braw_to_dng.exe` from the [releases page](https://github.com/memefarmerr/braw-to-dng/releases)
3. put the exe somewhere you can find again, for example `C:\tools\braw-to-dng\`
4. put the `.braw` stills in one folder, for example `C:\stills\braw`
5. open a terminal (start menu, type "terminal" or "powershell") and run:

```
"C:\tools\braw-to-dng\braw_to_dng.exe" -i "C:\stills\braw" -o "C:\stills\dng"
```

6. the dngs are written to `C:\stills\dng`, named after the clips

notes:

- windows may show a "windows protected your pc" prompt because the exe is not signed. click "more info", then "run anyway"
- add `-f png` to get png previews instead of dng
- if the sdk is somewhere else, add `--sdkPath "C:\path\to\Win\Libraries"` or set the `BRAW_SDK_PATH` environment variable
- dngs are uncompressed, about 6 mb per megapixel, so a 12 mp still is around 70 mb

## usage

```
braw_to_dng -i <input folder> -o <output folder> [-f dng|png]
```

| option | description |
| --- | --- |
| `-i`, `--inDir` | folder with the `.braw` files. not recursive |
| `-o`, `--outDir` | folder for the output. created if it does not exist |
| `-f`, `--outputFormat` | `dng` (default) or `png` |
| `--frame <index>` | frame to extract, default `0` |
| `--sdkPath <folder>` | folder that holds the codec library |
| `-h`, `--help` | show usage |

`clip.braw` is written as `clip.dng` (or `clip.png`). anything that is not a `.braw` file is ignored.

the blackmagic raw sdk has to be installed. the tool looks for the codec in this order:

1. the `--sdkPath` option
2. the `BRAW_SDK_PATH` environment variable
3. the default install folder, `C:\Program Files (x86)\Blackmagic Design\Blackmagic RAW\Blackmagic RAW SDK\Win\Libraries` on windows and `/Applications/Blackmagic RAW/Blackmagic RAW SDK/Mac/Libraries` on macos

## build from source

needs cmake 3.16 or newer, a c++17 compiler and the blackmagic raw sdk.

```
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

if the sdk is installed somewhere else, point cmake at its root folder (the one that contains `Win/` and `Mac/`):

```
cmake -B build -DBRAW_SDK_ROOT="C:/Program Files (x86)/Blackmagic Design/Blackmagic RAW/Blackmagic RAW SDK"
```

### windows

either of these works:

- visual studio 2019 or 2022 with the "desktop development with c++" workload, which installs the windows sdk. run the cmake commands above from a developer powershell
- mingw-w64, for example [w64devkit](https://github.com/skeeto/w64devkit), with cmake:

```
cmake -B build -G "MinGW Makefiles" -DCMAKE_CXX_COMPILER=g++ -DCMAKE_MAKE_PROGRAM=mingw32-make
cmake --build build
```

`build-windows.bat` does this for you. it uses a w64devkit install in `%LOCALAPPDATA%\braw-tools\w64devkit` if there is one, otherwise an msvc install found through vswhere.

### macos

the macos build links the sdk framework from the default install location. the xcode command line tools are needed.

### the windows api header

the windows sdk ships `BlackmagicRawAPI.idl` instead of a c++ header, so `win/BlackmagicRawAPI.h` is generated from it by `tools/generate_win_header.py`. run this after updating the sdk:

```
python tools/generate_win_header.py "C:/Program Files (x86)/Blackmagic Design/Blackmagic RAW/Blackmagic RAW SDK/Win/Include/BlackmagicRawAPI.idl" win/BlackmagicRawAPI.h
```

## credit

this is a port and continuation of [floffah's braw-to-dng](https://github.com/Floffah/braw-to-dng). all credit for the original tool goes to floffah. the original code is based on blackmagic's extractframe sample.

this fork is maintained by meme farmer studios.

## license

copyright 2023 floffah, copyright 2026 meme farmer studios, copyright 2018 Blackmagic Design. see [LICENSE](LICENSE).
