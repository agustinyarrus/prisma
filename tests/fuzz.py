"""fuzz.py - fuzzing por mutacion de image2png (por defecto contra el build con AddressSanitizer).

    python tests/fuzz.py                      2000 mutaciones contra build/asan/image2png.exe
    python tests/fuzz.py -n 10000 --seed 7    mas y con otra semilla
    python tests/fuzz.py --exe bin/image2png.exe

Semillas: las entradas que genero la suite (tests/out/suite), una variedad por formato. Cada
mutante lleva de 1 a 3 mutaciones apiladas. Los mutantes corren en LOTES (un proceso para muchos
archivos: el arranque de WIC se paga una vez); si un lote se cae, se cuelga o ASan reporta algo,
se biseca archivo por archivo y el culpable queda guardado con su reporte y el comando para
reproducirlo. Un "fallo interno" atrapado por SEH tambien cuenta como bug: nunca deberia pasar.

Que NO cuenta como bug: que un archivo roto de un error prolijo (codigo 1 con mensaje).
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import random
import shutil
import struct
import subprocess
import sys
import time
import zlib
from collections import Counter, defaultdict
from dataclasses import dataclass, field
from pathlib import Path

ROOT = Path(__file__).resolve().parent
SUITE = ROOT / "out" / "suite"
OUT = ROOT / "out" / "fuzz"
DEFAULT_EXE = ROOT.parent / "build" / "asan" / "image2png.exe"

ASAN_EXIT = 86
EXPECTED_EXIT = {0, 1}
BATCH = 48
BATCH_TIMEOUT = 300                      # s por lote (ASan + WIC es lento, pero no tanto)
FILE_TIMEOUT = 60                        # s por archivo al bisecar
MAX_SEED_BYTES = 192 * 1024
SEEDS_PER_EXT = 6
HEADER_BYTES = 96                        # la mitad de las mutaciones caen en la cabecera

# extensiones que el exe lee (propias y via WIC); AVIF/JPEG ejercitan heif.c, exif.c y los APPn
EXTENSIONS = {"png", "apng", "gif", "bmp", "dib", "ico", "cur", "tga", "qoi", "pnm", "pbm", "pgm", "ppm", "pam",
              "pfm", "ff", "farbfeld", "psd", "psb", "hdr", "pcx", "dds", "sgi", "rgb", "rgba", "bw",
              "jpg", "jpeg", "tif", "tiff", "webp", "avif", "jxr"}
SLOW_EXTENSIONS = {"avif", "webp", "tif", "tiff", "jxr", "jpg", "jpeg"}     # WIC: se usan menos

INTERESTING_8 = [0x00, 0x01, 0x7F, 0x80, 0xFF, 0xFE, 0x10, 0x20, 0x40]
INTERESTING_32 = [0, 1, 2, 0x7F, 0x80, 0xFF, 0x100, 0x7FFF, 0x8000, 0xFFFF, 0x10000, 0x7FFFFFFF,
                  0x80000000, 0xFFFFFFFF, 0xFFFFFFFE, 0x40000000, 65535 * 65535]

# Operaciones que se agregan al azar a cada lote: el pipeline entero ve pixeles "raros".
OP_SETS = [[], [], ["--remove-bg"], ["--colors", "64"], ["--resize", "37%"], ["--trim", "--bleed"],
           ["--defringe", "2", "--premultiply"], ["--rotate", "90", "--flip", "h"], ["--frames", "all"],
           ["--key", "white", "--fuzz", "10", "--feather", "5"], ["--color-to-alpha", "black"],
           ["--grayscale", "--depth", "16"], ["--alpha", "remove"], ["--unmatte", "white", "--clean"]]

# paleta de la terminal (Catppuccin sobre negro)
INK = {"ok": (181, 223, 168), "err": (245, 163, 181), "warn": (238, 223, 184), "acc": (196, 181, 253),
       "dim": (127, 132, 156), "num": (168, 207, 242), "text": (205, 214, 244), "teal": (143, 214, 204)}
COLOR = sys.stdout.isatty() and not os.environ.get("NO_COLOR")


def ink(name: str, text: str) -> str:
    if not COLOR:
        return text
    r, g, b = INK[name]
    return f"\x1b[38;2;{r};{g};{b}m{text}\x1b[0m"


# ---------------------------------------------------------------------------------------------------
# semillas
# ---------------------------------------------------------------------------------------------------

def collect_seeds(rng: random.Random) -> list[Path]:
    by_ext: dict[str, list[Path]] = defaultdict(list)
    for path in SUITE.rglob("*"):
        if not path.is_file() or "out" in path.relative_to(SUITE).parts[:-1][2:] or path.parent.name == "out":
            continue
        ext = path.suffix.lower().lstrip(".")
        if ext in EXTENSIONS and 0 < path.stat().st_size <= MAX_SEED_BYTES:
            by_ext[ext].append(path)
    seeds = []
    for ext, paths in sorted(by_ext.items()):
        paths.sort()
        rng.shuffle(paths)
        seeds += paths[:SEEDS_PER_EXT if ext not in SLOW_EXTENSIONS else 2]
    return seeds


# ---------------------------------------------------------------------------------------------------
# mutaciones
# ---------------------------------------------------------------------------------------------------

def spot(rng: random.Random, data: bytearray, width: int = 1) -> int:
    """Posicion a mutar: la mitad de las veces dentro de la cabecera, donde viven los tamaños."""
    limit = max(1, len(data) - width)
    if rng.random() < 0.5:
        return rng.randrange(min(limit, HEADER_BYTES))
    return rng.randrange(limit)


def m_truncate(rng, data):
    return data[:rng.randrange(max(1, len(data)))]


def m_bitflip(rng, data):
    if not data:                                   # otra mutacion pudo dejar el archivo vacio
        return data
    for _ in range(rng.randint(1, 8)):
        i = spot(rng, data)
        data[i] ^= 1 << rng.randrange(8)
    return data


def m_byte(rng, data):
    if not data:
        return data
    for _ in range(rng.randint(1, 4)):
        data[spot(rng, data)] = rng.choice(INTERESTING_8)
    return data


def m_word(rng, data):
    width = rng.choice((2, 4))
    if len(data) < width:
        return data
    i = spot(rng, data, width)
    value = rng.choice(INTERESTING_32) & ((1 << (8 * width)) - 1)
    order = rng.choice("<>")
    data[i:i + width] = struct.pack(order + ("H" if width == 2 else "I"), value)
    return data


def m_duplicate(rng, data):
    if len(data) < 4:
        return data
    a = rng.randrange(len(data))
    n = rng.randint(1, min(512, len(data) - a))
    at = rng.randrange(len(data))
    return data[:at] + data[a:a + n] + data[at:]


def m_delete(rng, data):
    if len(data) < 4:
        return data
    a = rng.randrange(len(data))
    n = rng.randint(1, min(512, len(data) - a))
    return data[:a] + data[a + n:]


def m_random_block(rng, data):
    if not data:
        return data
    a = spot(rng, data)
    n = rng.randint(1, 64)
    data[a:a + n] = bytes(rng.randrange(256) for _ in range(min(n, len(data) - a)))
    return data


MUTATORS = [m_truncate, m_bitflip, m_bitflip, m_byte, m_word, m_word, m_word, m_duplicate, m_delete, m_random_block]


def repair_png_crcs(data: bytearray) -> bytearray:
    """Recalcula los CRC de los chunks que siguen bien formados: asi la mutacion llega al
    decodificador en vez de morir en el control de integridad."""
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        return data
    at = 8
    while at + 12 <= len(data):
        n = struct.unpack(">I", data[at:at + 4])[0]
        if n > len(data) - at - 12:
            break
        crc = zlib.crc32(bytes(data[at + 4:at + 8 + n])) & 0xFFFFFFFF
        data[at + 8 + n:at + 12 + n] = struct.pack(">I", crc)
        at += 12 + n
    return data


def mutate(rng: random.Random, seed: bytes) -> bytes:
    data = bytearray(seed)
    for _ in range(rng.choice((1, 1, 2, 3))):
        data = bytearray(rng.choice(MUTATORS)(rng, data))
    if rng.random() < 0.7:
        data = repair_png_crcs(data)
    return bytes(data)


# ---------------------------------------------------------------------------------------------------
# ejecucion
# ---------------------------------------------------------------------------------------------------

@dataclass
class Finding:
    kind: str                    # asan | crash | hang | internal
    path: Path
    detail: str


@dataclass
class Stats:
    per_ext: Counter = field(default_factory=Counter)
    outcomes: Counter = field(default_factory=Counter)
    findings: list[Finding] = field(default_factory=list)


def run(exe: Path, files: list[Path], outdir: Path, ops: list[str], timeout: float):
    outdir.mkdir(parents=True, exist_ok=True)
    cmd = [str(exe), *map(str, files), "-o", str(outdir), "--json", "-j", "8", "--fast", "--max-pixels", "16",
           "--ignore-crc", "--plain", *ops]
    env = dict(os.environ, ASAN_OPTIONS=f"exitcode={ASAN_EXIT}:halt_on_error=1:allocator_may_return_null=1")
    try:
        p = subprocess.run(cmd, capture_output=True, timeout=timeout, env=env)
    except subprocess.TimeoutExpired:
        return "hang", "", ""
    out, err = p.stdout.decode("utf-8", "replace"), p.stderr.decode("utf-8", "replace")
    if "AddressSanitizer" in err or p.returncode == ASAN_EXIT:
        return "asan", out, err
    if p.returncode not in EXPECTED_EXIT:
        return f"crash 0x{p.returncode & 0xFFFFFFFF:08X}", out, err
    return "ok", out, err


def save_finding(kind: str, path: Path, ops: list[str], exe: Path, report: str) -> Path:
    digest = hashlib.sha1(path.read_bytes()).hexdigest()[:12]
    dest = OUT / "hallazgos" / f"{kind.split()[0]}-{digest}{path.suffix}"
    dest.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(path, dest)
    repro = f'"{exe}" "{dest}" -o salida.png --json --fast --max-pixels 16 --ignore-crc {" ".join(ops)}'
    dest.with_suffix(dest.suffix + ".txt").write_text(f"{kind}\n\n{repro}\n\n{report}", encoding="utf-8")
    return dest


def json_results(stdout: str) -> dict[str, dict]:
    results = {}
    for line in stdout.splitlines():
        if line.startswith("{") and '"input"' in line:
            try:
                row = json.loads(line)
                results[row["input"]] = row
            except (json.JSONDecodeError, KeyError) as exc:
                results[f"?{len(results)}"] = {"status": "error", "message": f"JSON ilegible: {exc}"}
    return results


def process_batch(exe: Path, files: list[Path], ops: list[str], batch_dir: Path, stats: Stats) -> None:
    outcome, out, err = run(exe, files, batch_dir / "out", ops, BATCH_TIMEOUT)
    if outcome == "ok":
        rows = json_results(out)
        for f in files:
            row = next((r for k, r in rows.items() if k.endswith(f.name)), None)
            status = row.get("status", "?") if row else "sin-reporte"
            stats.outcomes[status] += 1
            message = (row or {}).get("message", "")
            if "fallo interno" in message:
                dest = save_finding("internal", f, ops, exe, message)
                stats.findings.append(Finding("internal", dest, message[:120]))
        return
    # El lote entero cayo: se biseca archivo por archivo.
    for f in files:
        single, out1, err1 = run(exe, [f], batch_dir / "solo", ops, FILE_TIMEOUT)
        if single == "ok":
            stats.outcomes["ok-aislado"] += 1
            continue
        report = next((l for l in err1.splitlines() if "ERROR: AddressSanitizer" in l), err1.strip()[:300])
        dest = save_finding(single, f, ops, exe, err1 or out1)
        stats.findings.append(Finding(single, dest, report[:160]))
        stats.outcomes[single.split()[0]] += 1


# ---------------------------------------------------------------------------------------------------
# presentacion
# ---------------------------------------------------------------------------------------------------

def progress(done: int, total: int, stats: Stats, t0: float) -> None:
    if not COLOR:                                  # a un archivo: una linea cada ~10 %
        step = max(total // 10, 1)
        if done % step < BATCH or done == total:
            print(f"  {done}/{total} · {done / max(time.perf_counter() - t0, 1e-9):.1f}/s · {len(stats.findings)} hallazgos", flush=True)
        return
    width = 36
    filled = int(width * done / max(total, 1))
    rate = done / max(time.perf_counter() - t0, 1e-9)
    bugs = len(stats.findings)
    bar = ink("acc", "━" * filled) + ink("dim", "─" * (width - filled))
    status = ink("err", f"{bugs} hallazgos") if bugs else ink("ok", "0 hallazgos")
    print(f"\r  {bar}  {ink('num', f'{done:>6}')}{ink('dim', f'/{total}')}  {ink('dim', f'{rate:5.1f}/s')}  {status}   ", end="", flush=True)


def summary(stats: Stats, total: int, seconds: float, exe: Path, seeds: int) -> None:
    width = 84
    title = " image2png · fuzzing "
    lines = []
    bugs = len(stats.findings)
    lines.append((ink("ok" if not bugs else "err", f"{bugs} hallazgos") + ink("dim", f"   ·   {total} mutantes de {seeds} semillas   ·   {seconds:.1f} s   ·   {total / max(seconds, 1e-9):.1f}/s"),
                  len(f"{bugs} hallazgos   ·   {total} mutantes de {seeds} semillas   ·   {seconds:.1f} s   ·   {total / max(seconds, 1e-9):.1f}/s")))
    lines.append((ink("dim", f"exe {exe.parent.name}/{exe.name}"), len(f"exe {exe.parent.name}/{exe.name}")))
    outcome_text = "   ".join(f"{k} {v}" for k, v in stats.outcomes.most_common())
    lines.append((ink("text", outcome_text), len(outcome_text)))
    exts = sorted(stats.per_ext.items(), key=lambda kv: -kv[1])
    for i in range(0, len(exts), 6):
        row = "   ".join(f"{e:<5}{n:>5}" for e, n in exts[i:i + 6])
        lines.append((ink("dim", row), len(row)))
    for f in stats.findings[:8]:
        text = f"✗ {f.kind:<14} {f.path.name}  {f.detail}"[:width - 2]
        lines.append((ink("err", text), len(text)))
    print()
    print()
    print("  " + ink("dim", "╭─") + ink("acc", title) + ink("dim", "─" * (width - len(title) - 1) + "╮"))
    for text, raw in lines:
        print("  " + ink("dim", "│ ") + text + " " * max(0, width - 1 - raw) + ink("dim", "│"))
    print("  " + ink("dim", "╰" + "─" * width + "╯"))
    print()


# ---------------------------------------------------------------------------------------------------

def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("-n", "--count", type=int, default=2000)
    ap.add_argument("--seed", type=int, default=20260923)
    ap.add_argument("--exe", type=Path, default=DEFAULT_EXE)
    args = ap.parse_args()
    exe = args.exe.resolve()
    if not exe.exists():
        print(ink("err", f"falta {exe}: compila con build.ps1 -Mode asan (o pasa --exe)"))
        return 2
    rng = random.Random(args.seed)
    seeds = collect_seeds(rng)
    if not seeds:
        print(ink("err", "no hay semillas: corre antes la suite (python tests/suite.py)"))
        return 2
    seed_bytes = {s: s.read_bytes() for s in seeds}
    work = OUT / "lotes"
    shutil.rmtree(work, ignore_errors=True)
    stats = Stats()
    print()
    print("  " + ink("acc", "image2png · fuzzing") + ink("dim", f"  ·  {len(seeds)} semillas  ·  {args.count} mutantes  ·  lotes de {BATCH}  ·  semilla {args.seed}"))
    print()
    t0 = time.perf_counter()
    done = 0
    batch_no = 0
    while done < args.count:
        n = min(BATCH, args.count - done)
        batch_dir = work / f"{batch_no:05d}"
        batch_dir.mkdir(parents=True)
        files = []
        for i in range(n):
            seed = rng.choice(seeds)
            ext = seed.suffix.lower()
            stats.per_ext[ext.lstrip(".")] += 1
            path = batch_dir / f"m{i:03d}{ext}"
            path.write_bytes(mutate(rng, seed_bytes[seed]))
            files.append(path)
        ops = rng.choice(OP_SETS)
        process_batch(exe, files, ops, batch_dir, stats)
        shutil.rmtree(batch_dir, ignore_errors=True)
        done += n
        batch_no += 1
        progress(done, args.count, stats, t0)
    elapsed = time.perf_counter() - t0
    summary(stats, done, elapsed, exe, len(seeds))
    report = {"mutants": done, "seconds": elapsed, "seeds": len(seeds), "exe": str(exe),
              "outcomes": dict(stats.outcomes), "per_ext": dict(stats.per_ext),
              "findings": [{"kind": f.kind, "file": str(f.path), "detail": f.detail} for f in stats.findings]}
    (OUT / "fuzz-report.json").write_text(json.dumps(report, ensure_ascii=False, indent=1), encoding="utf-8")
    return 1 if stats.findings else 0


if __name__ == "__main__":
    sys.exit(main())
