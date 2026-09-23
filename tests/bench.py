"""bench.py - image2png contra Pillow, OpenCV y ffmpeg: tiempo y tamaño, verificando que todo sea
sin perdida.

    python tests/bench.py                 todos los casos (mediana de 3 corridas)
    python tests/bench.py --runs 5 foto   solo los casos que contengan "foto"

Los casos son realistas: una foto 4K de las que trae Windows, una captura de pantalla sintetica
(texto, ventanas, iconos), un logo con alfa suave, una foto 4K con alfa, una imagen de 16 bits, un
GIF animado a APNG y una carpeta de 24 capturas (donde cuenta el paralelismo entre archivos).

Medicion: cada herramienta hace el trabajo COMPLETO (leer, decodificar, codificar, escribir) y se
toma la mediana de N corridas despues de una de calentamiento. Las de linea de comandos (prisma,
ffmpeg) incluyen el arranque del proceso; Pillow y OpenCV corren dentro de este Python (no pagan
arranque): la comparacion les es favorable a ellas. Antes de medir, cada salida se decodifica y se
compara con la entrada: una herramienta que pierde datos no entra en la tabla.
"""
from __future__ import annotations

import argparse
import os
import platform
import shutil
import statistics
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Callable

import cv2
import numpy as np
import PIL
from PIL import Image, ImageDraw, ImageFont, ImageSequence

sys.path.insert(0, str(Path(__file__).resolve().parent))
import cases  # noqa: E402  (read_rgba: el lector rapido de la suite)
import refpng  # noqa: E402

ROOT = Path(__file__).resolve().parent
WORK = ROOT / "out" / "bench"
EXE = Path(os.environ.get("PRISMA_EXE", ROOT.parent / "bin" / "image2png.exe"))
DOCS = ROOT.parent / "docs" / "benchmarks.md"
WALLPAPERS = Path(os.environ.get("WINDIR", r"C:\Windows")) / "Web" / "Wallpaper"
FFMPEG = shutil.which("ffmpeg")
JPEG_TOLERANCE = 12              # decodificadores de JPEG distintos redondean distinto (IDCT, croma)

INK = {"ok": (181, 223, 168), "err": (245, 163, 181), "warn": (238, 223, 184), "acc": (196, 181, 253),
       "dim": (127, 132, 156), "num": (168, 207, 242), "text": (205, 214, 244), "peach": (246, 192, 160)}
COLOR = sys.stdout.isatty() and not os.environ.get("NO_COLOR")


def ink(name: str, text: str) -> str:
    if not COLOR:
        return text
    r, g, b = INK[name]
    return f"\x1b[38;2;{r};{g};{b}m{text}\x1b[0m"


# ---------------------------------------------------------------------------------------------------
# entradas (deterministicas)
# ---------------------------------------------------------------------------------------------------

def wallpaper(n: int) -> Path:
    candidates = sorted(WALLPAPERS.glob("Theme*/*.jpg"))
    if not candidates:
        raise SystemExit("no se encontraron los fondos de Windows en " + str(WALLPAPERS))
    return candidates[n % len(candidates)]


def font(size: int):
    try:
        return ImageFont.truetype("segoeui.ttf", size)
    except OSError:
        return ImageFont.load_default(size)


