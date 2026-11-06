# dsi-photoConverter

A command-line **JPEG/PNG → Nintendo DSi Camera photo converter** written in C.
Supports single-image conversion, batch processing, and adding new photos to an
SD album using an existing or newly created `pit.bin` index.

Runs on a computer with Linux, Windows, or macOS. Python is not required to run
the converter.

## Features

- Resizes images to **640×480** and generates a **160×120** thumbnail.
- Creates DSi-style EXIF/MakerNote metadata and calculates the JPEG's AES signature.
- Adjusts JPEG quality to keep the output at or below **140,000 bytes**.
- Converts multiple images in one run; PNG transparency is composited onto white.
- With a supplied `pit.bin` path, selects available filenames and folders, adds
  album entries, and recalculates CRC16. Existing entries, stickers, and frames
  are preserved when updating an existing index.
- Creates a missing SD index, scanning supported signed JPEGs in standard photo
  and frame folders. Can also create an index without converting any images.
- Backs up existing indexes. If a batch fails during normal operation, removes
  the photos created by that batch and leaves the original index unchanged.
  A new index is written only after a successful batch.

**Verified:** Linux builds and automated tests. A user has confirmed on a real
Nintendo DSi that converted photos display in the SD album after importing them
and updating an existing `pit.bin`; replacing an existing photo also works.
Creating a new index has been checked against the contents of a real SD card on
a computer, but using a newly created index on the console still needs confirmation.

## Quick start

After building locally, run the following commands from the project root.
Use `./build/dsi-photoConverter` on Linux/macOS or
`.\build\Release\dsi-photoConverter.exe` in Windows PowerShell.
Quote paths containing spaces.

### Convert one photo without an index

```bash
./build/dsi-photoConverter photo.png
```

Creates the first available `HNI_XXXX.JPG` in the current directory.
Does not create or modify `pit.bin`. Copying this JPEG to an SD card alone does
not register it in the album. Use the indexed import mode below to add a new photo.

### Convert multiple photos without an index

```bash
./build/dsi-photoConverter -o converted photo1.png photo2.jpg photo3.png
```

Saves the results in `converted`, creating the directory if needed.
Batch mode never overwrites existing files.

In Bash/Zsh, use shell globs to select multiple photos:

```bash
./build/dsi-photoConverter -o converted photos/*.png photos/*.jpg
```

Only use globs that match existing files. In Windows PowerShell, you can build
a file list as follows:

```powershell
$photos = @(Get-ChildItem .\photos -File | Where-Object { $_.Extension -in '.png', '.jpg', '.jpeg' } | ForEach-Object { $_.FullName })
if ($photos.Count -gt 0) {
    & .\build\Release\dsi-photoConverter.exe -o converted @photos
}
```

### Import multiple photos with `pit.bin`

If you have a copy of the index from your SD card:

```bash
./build/dsi-photoConverter photo1.png photo2.jpg pit.bin -o ready
```

Or specify the index explicitly:

```bash
./build/dsi-photoConverter --pit pit.bin -o ready photo1.png photo2.jpg
```

If the index exists, the program **updates the supplied `pit.bin` in place** and
creates a backup next to it named `pit.bin.bak`. Subsequent runs use `.bak.1`,
`.bak.2`, and so on. If the file is missing, it creates a new SD index and its
parent directories after the entire batch succeeds; there is no old file to back up.
Photos are written into a directory tree like this:

```text
ready/
└── DCIM/
    ├── 100NIN02/
    │   ├── HNI_0001.JPG
    │   └── HNI_0002.JPG
    └── 101NIN02/
        └── ...
```

This is an example layout; actual filenames depend on available slots in the
index. Names are selected automatically. Do not rename the generated photos,
otherwise their paths will no longer match the index.

### Create a new `pit.bin`

To create an index without converting images:

```bash
./build/dsi-photoConverter --create-pit ready/private/ds/app/484E494A/pit.bin -o ready
```

Or create an index and import photos in the same run:

```bash
./build/dsi-photoConverter --create-pit ready/private/ds/app/484E494A/pit.bin -o ready photo1.png photo2.jpg
```

`--create-pit` refuses to overwrite any existing file. Normal `--pit` imports
update a valid existing index or create a missing one automatically.
Without either index option, conversion still produces JPEGs only.

