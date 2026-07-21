# iFap

HDR image viewer built on the [MANGO](https://github.com/t0rakka/mango) multimedia library. iFap decodes images on background worker threads, uploads them to the GPU, and displays them through a Vulkan HDR swapchain with scene-linear processing and colorspace-aware output (PQ HDR10, sRGB, and other surface formats supported by the display).

Open a single file, browse a folder recursively, or open an archive directly. iFap reads images from compressed containers (ZIP, RAR, ISO, HBS, and related comic-book extensions such as CBZ/CBR) without extracting them first — handy for manga and image collections stored as multi-file archives. Large images are decoded progressively; navigation stays responsive while uploads continue in the background.

## Features

- Vulkan rendering with float16 processing target and HDR output transforms via MANGO
- Bilinear and bicubic filtering, pan/zoom, optional alpha blending
- Folder indexing with prefetch in the navigation direction
- **Archive support** — open `.zip`/`.cbz`, `.rar`/`.cbr`, `.iso`, `.hbs` (and nested paths inside them) via MANGO's virtual filesystem; browse and view images inside without manual extraction
- Broad image format support inherited from MANGO's decoders (see below)

## Usage

```bash
./ifap [image-or-folder] [--info] [--validate]
```

| Key | Action |
|-----|--------|
| `←` / `Q` | Previous image |
| `→` / `W` | Next image |
| Left drag | Pan |
| Right drag / wheel | Zoom |
| `1` / `2` / `3` | Nearest / bilinear / bicubic filter |
| `B` | Toggle alpha blending |
| `F` / double-click | Fullscreen |
| `Esc` | Quit |

`--info` enables decode timing and other informational console output. `--validate` enables the Vulkan validation layer.

### Archives and containers

Point iFap at an archive file or a path inside one (MANGO resolves the container transparently):

```bash
./ifap ~/downloads/chapter.zip
./ifap ~/manga/series/vol01.rar
```

Supported container types (via MANGO):

| Extension | Type |
|-----------|------|
| `.zip`, `.zipx`, `.cbz` | ZIP archive (CBZ = comic/manga zip) |
| `.rar`, `.cbr` | RAR archive (CBR = comic/manga rar) |
| `.iso` | ISO disc image |
| `.hbs` | HBS archive |

You can navigate prev/next through all decodable images found in the archive, the same as in a normal folder. RAR-based manga releases are a common case and work out of the box.

## Supported formats

Formats are provided by MANGO. The lists below are derived from `mango/source/mango/image/image_*.cpp` decoder registration. Availability of optional codecs (camera RAW, HEIF, AVIF, JXL, etc.) depends on how MANGO was built.

### HDR-capable and high dynamic range workflows

These are the formats most relevant to iFap's HDR display path:

| Extension | Format |
|-----------|--------|
| `.jpg`, `.jpeg`, `.jfif`, `.jpe`, `.jps`, `.mpo` | JPEG (including **Ultra HDR** / gain-map JPEG when present) |
| `.png` | PNG (including **16-bit** and wide-gamut/HDR PNG) |
| `.avif` | AVIF |
| `.jxl` | JPEG XL |
| `.heic`, `.heif` | HEIF / HEIC |
| `.hdr` | Radiance RGBE |
| `.exr` | OpenEXR |
| `.tif`, `.tiff` | TIFF |
| `.pfm` | Portable Float Map |
| `.dng`, `.cr2`, `.cr3`, `.nef`, `.arw`, `.raf`, … | Camera RAW (via LibRaw, when enabled in MANGO) |
| `.dds` | DirectDraw Surface (including float/HDR pixel formats) |
| `.webp` | WebP |
| `.jp2`, `.j2k`, `.j2c`, `.jpc`, `.jph`, `.jhc` | JPEG 2000 |
| `.psd`, `.psb` | Adobe Photoshop (16-bit/float channels) |

### Common raster and exchange formats

`.bmp`, `.gif`, `.ico`, `.cur`, `.tga`, `.pcx`, `.pnm`, `.pbm`, `.pgm`, `.ppm`, `.pam`, `.qoi`, `.toi`, `.sgi`, `.rgb`, `.rgba`, `.bw`, `.wp2`, `.jxr`, `.wdp`, `.pict`, `.pct`, `.pic`, `.soft`, `.zpng`, `.fbmp`

### GPU texture / compressed containers

`.dds`, `.ktx`, `.ktx2`, `.pvr`, `.astc`, `.pkm`

### Amiga, Atari ST, and related

`.iff`, `.lbm`, `.ilbm`, `.anim`, `.ham`, `.ham6`, `.ham8`, `.ehb`, `.rgbn`, `.rgb8`, `.cimg`, `.pi1`–`.pi3`, `.pc1`–`.pc3`, `.neo`, `.spu`, `.spc`, `.sps`, `.ca1`–`.ca3`, `.tny`, `.tn1`–`.tn3`, `.img`, `.ximg`, `.pix`, `.mag`, `.shr`, `.3200`, `.scr`

### MSX

`.sc2`, `.sc4`, `.sc5`, `.sr5`, `.sc6`, `.sc7`, `.sc8`, `.sca`, `.scc`, `.g9b`, `.mif`, `.mig`

### Commodore 64

`.mpic`, `.afl`, `.afli`, `.ami`, `.art`, `.ocp`, `.a64`, `.blp`, `.bpi`, `.pi`, `.cdu`, `.dol`, `.dd`, `.ddl`, `.drl`, `.dlp`, `.drz`, `.dp64`, `.drp`, `.dp`, `.eci`, `.fpt`, `.fcp`, `.fd2`, `.fpr`, `.fun`, `.fp2`, `.gun`, `.ifl`, `.hcb`, `.hfc`, `.him`, `.koa`, `.kla`, `.pmg`, `.pp`, `.ppp`, `.rpm`, `.sar`, `.unp`, `.shfli`, `.shx`, `.shfxl`, `.mci`, `.mcp`, `.ufup`, `.ufli`, `.uifli`, `.vid`, `.flf`, `.vic`

### Atari 8-bit

`.gr8`, `.gr9`, `.mic`, `.hip`, `.pi9`, `.tip`, `.rip`, `.ice`

## Building

iFap requires a C++20 compiler and a built/install MANGO package (Vulkan, image, and window modules).

Build and install MANGO first (sibling checkout shown; adjust paths as needed):

```bash
git clone https://github.com/t0rakka/mango.git
cd mango
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
sudo cmake --install build
```

Then configure and build iFap:

```bash
git clone https://github.com/t0rakka/ifap.git
cd ifap
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

If MANGO is not installed system-wide, point CMake at your build tree:

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH=/path/to/mango/build
cmake --build build
```

The executable is `build/ifap` (or `build/ifap.exe` on Windows).

### Linux runtime note

When distributing a binary with shared libraries next to it:

```bash
patchelf --set-rpath '$ORIGIN' ifap
```

## License

Copyright 2013–2025 Twilight 3D Finland Oy. See source file headers for license terms.