def screenshot(path: Path, seed: int, w: int = 1920, h: int = 1080) -> None:
    """Captura de pantalla creible: fondo, ventanas con barra de titulo, texto, iconos, una foto."""
    rng = np.random.default_rng(seed)
    im = Image.new("RGB", (w, h), (32, 34, 44))
    d = ImageDraw.Draw(im)
    photo = Image.open(wallpaper(seed)).convert("RGB")
    for k in range(4):
        x0, y0 = int(rng.integers(0, w // 2)), int(rng.integers(0, h // 2))
        x1, y1 = x0 + int(rng.integers(w // 4, w // 2)), y0 + int(rng.integers(h // 4, h // 2))
        d.rectangle([x0, y0, x1, y1], fill=(248, 248, 250), outline=(200, 200, 210))
        d.rectangle([x0, y0, x1, y0 + 32], fill=tuple(int(v) for v in rng.integers(40, 120, 3)))
        d.text((x0 + 12, y0 + 6), f"Documento {seed}-{k} · Explorador de archivos", font=font(15), fill=(255, 255, 255))
        for line in range(int((y1 - y0 - 48) / 22)):
            words = " ".join("".join(chr(97 + int(c)) for c in rng.integers(0, 26, int(rng.integers(2, 9)))) for _ in range(int(rng.integers(3, 12))))
            d.text((x0 + 16, y0 + 44 + 22 * line), words, font=font(14), fill=(30, 30, 40))
        if k == 1:
            thumb = photo.resize((min(360, x1 - x0 - 40), min(220, y1 - y0 - 80)))
            im.paste(thumb, (x0 + 20, y0 + 60))
        for i in range(5):
            cx, cy = x1 - 30 - 28 * i, y0 + 16
            d.ellipse([cx - 8, cy - 8, cx + 8, cy + 8], fill=tuple(int(v) for v in rng.integers(80, 255, 3)))
    im.save(path, compress_level=1)


def logo(path: Path, size: int = 1024) -> None:
    """Logo con alfa suave: formas dibujadas a 4x y reducidas (antialias real)."""
    big = Image.new("RGBA", (size * 4, size * 4), (0, 0, 0, 0))
    d = ImageDraw.Draw(big)
    s = size * 4
    d.ellipse([s * 0.08, s * 0.08, s * 0.92, s * 0.92], fill=(36, 99, 235, 255))
    d.ellipse([s * 0.22, s * 0.22, s * 0.78, s * 0.78], fill=(0, 0, 0, 0))
    d.polygon([(s * 0.5, s * 0.18), (s * 0.82, s * 0.8), (s * 0.18, s * 0.8)], fill=(250, 204, 21, 230))
    d.text((s * 0.3, s * 0.42), "prisma", font=font(size // 2), fill=(255, 255, 255, 200))
    big.resize((size, size), Image.LANCZOS).save(path, compress_level=1)


def photo_rgba(path: Path) -> None:
    im = Image.open(wallpaper(1)).convert("RGBA")
    w, h = im.size
    y, x = np.mgrid[0:h, 0:w]
    r = np.sqrt(((x - w / 2) / (w / 2)) ** 2 + ((y - h / 2) / (h / 2)) ** 2)
    a = np.clip((1.05 - r) * 4, 0, 1) * 255
    arr = np.asarray(im).copy()
    arr[..., 3] = a.astype(np.uint8)
    Image.fromarray(arr).save(path, compress_level=1)


def deep16(path: Path, w: int = 2048, h: int = 1536) -> None:
    y, x = np.mgrid[0:h, 0:w].astype(np.float64)
    rng = np.random.default_rng(16)
    img = np.dstack([x / w * 65535, y / h * 65535, (np.sin(x / 90) * np.cos(y / 70) * 0.5 + 0.5) * 65535])
    img += rng.normal(0, 40, img.shape)
    cv2.imwrite(str(path), np.clip(img, 0, 65535).astype(np.uint16)[..., ::-1], [cv2.IMWRITE_PNG_COMPRESSION, 1])


def gif_animation(path: Path, frames: int = 72, w: int = 480, h: int = 270) -> None:
    images = []
    for f in range(frames):
        im = Image.new("RGB", (w, h), (18, 18, 28))
        d = ImageDraw.Draw(im)
        for k in range(6):
            t = f / frames * 2 * np.pi + k
            cx, cy = w / 2 + np.cos(t) * (60 + 20 * k), h / 2 + np.sin(t * 1.5) * (40 + 10 * k)
            d.ellipse([cx - 22, cy - 22, cx + 22, cy + 22], fill=(60 + 30 * k, 200 - 20 * k, 255 - 25 * k))
        d.text((12, h - 30), f"cuadro {f:02d}", font=font(18), fill=(230, 230, 240))
        images.append(im.convert("P", palette=Image.ADAPTIVE, colors=64))
    images[0].save(path, save_all=True, append_images=images[1:], duration=40, loop=0)


# ---------------------------------------------------------------------------------------------------
# herramientas
# ---------------------------------------------------------------------------------------------------

def run(cmd: list[str]) -> None:
    p = subprocess.run(cmd, capture_output=True)
    if p.returncode != 0:
        raise RuntimeError(p.stderr.decode("utf-8", "replace")[-300:])


def prisma(*extra: str) -> Callable[[list[Path], Path], None]:
    def go(inputs: list[Path], out: Path) -> None:
        target = out if len(inputs) == 1 else out.parent
        run([str(EXE), *map(str, inputs), "-o", str(target), "--plain", "--quiet", *extra])
    return go


def pillow(**save_kw) -> Callable[[list[Path], Path], None]:
    def go(inputs: list[Path], out: Path) -> None:
        for src in inputs:
            dst = out if len(inputs) == 1 else out.parent / (src.stem + ".png")
            im = Image.open(src)
            if getattr(im, "is_animated", False):
                frames = [f.convert("RGBA") for f in ImageSequence.Iterator(im)]
                frames[0].save(dst, save_all=True, append_images=frames[1:], duration=im.info.get("duration", 40), loop=0, **save_kw)
            else:
                im.load()
                im.save(dst, "PNG", **save_kw)
    return go


def opencv(level: int) -> Callable[[list[Path], Path], None]:
    def go(inputs: list[Path], out: Path) -> None:
        for src in inputs:
            dst = out if len(inputs) == 1 else out.parent / (src.stem + ".png")
            img = cv2.imread(str(src), cv2.IMREAD_UNCHANGED)
            if img is None or not cv2.imwrite(str(dst), img, [cv2.IMWRITE_PNG_COMPRESSION, level]):
                raise RuntimeError("OpenCV no pudo")
    return go


def ffmpeg(*extra: str) -> Callable[[list[Path], Path], None]:
    def go(inputs: list[Path], out: Path) -> None:
        for src in inputs:
            dst = out if len(inputs) == 1 else out.parent / (src.stem + ".png")
            run([FFMPEG, "-v", "error", "-y", "-i", str(src), *extra, str(dst)])
    return go


# ---------------------------------------------------------------------------------------------------
# casos
# ---------------------------------------------------------------------------------------------------

@dataclass
class Case:
    key: str
    title: str
    make: Callable[[Path], list[Path]]
    tools: dict[str, Callable]
    lossy_input: bool = False
    animated: bool = False


def still_tools(extra_ffmpeg=(), with_max: bool = True) -> dict[str, Callable]:
    # --max prueba todas las estrategias a deflate 12 en un solo flujo: en una foto 4K son ~70 s
    # por corrida (para -11 %); en esos casos no se mide, se cuenta aparte en el informe.
    tools = {
        "prisma": prisma(),
        "prisma --fast": prisma("--fast"),
        "prisma --max": prisma("--max"),
        "Pillow": pillow(),
        "Pillow optimize": pillow(optimize=True),
        "OpenCV": opencv(1),
        "OpenCV nivel 9": opencv(9),
    }
    if FFMPEG:
        tools["ffmpeg"] = ffmpeg(*extra_ffmpeg)
    if not with_max:
        del tools["prisma --max"]
    return tools


def one(maker: Callable[[Path], None], name: str) -> Callable[[Path], list[Path]]:
    def make(d: Path) -> list[Path]:
        path = d / name
        if not path.exists():
            maker(path)
        return [path]
    return make


def copy_of(src_fn: Callable[[], Path], name: str) -> Callable[[Path], list[Path]]:
    def make(d: Path) -> list[Path]:
        dst = d / name
        if not dst.exists():
            shutil.copy2(src_fn(), dst)
        return [dst]
    return make


def folder(d: Path) -> list[Path]:
    sub = d / "capturas"
    sub.mkdir(exist_ok=True)
    paths = []
    for i in range(24):
        p = sub / f"captura-{i:02d}.png"
        if not p.exists():
            screenshot(p, 100 + i, 1280, 800)
        paths.append(p)
    return paths


def animated_tools() -> dict[str, Callable]:
    tools = {"prisma": prisma(), "prisma --max": prisma("--max"), "Pillow (APNG)": pillow()}
    if FFMPEG:
        tools["ffmpeg (APNG)"] = ffmpeg("-f", "apng", "-plays", "0")
    return tools


CASES = [
    Case("foto-jpeg-4k", "foto JPEG 3840×2400 → PNG", copy_of(lambda: wallpaper(0), "foto.jpg"), still_tools(with_max=False), lossy_input=True),
    Case("captura", "captura de pantalla 1920×1080 (PNG)", one(lambda p: screenshot(p, 7), "captura.png"), still_tools()),
    Case("logo-alfa", "logo 1024×1024 con alfa suave", one(logo, "logo.png"), still_tools()),
    Case("foto-rgba-4k", "foto 3840×2400 con alfa (viñeta)", one(photo_rgba, "foto-rgba.png"), still_tools(with_max=False)),
    Case("16-bits", "degradé de 16 bits 2048×1536", one(deep16, "deep16.png"), still_tools(with_max=False)),
    Case("gif-apng", "GIF de 72 cuadros 480×270 → APNG", one(gif_animation, "anim.gif"), animated_tools(), animated=True),
    Case("carpeta-24", "carpeta de 24 capturas 1280×800", folder, {k: v for k, v in still_tools().items() if not k.startswith("prisma --")}),
]


# ---------------------------------------------------------------------------------------------------
# verificacion y medicion
# ---------------------------------------------------------------------------------------------------

def is_apng(path: Path) -> bool:
    return any(kind == b"acTL" for kind, _ in cases.png_chunks(path))


def frames_of(path: Path) -> list[np.ndarray]:
    """Cuadros RGBA de un archivo. PNG fijo: el lector rapido de la suite (OpenCV en 16 bits, Pillow
    en 8); APNG: el lector de referencia, que compone segun la especificacion (y es Python puro:
    en una foto 4K tardaria minutos)."""
    if path.suffix.lower() == ".png":
        if is_apng(path):
            return [f.astype(np.int64) for f in refpng.read_png(path)]
        return [cases.read_rgba(path).astype(np.int64)]
    im = Image.open(path)
    out = []
    for f in ImageSequence.Iterator(im):
        out.append(np.asarray(f.convert("RGBA")).astype(np.int64))
    return out


def reference(path: Path) -> list[np.ndarray]:
    if path.suffix.lower() == ".png":
        return frames_of(path)
    if path.suffix.lower() == ".gif":
        return frames_of(path)
    return [np.asarray(Image.open(path).convert("RGBA")).astype(np.int64)]


def to_depth8(frames: list[np.ndarray]) -> list[np.ndarray]:
    return [(f * 255 + 32767) // 65535 if f.max() > 255 else f for f in frames]


def verify(inputs: list[Path], outputs: list[Path], lossy: bool, animated: bool) -> str | None:
    """None si la salida es fiel; si no, el motivo."""
    for src, dst in zip(inputs, outputs):
        if not dst.exists():
            return f"falta {dst.name}"
        want, got = reference(src), frames_of(dst)
        if animated:
            want, got = to_depth8(want), to_depth8(got)
        if len(want) != len(got):
            return f"{len(got)} cuadros de {len(want)}"
        for a, b in zip(want, got):
            if a.shape != b.shape:
                return f"forma {b.shape} != {a.shape}"
            # 16 bits: cv2 y refpng devuelven 16; una herramienta que baje a 8 bits pierde datos
            diff = int(np.abs(a - b)[a[..., 3] > 0].max()) if (a[..., 3] > 0).any() else 0
            if diff > (JPEG_TOLERANCE if lossy else 0):
                return f"dif. max {diff}"
    return None


@dataclass
class Row:
    tool: str
    seconds: float
    size: int
    note: str = ""


def measure(case: Case, runs: int, work: Path) -> list[Row]:
    inputs = case.make(work)
    rows = []
    for name, tool in case.tools.items():
        slot = work / "salidas" / case.key / name.replace(" ", "_").replace("(", "").replace(")", "")
        shutil.rmtree(slot, ignore_errors=True)
        slot.mkdir(parents=True)
        out = slot / (inputs[0].stem + ".png")
        outputs = [out] if len(inputs) == 1 else [slot / (p.stem + ".png") for p in inputs]
        try:
            tool(inputs, out)                                            # calentamiento + verificacion
            problem = verify(inputs, outputs, case.lossy_input, case.animated)
            times = []
            for _ in range(runs):
                for o in outputs:
                    o.unlink(missing_ok=True)
                t0 = time.perf_counter()
                tool(inputs, out)
                times.append(time.perf_counter() - t0)
            size = sum(o.stat().st_size for o in outputs)
            rows.append(Row(name, statistics.median(times), size, problem or ""))
        except Exception as exc:                                         # herramienta que no sabe: se informa
            rows.append(Row(name, float("nan"), 0, f"no pudo: {str(exc)[:60]}"))
    return rows


# ---------------------------------------------------------------------------------------------------
# presentacion
# ---------------------------------------------------------------------------------------------------

def fmt_ms(s: float) -> str:
    if s != s:
        return "—"
    return f"{s * 1000:,.0f} ms".replace(",", ".") if s < 10 else f"{s:.1f} s".replace(".", ",")


def comma(text: str) -> str:
    """Coma decimal, como en el resto de prisma."""
    return text.replace(".", ",")


def fmt_kb(n: int) -> str:
    if not n:
        return "—"
    return f"{n / 1024:,.0f} KB".replace(",", ".") if n < 10 * 1024 * 1024 else f"{n / 1048576:.1f} MB".replace(".", ",")


def print_case(case: Case, rows: list[Row]) -> None:
    valid = [r for r in rows if r.seconds == r.seconds and not r.note]
    fastest = min((r.seconds for r in valid), default=1)
    smallest = min((r.size for r in valid), default=1)
    print()
    print("  " + ink("acc", case.title))
    for r in rows:
        bad = bool(r.note) or r.seconds != r.seconds
        name = ink("err" if bad else ("peach" if r.tool.startswith("prisma") else "text"), f"{r.tool:<17}")
        t = ink("num", f"{fmt_ms(r.seconds):>9}")
        sz = ink("num", f"{fmt_kb(r.size):>10}")
        rel_t = "" if bad else ink("dim", comma(f"{r.seconds / fastest:5.1f}× tiempo"))
        rel_s = "" if bad else ink("ok" if r.size == smallest else "dim", comma(f"{(r.size / smallest - 1) * 100:+6.1f} % tamaño"))
        note = ink("err", "  " + r.note) if r.note else ""
        print(f"    {name} {t} {sz}   {rel_t}  {rel_s}{note}")


def write_markdown(results: list[tuple[Case, list[Row]]], runs: int, note: str) -> None:
    cpu = platform.processor() or "?"
    try:
        out = subprocess.run([str(EXE), "--version", "--plain"], capture_output=True).stderr.decode("utf-8", "replace")
        cpu = next((line.strip() for line in out.splitlines() if "GHz" in line or "núcleos" in line), cpu)
    except OSError:
        pass
    lines = ["# Benchmarks", "",
             f"Medidos con `python tests/bench.py` (mediana de {runs} corridas, después de una de calentamiento).", "",
             f"- Máquina: {cpu}",
             f"- Windows {platform.version()} · Python {platform.python_version()} · Pillow {PIL.__version__} · OpenCV {cv2.__version__}"
             + (" · ffmpeg (winget)" if FFMPEG else ""),
             "- Cada herramienta hace el trabajo completo: leer, decodificar, codificar y escribir. prisma y ffmpeg",
             "  pagan además el arranque del proceso; Pillow y OpenCV corren dentro del mismo Python.",
             "- Toda salida se verifica contra la entrada antes de medir (sin pérdida; con JPEG, tolerancia de",
             f"  {JPEG_TOLERANCE} niveles porque cada decodificador de JPEG redondea distinto).",
             "- `prisma --max` no se mide en las imágenes 4K: prueba todas las estrategias a deflate 12 en un solo",
             "  flujo y tarda del orden de un minuto por foto (en la foto JPEG deja el PNG un 11 % más chico)."]
    if note:
        lines.append(f"- Condiciones: {note}")
    lines.append("")
    for case, rows in results:
        valid = [r for r in rows if r.seconds == r.seconds and not r.note]
        fastest = min((r.seconds for r in valid), default=1)
        smallest = min((r.size for r in valid), default=1)
        lines += [f"## {case.title}", "", "| herramienta | tiempo | tamaño | tiempo relativo | tamaño relativo |",
                  "|---|---:|---:|---:|---:|"]
        for r in rows:
            if r.note or r.seconds != r.seconds:
                lines.append(f"| {r.tool} | {fmt_ms(r.seconds)} | {fmt_kb(r.size)} | — | {r.note} |")
            else:
                bold = "**" if r.tool == "prisma" else ""
                lines.append(f"| {bold}{r.tool}{bold} | {fmt_ms(r.seconds)} | {fmt_kb(r.size)} | {comma(f'{r.seconds / fastest:.1f}')}× | {comma(f'{(r.size / smallest - 1) * 100:+.1f}')} % |")
        lines.append("")
    DOCS.parent.mkdir(exist_ok=True)
    DOCS.write_text("\n".join(lines), encoding="utf-8")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("filters", nargs="*")
    ap.add_argument("--runs", type=int, default=3)
    ap.add_argument("--no-docs", action="store_true", help="no reescribir docs/benchmarks.md")
    ap.add_argument("--nota", default="", help="condiciones de la medicion, para el informe")
    args = ap.parse_args()
    if not EXE.exists():
        print(ink("err", f"falta {EXE}: compila con build.ps1"))
        return 2
    WORK.mkdir(parents=True, exist_ok=True)
    selected = [c for c in CASES if not args.filters or any(f in c.key for f in args.filters)]
    print()
    print("  " + ink("acc", "image2png · benchmarks") + ink("dim", f"  ·  {len(selected)} casos  ·  mediana de {args.runs}  ·  Pillow {PIL.__version__} · OpenCV {cv2.__version__}" + (" · ffmpeg" if FFMPEG else "")))
    results = []
    for case in selected:
        rows = measure(case, args.runs, WORK)
        print_case(case, rows)
        results.append((case, rows))
    if not args.no_docs and not args.filters:
        write_markdown(results, args.runs, args.nota)
        print()
        print("  " + ink("dim", f"informe: {DOCS.relative_to(ROOT.parent)}"))
    print()
    return 0


if __name__ == "__main__":
    sys.exit(main())
