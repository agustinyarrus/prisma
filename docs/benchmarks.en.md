<p align="right"><sub><b>English</b> · <a href="benchmarks.md">Español</a></sub></p>

# Benchmarks

Measured with `python tests/bench.py` (median of 3 runs, after one warm-up run).

- Machine: 11th Gen Intel(R) Core(TM) i7-1165G7 @ 2.80GHz · 4 cores / 8 threads · AVX2 yes · AVX-512 yes
- Windows 10.0.26100 · Python 3.13.5 · Pillow 12.3.0 · OpenCV 5.0.0 · ffmpeg (winget)
- Each tool does the full job: read, decode, encode and write. prisma and ffmpeg
  also pay for the process startup; Pillow and OpenCV run inside the same Python.
- Every output is verified against the input before measuring (lossless; with JPEG, a tolerance of
  12 levels because every JPEG decoder rounds differently).
- `prisma --max` isn't measured on the 4K images: it tries every strategy at deflate 12 in a single
  stream and takes on the order of a minute per photo (on the JPEG photo it leaves the PNG 11 % smaller).
- Conditions: a laptop with other tasks running in the background (1 to 2 cores busy): the times are noisy; the ratios are what counts. Creating a process costs 200-400 ms here (cmd /c exit: a median of 419 ms) because of this machine's antivirus, and prisma and ffmpeg pay it on every run.

## 3840×2400 JPEG photo → PNG

| tool | time | size | relative time | relative size |
|---|---:|---:|---:|---:|
| **prisma** | 843 ms | 2,231 KB | 2.4× | +4.2 % |
| prisma --fast | 345 ms | 2,700 KB | 1.0× | +26.2 % |
| Pillow | 1,242 ms | 2,367 KB | 3.6× | +10.6 % |
| Pillow optimize | 3,732 ms | 2,140 KB | 10.8× | +0.0 % |
| OpenCV | 805 ms | 3,231 KB | 2.3× | +51.0 % |
| OpenCV level 9 | 15.6 s | 2,254 KB | 45.3× | +5.3 % |
| ffmpeg | 1,265 ms | 2,487 KB | 3.7× | +16.2 % |

## 1920×1080 screenshot (PNG)

| tool | time | size | relative time | relative size |
|---|---:|---:|---:|---:|
| **prisma** | 215 ms | 107 KB | 2.1× | +8.1 % |
| prisma --fast | 208 ms | 122 KB | 2.0× | +22.8 % |
| prisma --max | 2,640 ms | 99 KB | 26.0× | +0.0 % |
| Pillow | 101 ms | 117 KB | 1.0× | +17.9 % |
| Pillow optimize | 276 ms | 115 KB | 2.7× | +16.1 % |
| OpenCV | 134 ms | 143 KB | 1.3× | +43.7 % |
| OpenCV level 9 | 404 ms | 114 KB | 4.0× | +14.9 % |
| ffmpeg | 363 ms | 126 KB | 3.6× | +26.6 % |

## 1024×1024 logo with soft alpha

| tool | time | size | relative time | relative size |
|---|---:|---:|---:|---:|
| **prisma** | 197 ms | 69 KB | 2.8× | +9.1 % |
| prisma --fast | 168 ms | 89 KB | 2.4× | +41.1 % |
| prisma --max | 3,122 ms | 63 KB | 43.8× | +0.0 % |
| Pillow | 71 ms | 85 KB | 1.0× | +34.1 % |
| Pillow optimize | 160 ms | 83 KB | 2.2× | +31.7 % |
| OpenCV | 91 ms | 114 KB | 1.3× | +80.3 % |
| OpenCV level 9 | 324 ms | 86 KB | 4.5× | +36.0 % |
| ffmpeg | 387 ms | 92 KB | 5.4× | +45.6 % |

## 3840×2400 photo with alpha (vignette)

| tool | time | size | relative time | relative size |
|---|---:|---:|---:|---:|
| **prisma** | 1,349 ms | 3,410 KB | 3.0× | +4.0 % |
| prisma --fast | 453 ms | 4,345 KB | 1.0× | +32.5 % |
| Pillow | 1,839 ms | 3,644 KB | 4.1× | +11.1 % |
| Pillow optimize | 7,485 ms | 3,280 KB | 16.5× | +0.0 % |
| OpenCV | 1,637 ms | 4,968 KB | 3.6× | +51.5 % |
| OpenCV level 9 | 26.6 s | 3,439 KB | 58.7× | +4.9 % |
| ffmpeg | 1,763 ms | 3,810 KB | 3.9× | +16.1 % |

## 16-bit 2048×1536 gradient

| tool | time | size | relative time | relative size |
|---|---:|---:|---:|---:|
| **prisma** | 550 ms | 12.4 MB | 1.2× | +0.2 % |
| prisma --fast | 443 ms | 12.4 MB | 1.0× | +0.2 % |
| Pillow | 962 ms | 1,558 KB | — | max diff 65280 |
| Pillow optimize | 10.1 s | 1,426 KB | — | max diff 65280 |
| OpenCV | 1,796 ms | 12.9 MB | 4.1× | +4.3 % |
| OpenCV level 9 | 3,533 ms | 12.9 MB | 8.0× | +4.1 % |
| ffmpeg | 1,508 ms | 12.4 MB | 3.4× | +0.0 % |

## 72-frame 480×270 GIF → APNG

| tool | time | size | relative time | relative size |
|---|---:|---:|---:|---:|
| **prisma** | 474 ms | 113 KB | 1.0× | +19.7 % |
| prisma --max | 6,779 ms | 94 KB | 14.3× | +0.0 % |
| Pillow (APNG) | 1,019 ms | 235 KB | 2.2× | +148.8 % |
| ffmpeg (APNG) | 1,452 ms | 236 KB | 3.1× | +149.6 % |

## Folder of 24 1280×800 screenshots

| tool | time | size | relative time | relative size |
|---|---:|---:|---:|---:|
| **prisma** | 508 ms | 2,509 KB | 1.0× | +0.0 % |
| Pillow | 1,270 ms | 2,700 KB | 2.5× | +7.6 % |
| Pillow optimize | 3,718 ms | 2,644 KB | 7.3× | +5.4 % |
| OpenCV | 1,449 ms | 3,103 KB | 2.9× | +23.7 % |
| OpenCV level 9 | 6,173 ms | 2,594 KB | 12.2× | +3.4 % |
| ffmpeg | 5,197 ms | 2,795 KB | 10.2× | +11.4 % |

<!-- i18n source=benchmarks.md sha256=d0283d172b31 -->