New indexes contain 3,000 entries and are written with a valid CRC16. Creation
scans `DCIM/xxxNIN02/HNI_XXXX.JPG` and
`private/ds/app/484E494A/DCIM/xxxNIN02/HNI_XXXX.JPG` under the `-o` root, registering
existing supported signed photos and frames before adding new images. Existing
JPEGs are left unchanged. Their modification times are used as album timestamps;
unrepresentable dates fall back to the current time. Invalid or unsupported
JPEGs in these standard slots cause creation to stop without writing the index.

Stickers cannot be recovered from JPEGs. Files outside these standard paths are
not indexed. Use the actual card root as `-o` when creating an index for a card
that already contains photos. Creating an index in an empty output folder cannot
recover the photos on a disconnected card.

## Adding photos to an SD card

1. Turn off the DSi and connect its SD card to your computer.
2. Back up the card. The album index is located at
   `private/ds/app/484E494A/pit.bin`. If missing, the program can create it; there
   is no need to take a placeholder photo first.
3. Choose one of the methods below.
4. After a successful import, safely eject the card, insert it into your DSi,
   and open the **SD card album** in Nintendo DSi Camera.

### Method A: write directly to the connected SD card

Set `-o` to the **root of the card**, rather than its `DCIM` or `100NIN02` folder.
Linux example (replace the mount path with your own):

```bash
./build/dsi-photoConverter \
  --pit /media/user/SD/private/ds/app/484E494A/pit.bin \
  -o /media/user/SD photo1.png photo2.jpg
```

Windows example, assuming the SD card is drive `E:`:

```powershell
.\build\Release\dsi-photoConverter.exe --pit "E:\private\ds\app\484E494A\pit.bin" -o "E:\" photo1.png photo2.jpg
```

The program places the JPEGs in `DCIM/xxxNIN02` and updates the index on the card.
This method also checks for existing JPEGs in the output folders.

### Method B: prepare the files on your computer

1. Copy `pit.bin` from the card into a working directory on your computer. If
   the card has no index, use direct import (Method A) to register existing JPEGs,
   or copy its photo and frame directory trees into the output folder first.
2. Run `./build/dsi-photoConverter --pit pit.bin -o ready photo1.png photo2.jpg`.
3. Copy **the contents of `ready/DCIM` into `DCIM` on the card**, preserving all
   folder and file names. Avoid creating a nested `DCIM/DCIM` directory.
4. Copy **the updated `pit.bin` from your working directory** back to
   `private/ds/app/484E494A/pit.bin` on the same card.

Transfer both the JPEGs and their matching updated index. Do not add or delete
photos on the DSi between copying the index to your computer and transferring
the results back. If a JPEG filename conflict occurs during transfer, stop and
use a fresh index with direct import to the card. The program only checks files
inside the output directory specified by `-o`.

## Command reference

| Command / option | Purpose |
| --- | --- |
| `dsi-photoConverter photo.png` | Convert one photo with an automatic name in the current directory |
| `dsi-photoConverter -o DIR photo1.png photo2.jpg` | Convert a batch without updating an index |
| `dsi-photoConverter --pit pit.bin -o ROOT photo1.png photo2.jpg` | Convert a batch and register it in the SD album |
| `dsi-photoConverter photo.png pit.bin -o ROOT` | Supply the index as a positional argument |
| `dsi-photoConverter --create-pit pit.bin -o ROOT` | Create a new index from standard photo/frame folders; never overwrite an existing file |
| `dsi-photoConverter --verify photo.JPG` | Verify the JPEG's AES signature |
| `dsi-photoConverter --help` | Show help |

`--output-dir` is the long form of `-o`. The output directory defaults to the
current directory. The parent of a new output directory must already exist.
`--verify` checks the signature; it does not check album registration or whether
the console will display the photo.

For compatibility, you can specify an explicit **new** output filename:

```bash
./build/dsi-photoConverter input.png HNI_0001.JPG
```

Two positional arguments without options are treated as input and output if the
second file does not exist. Two existing images are treated as a batch. Use `-o`
to select batch mode unambiguously. To specify an output file and allow overwriting:

```bash
./build/dsi-photoConverter --output HNI_0001.JPG input.png
```

## Limitations

