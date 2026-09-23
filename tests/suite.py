"""suite.py - prueba de punta a punta de image2png.

    python tests/suite.py              todo
    python tests/suite.py png bmp      solo esos grupos (o nombres que contengan el texto)
    python tests/suite.py -j 8         casos en paralelo (default 4)

Cada caso genera su entrada, corre el exe de verdad y verifica la salida con un decodificador
independiente (Pillow / OpenCV). Un crash (codigo de salida de excepcion) cuenta como falla aunque
el caso esperara un error.
"""
from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass
from pathlib import Path

import numpy as np
from PIL import Image, ImageSequence

sys.path.insert(0, str(Path(__file__).resolve().parent))
import cases  # noqa: E402
import refpng  # noqa: E402

ROOT = Path(__file__).resolve().parent
EXE = cases.EXE
OUT = ROOT / "out" / "suite"
EXPECTED_EXIT = {0, 1, 2, 130}

# paleta de la terminal (Catppuccin claro sobre negro)
INK = {"ok": (181, 223, 168), "err": (245, 163, 181), "warn": (238, 223, 184), "acc": (196, 181, 253),
       "dim": (127, 132, 156), "teal": (143, 214, 204), "num": (168, 207, 242), "text": (205, 214, 244)}
COLOR = sys.stdout.isatty() and not os.environ.get("NO_COLOR")


def ink(name: str, text: str) -> str:
    if not COLOR:
        return text
    r, g, b = INK[name]
    return f"\x1b[38;2;{r};{g};{b}m{text}\x1b[0m"


@dataclass
class Result:
    group: str
    name: str
    ok: bool
    detail: str
    seconds: float
    crashed: bool = False

    def __post_init__(self):
        self.ok = bool(self.ok)                 # numpy.bool_ no es serializable a JSON
        self.detail = str(self.detail)
        self.seconds = float(self.seconds)


def pillow_frames(path: Path, transform=None) -> list[np.ndarray]:
    im = Image.open(path)
    frames = []
    for f in ImageSequence.Iterator(im):
        g = f.copy()
        if transform:
            g = transform(g)
        frames.append(np.asarray(g.convert("RGBA")))
    return frames


compare = cases.compare_rgba


