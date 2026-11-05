# Supported formats and limits

Source files are never converted or modified. Files that cannot be decoded are skipped without stopping the
scan and are listed in **Scan report** with the reason.

## Video

Videos are opened with the system decoder where available (Media Foundation on Windows) and otherwise with
the bundled FFmpeg 8.1 decoders, so no codec packs are needed. This covers common containers (MP4, MOV, MKV,
WebM, AVI, WMV/ASF, MPEG-TS, FLV, …) and codecs (H.264, HEVC including 10-bit, VP8/VP9, AV1, MPEG-2/4,
WMV, ProRes, …). A listed extension does not guarantee that every codec combination inside it can be decoded;
encrypted and damaged files are reported, not opened.

- Rotation metadata is applied. HLG and PQ HDR video is tone-mapped to SDR; Dolby Vision is shown through
  its base layer.
- **Similar videos** compares frames, not audio or subtitles. Mostly black or single-color frames do not
  count as evidence.
- Embedded subtitle tracks are listed in **Details**; subtitles are not played or compared.

## Images

Files are recognized by their content signature, not only by extension.

| Family | Extensions | Notes |
|---|---|---|
| Common | jpg jpeg jfif jpe png webp bmp dib gif tif tiff tga icb vda vst | |
| Icons | ico cur icns | the largest size is compared |
| Netpbm and X11 | pbm pgm ppm pnm pam xbm xpm wbmp | |
| Animated | gif apng png webp avif avifs heic heif | sampled at five points in time |
| Multi-page | tif tiff, HEIF/AVIF collections | sampled across pages, page selector in preview |
| HEIF / AVIF | heic heif hif avif | libheif |
| Vector | svg svgz | rasterized on white; external references and scripts are rejected |
| Layered | psd psb | merged composite only |
| JPEG 2000 / JPEG-LS | jp2 j2k jpc jpf jpx j2c jls | |
| HDR | exr hdr pic pfm phm | tone-mapped for comparison |
| Other | dds pcx qoi sgi rgb rgba bw ras sun dpx iff ilbm lbm | |
| Camera RAW | dng cr2 cr3 nef nrw arw srf sr2 raf orf rw2 pef srw erf kdc dcr 3fr mef mos iiq raw | embedded preview or uncompressed data; otherwise developed by the system RAW codec on Windows and macOS |
| JPEG XR | jxr wdp hdp | Windows only (WIC); not listed on macOS and Linux, which have no decoder |
| JPEG XL | jxl | exact copies only |

- EXIF orientation is applied, ICC profiles are converted to sRGB and transparency is composited on white.
- A static image never matches an animation, and two animations must match over their whole duration.
- RAW embedded previews and developed RAW data are different kinds of evidence and never match each other.

### Limits

- Single images: 64 megapixels, 16,384 pixels per side, 256 MiB file size (SVG: 16 MiB).
- Larger JPEG files (up to 131,072 pixels per side) are decoded at reduced size for comparison.
- Animations: 600 frames or 512 megapixels in total. Multi-page files: 256 pages.

## Sessions and scans

- Review sessions (`.cvsession`) and resumable scan lists hold up to 100,000 files / 32 MiB.
- Parallel reads adapt to the storage type: more readers on SSD/NVMe, fewer on hard disks, network shares
  and removable drives. **Scan report** shows the detected type.