- Existing indexes must be **3,000-entry SD indexes** with a valid CRC16.
  Missing indexes can be created; invalid existing files are never replaced.
  The 500-entry internal-memory index is not supported.
- Supports folders `100NIN02`–`999NIN02`, with 100 photo filenames per folder.
  Without an index, uses `HNI_0001.JPG`–`HNI_9999.JPG` in one output directory.
- If the album has no free entries, import fails before creating any photos.
- If you use Memory Pit, supply the **original album index**, rather than the
  exploit file, which is also named `pit.bin`.
- Do not run simultaneous imports to the same card. Normal error rollback does
  not protect against removing the card or losing power during a write.
- Images are stretched to 640×480. EXIF orientation is not applied; rotate the
  image in an editor beforehand if needed.
- Accepts JPEG and PNG images up to 64 million pixels.
- Does not modify DSi firmware and cannot run directly on the console.

## Building from source

Requires a C99 compiler, **CMake 3.16+**, libjpeg, **libpng 1.6+**, and
**OpenSSL 1.1.1+**. Use a supported OpenSSL 3.x version for new builds.
Python 3 is only required for the integration tests.

### Linux: Debian / Ubuntu / Mint

```bash
sudo apt update
sudo apt install -y build-essential cmake libjpeg-dev libpng-dev libssl-dev python3
```

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
./build/dsi-photoConverter --help
```

Fedora dependencies: `gcc cmake libjpeg-turbo-devel libpng-devel openssl-devel python3`.
Arch dependencies: `base-devel cmake libjpeg-turbo libpng openssl python`.

### Windows: Visual Studio and vcpkg

Install Git, CMake, Python 3, and Visual Studio 2022 with the
**Desktop development with C++** workload. In PowerShell, install vcpkg if needed:

```powershell
git clone https://github.com/microsoft/vcpkg.git C:\dev\vcpkg
& C:\dev\vcpkg\bootstrap-vcpkg.bat -disableMetrics
```

The `C:\dev` directory must already exist. If vcpkg is installed elsewhere,
adjust `$env:VCPKG_ROOT`:

```powershell
$env:VCPKG_ROOT = 'C:\dev\vcpkg'
& "$env:VCPKG_ROOT\vcpkg.exe" install libjpeg-turbo:x64-windows-static libpng:x64-windows-static openssl:x64-windows-static
cmake -S . -B build -A x64 "-DCMAKE_TOOLCHAIN_FILE=$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" -DVCPKG_TARGET_TRIPLET=x64-windows-static -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
.\build\Release\dsi-photoConverter.exe --help
```

These commands use static dependencies and the static MSVC Runtime for Release.
Use a new build directory when changing the triplet or runtime type.
Debug builds with this triplet require the `MultiThreadedDebug` runtime.

### macOS

Install the Xcode Command Line Tools and [Homebrew](https://brew.sh/), then run:

```bash
brew install cmake jpeg-turbo libpng openssl@3 python
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DOPENSSL_ROOT_DIR="$(brew --prefix openssl@3)" \
  -DCMAKE_PREFIX_PATH="$(brew --prefix jpeg-turbo);$(brew --prefix libpng)"
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
./build/dsi-photoConverter --help
```

### Tests and local packaging

CTest checks conversion and signature verification. When Python 3 is available,
it also runs integration scenarios covering new index creation, existing photo/frame
registration, batch processing, backups,
preservation of existing entries, CRC16, occupied filenames, folder rollover,
a full or damaged index, and rollback after failure. Tests use a synthetic
`pit.bin`; they do not replace testing on a console.

Create a ZIP containing the program, README, and license files:

```bash
cpack --config build/CPackConfig.cmake -C Release -B dist
```

When distributing a custom Windows build, include the license notices for its
static dependencies as described in [THIRD_PARTY.md](THIRD_PARTY.md).
Linux/macOS builds use shared system libraries and are not fully standalone:
they require the corresponding runtime dependencies and a compatible OS and architecture.

## License and references

Project source code is available under the [MIT license](LICENSE).
Dependency licenses and format research references are listed in
[THIRD_PARTY.md](THIRD_PARTY.md).
`pit.bin` format documentation:
[GBATEK](https://problemkaputt.de/gbatek.htm#dsisdmmccamerafilespitbin).

This project is not affiliated with Nintendo and is not an official Nintendo tool.