def run_case(c: cases.Case) -> Result:
    d = OUT / c.group / c.name
    if d.exists():
        shutil.rmtree(d, ignore_errors=True)
    d.mkdir(parents=True, exist_ok=True)
    t0 = time.perf_counter()
    try:
        exp = c.make(d)
    except Exception as e:                                   # la generacion misma fallo: es un bug de la prueba
        return Result(c.group, c.name, False, f"no se pudo generar la entrada: {e!r}", 0)
    (d / "out").mkdir(exist_ok=True)          # carpeta: con una sola entrada, -o out seria un ARCHIVO
    cmd = [str(EXE), *exp.inputs, "-o", "out", "--json", "-j", "2", *c.args, *exp.extra_args]
    env = dict(os.environ, ASAN_OPTIONS=f"exitcode={cases.ASAN_EXIT}:halt_on_error=1")
    try:
        p = subprocess.run(cmd, cwd=d, capture_output=True, timeout=600, env=env)
    except subprocess.TimeoutExpired:
        return Result(c.group, c.name, False, "COLGADO (mas de 600 s)", time.perf_counter() - t0, crashed=True)
    secs = time.perf_counter() - t0
    stderr = p.stderr.decode("utf-8", "replace")
    if "AddressSanitizer" in stderr or p.returncode == cases.ASAN_EXIT:
        report = next((line for line in stderr.splitlines() if "ERROR: AddressSanitizer" in line), stderr.strip()[:160])
        (d / "asan.txt").write_text(stderr, encoding="utf-8")
        return Result(c.group, c.name, False, f"ASAN: {report[:150]}", secs, crashed=True)
    if p.returncode not in EXPECTED_EXIT:
        return Result(c.group, c.name, False, f"CRASH: codigo 0x{p.returncode & 0xFFFFFFFF:08X}", secs, crashed=True)
    lines = [json.loads(line) for line in p.stdout.decode("utf-8", "replace").splitlines() if line.startswith("{")]
    files = [x for x in lines if "input" in x]
    first = files[0] if files else {}
    outs = sorted((d / "out").glob("*.png")) if (d / "out").exists() else []

    if exp.fail:
        msg = " | ".join(x.get("message", "") for x in files) + stderr
        ok = p.returncode != 0 and exp.fail.lower() in msg.lower()
        return Result(c.group, c.name, ok, f"fallo como se esperaba: {first.get('message', msg.strip())[:90]}" if ok
                      else f"se esperaba un error con \"{exp.fail}\"; codigo {p.returncode}: {msg.strip()[:120]}", secs)
    if p.returncode != 0:
        why = first.get("message") or stderr.strip()
        return Result(c.group, c.name, False, f"codigo {p.returncode}: {why[:160]}", secs)
    if exp.expect_files is not None:
        return Result(c.group, c.name, len(outs) == exp.expect_files, f"{len(outs)} archivos (esperados {exp.expect_files})", secs)
    if not outs:
        return Result(c.group, c.name, False, "no se escribio ninguna salida", secs)
    try:
        if exp.check:
            ok, detail = exp.check([str(o) for o in outs], d, first)
            return Result(c.group, c.name, bool(ok), detail, secs)
        if exp.frames is not None:
            got = refpng.read_png(outs[0])
            if len(got) != len(exp.frames):
                return Result(c.group, c.name, False, f"{len(got)} cuadros, se esperaban {len(exp.frames)}", secs)
            worst = "exacto"
            for i, (a, b) in enumerate(zip(got, exp.frames)):
                ok, detail = compare(a, b, exp.tol, exp.compare_hidden)
                if not ok:
                    return Result(c.group, c.name, False, f"cuadro {i}: {detail}", secs)
                if detail != "exacto":
                    worst = detail
            return Result(c.group, c.name, True, f"{len(got)} cuadros = composicion de referencia · {worst}", secs)
        if exp.pillow:
            want = pillow_frames(d / exp.inputs[0], exp.pillow_transform)
            # Nuestra salida animada se lee con el lector de referencia: Pillow compone OVER con
            # "paste" enmascarado, que no es Porter-Duff cuando el destino es translucido.
            got = refpng.read_png(outs[0]) if len(want) > 1 else pillow_frames(outs[0])
            if len(want) != len(got):
                return Result(c.group, c.name, False, f"{len(got)} cuadros, Pillow ve {len(want)}", secs)
            worst = "exacto"
            for i, (a, b) in enumerate(zip(got, want)):
                ok, detail = compare(a, b, exp.tol, exp.compare_hidden)
                if not ok:
                    return Result(c.group, c.name, False, f"cuadro {i}: {detail}", secs)
                if detail != "exacto":
                    worst = detail
            return Result(c.group, c.name, True, f"{len(got)} cuadro(s) = Pillow · {worst}", secs)
        o = cases.read_rgba(outs[0])
        ok, detail = compare(o, exp.truth, exp.tol, exp.compare_hidden)
        return Result(c.group, c.name, ok, detail, secs)
    except Exception as e:
        return Result(c.group, c.name, False, f"error al verificar: {e!r}", secs)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("filters", nargs="*")
    ap.add_argument("-j", "--jobs", type=int, default=4)
    args = ap.parse_args()
    if not EXE.exists():
        print(ink("err", f"falta {EXE}: compila con build.ps1"))
        return 2
    selected = [c for c in cases.CASES if not args.filters or any(f in c.group or f in c.name for f in args.filters)]
    print()
    print("  " + ink("acc", "image2png · suite de pruebas") + ink("dim", f"  ·  {len(selected)} casos  ·  {args.jobs} en paralelo  ·  {EXE.parent.name}/{EXE.name}"))
    print()
    t0 = time.perf_counter()
    results: list[Result] = []
    with ThreadPoolExecutor(args.jobs) as pool:
        for r in pool.map(run_case, selected):
            results.append(r)
            mark = ink("ok", "✓") if r.ok else ink("err", "✗")
            name = f"{r.group} · {r.name}"
            print(f"  {mark} {ink('text' if r.ok else 'err', name.ljust(46))} {ink('dim' if r.ok else 'warn', r.detail[:110])}  {ink('dim', f'{r.seconds * 1000:.0f} ms')}")
    wall = time.perf_counter() - t0
    groups: dict[str, list[Result]] = {}
    for r in results:
        groups.setdefault(r.group, []).append(r)
    passed = sum(r.ok for r in results)
    crashes = sum(r.crashed for r in results)
    width = 78
    print()
    title = " image2png · suite "
    print("  " + ink("dim", "╭─") + ink("acc", title) + ink("dim", "─" * (width - len(title) - 1) + "╮"))
    def row(text: str, raw_len: int):
        print("  " + ink("dim", "│ ") + text + " " * max(0, width - 1 - raw_len) + ink("dim", "│"))
    line = f"{passed}/{len(results)} pasaron"
    row(ink("ok" if passed == len(results) else "err", line) + ink("dim", f"   ·   {wall:.1f} s   ·   crashes {crashes}"),
        len(line) + len(f"   ·   {wall:.1f} s   ·   crashes {crashes}"))
    for g, rs in groups.items():
        ok = sum(r.ok for r in rs)
        bar = "━" * ok + "─" * (len(rs) - ok)
        text = f"{g:<10} {ok:>3}/{len(rs):<3} "
        shown_bar = bar[:40]
        row(ink("text", text) + ink("ok" if ok == len(rs) else "warn", shown_bar), len(text) + len(shown_bar))
    print("  " + ink("dim", "╰" + "─" * width + "╯"))
    failed = [r for r in results if not r.ok]
    if failed:
        print()
        for r in failed:
            print("  " + ink("err", f"✗ {r.group} · {r.name}") + ink("dim", f"  {r.detail}"))
    report = {"passed": passed, "total": len(results), "seconds": wall, "crashes": crashes,
              "results": [r.__dict__ for r in results]}
    (ROOT / "out").mkdir(exist_ok=True)
    (ROOT / "out" / "suite-report.json").write_text(json.dumps(report, ensure_ascii=False, indent=1), encoding="utf-8")
    print()
    return 0 if passed == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
