# prisma · image2png

Convierte **cualquier imagen a PNG** desde la consola. Un solo `.exe` de ~800 KB, sin
dependencias ni instalación, escrito en C17 y compilado con clang (LTO, SSE4 + AVX2 elegido al
arrancar). Lee 13 formatos con decodificadores propios y todo lo demás a través de los codecs de
Windows; escribe el PNG más chico que puede sin perder un solo bit.

Y sobre todo: **entiende la transparencia venga como venga** — canal alfa, color clave, paleta
con alfa, máscara de 1 bit, color premultiplicado, color mateado sobre blanco, imagen auxiliar de
AVIF/HEIC — y la entrega como alfa directo y exacto. Además la puede fabricar, limpiar o quitar.

```
image2png foto.heic                          foto.png
image2png -r fotos -o png --keep-tree        toda una carpeta, en paralelo, con subcarpetas
image2png logo.jpg --remove-bg               fondo afuera, bordes suaves y sin halo
image2png animacion.gif                      APNG con transparencia de 8 bits
image2png icono.png --colors 64              PNG8 con alfa suave: una fracción del tamaño
```

Las 88 opciones, agrupadas: `image2png --help`. Recetas: `image2png --examples`. Qué tiene una
imagen (alfa, colores, perfil y el mejor PNG posible) sin convertirla: `image2png -i archivo`.

`prisma` es la suite; `image2png` es su primera herramienta. El build y los módulos (`src/core`,
`src/codec`, `src/png`, `src/ops`) están pensados para que otras herramientas `*2png` compartan
todo el motor.

---

## Qué lee

| formato | decodificador | lo que sabe de transparencia |
|---|---|---|
| PNG / APNG | propio | los 15 tipos de color y profundidad, Adam7, `tRNS` gris/RGB/paleta, composición APNG con descarte y mezcla Porter-Duff reales, CRC roto con `--ignore-crc`, rescate de archivos cortados |
| GIF 87a/89a | propio | índice transparente, los 4 modos de descarte, entrelazado, retardos como los navegadores |
| BMP / DIB | propio | OS/2 y V1–V5, `BITFIELDS`/`ALPHABITFIELDS`, RLE4/RLE8 con saltos (quedan transparentes), 32 bits con el 4.º byte en cero = opaco |
| ICO / CUR | propio | todos los tamaños, DIB o PNG adentro, máscara AND, píxeles "inversos", punto caliente de cursores |
| TGA | propio | RLE, paletas, 4 orígenes, pie 2.0: alfa útil, a conservar o **premultiplicado** |
| PSD / PSB | propio | compuesto de 1/8/16/32 bits, RGB/CMYK/Lab/gris/paleta, **quita el mate blanco** del compuesto |
| DDS | propio | BC1–BC7, BC6H (HDR), DXT2/4 y modo **premultiplicado** (8, 16 bits y flotante), 25+ formatos DXGI |
| QOI, PNM/PAM/PFM, farbfeld, SGI, PCX, Radiance HDR | propios | PAM con alfa, farbfeld RGBA 16, SGI RGBA RLE, PCX RGBA, HDR/PFM con tonemap |
| JPEG, TIFF, JPEG XR, WebP, JPEG XL, RAW/DNG | codecs de Windows (WIC) | TIFF con alfa asociado y no asociado, CMYK con perfil |
| **HEIC / AVIF** | WIC + lector ISOBMFF propio | **alfa auxiliar recuperado** (Windows lo ignora), color premultiplicado (`prem`), matriz YCbCr corregida, primarios en `cICP` |

El formato se decide por el **contenido**, no por la extensión (un `.png` que en realidad es un
JPEG se lee igual). Lo que no se puede leer falla con un mensaje claro: JPEG 2000, OpenEXR, PDF,
XCF, ICNS, WMF/EMF, o un WebP/HEIC/AVIF/JPEG XL sin el codec de Microsoft Store instalado (dice
cuál instalar). `image2png --formats` lista lo que hay en tu equipo.

---

## Transparencia: lo que hace distinto

**Al leer** cada formato trae su trampa, y cada una está resuelta:

- **Premultiplicado** (TGA 2.0, DDS, DXT2/4, AVIF con `prem`) → se divide con redondeo entero
  exacto; en flotante, en luz lineal y *antes* del tonemap. Los formatos premultiplicados que
  llegan por WIC (PBGRA) los convierte el propio codec.
- **Mateado** (el compuesto de PSD está mezclado con blanco) → se des-mezcla: `c = (c − (1−a)·m) / a`.
- **Alfa que no es alfa** (BMP/TGA de 32 bits con el 4.º byte en cero, bit de atributo sin usar
  en TGA de 16) → opaco, con aviso.
- **Máscaras** (AND de ICO, saltos de RLE en BMP, índice transparente de GIF, `tRNS`) → alfa de
  8 bits sin halos.
- **AVIF/HEIC**: el codec de Windows descarta el alfa, que viaja como imagen auxiliar. prisma lee
  las cajas ISOBMFF, arma una copia del archivo donde el alfa es la imagen principal, la decodifica
  con el mismo codec (profundidad, rango y mosaicos incluidos), la alinea con las rotaciones
  `irot`/`imir` de cada ítem y la pega. Resultado: alfa idéntico al de libavif.
- **Animaciones** (GIF, APNG) → composición Porter-Duff real sobre lienzo translúcido (Pillow, por
  ejemplo, compone mal cuando el destino ya es semitransparente).

**Al procesar**, 31 de las 88 opciones son de transparencia (las que trabajan de a píxel se
fusionan en una sola pasada por la memoria):

| opción | qué hace |
|---|---|
| `--remove-bg [color]` | borra el fondo **conectado al borde** (relleno por renglones, O(n)) y des-mezcla una banda a los dos lados de la frontera: estima el color puro del sujeto propagando el interior por BFS y separa cada píxel de borde en sujeto + fondo. El antialias queda suave y sin ribete, y el blanco encerrado (los ojos de alguien, el hueco de una letra) no se toca |
| `--color-to-alpha color` | el algoritmo de GIMP/GEGL: el mínimo alfa que explica cada píxel como mezcla con ese color; conserva sombras y antialias |
| `--key color --fuzz % --feather %` | color clave con tolerancia y rampa suave |
| `--unmatte color`, `--unpremultiply`, `--premultiply` | reparar o producir mate y premultiplicado |
| `--defringe N`, `--bleed`, `--clean` | quitar ribetes; extender el color bajo alfa 0 (texturas sin halos en mipmaps); normalizar lo invisible |
| `--mask img`, `--alpha-from luma\|inverse-luma\|r\|g\|b`, `--extract-alpha`, `--invert-alpha` | el alfa desde otra imagen o un canal, o el alfa como imagen |
| `--threshold`, `--alpha-levels`, `--opacity` | alfa binario, niveles con gamma, opacidad global |
| `--flatten color [--linear]`, `--alpha remove` | componer sobre un fondo, opcionalmente en luz lineal |
| `--resize … [--linear]` | reescala **premultiplicado** (sin halos oscuros en los bordes), 7 filtros, opcionalmente en luz lineal |
| `--trim`, `--pad`, `--crop`, `--rotate`, `--flip` | la geometría: en animaciones el recorte es la unión de todos los cuadros |
| `--colors N` | paleta de N colores **con alfa**: ver abajo |

**`--colors N`, cuantización consciente del alfa.** Dos colores con alfa se comparan como se verían
sobre negro y sobre blanco (promedio de los dos errores, premultiplicado y ponderado por canal).
Esa métrica es una forma cuadrática definida positiva, así que k-means la minimiza exactamente:
median cut con cola de prioridad y corte de varianza mínima, Lloyd hasta converger con búsqueda del
vecino más cercano podada, re-siembra de centros vacíos, y remapeo directo, Floyd-Steinberg
serpenteante o Bayer 8×8. Medido: **0,99×** el costo de un k-means++ de 8 semillas y 300
iteraciones, y el dithering baja 11 % el error del color promedio local.

---

## El PNG que escribe

- **Tipo de color y profundidad mínimos sin pérdida**: paleta con `tRNS` (translúcidos primero),
  gris de 1/2/4 bits, color clave en `tRNS`, 16 bits solo si se usan de verdad.
- **Filtros por fila** (`none`, `sub`, `up`, `avg`, `paeth`, `minsum`, `entropy`, `bigrams` o
  `brute`: el que de verdad comprime menos), elegidos en paralelo. En el nivel por defecto, una
  **carrera sobre una muestra** de la imagen elige la estrategia: ninguna gana siempre (medido:
  `minsum`, la heurística de libpng, pierde 20–27 % en gráficos planos; `none` pierde 40–60 % en
  fotos). En 16 bits la misma carrera decide el nivel de deflate: sobre el ruido del byte bajo, el
  nivel 1 de libdeflate le gana al 9 por ~4 %.
- **Deflate de libdeflate en tramos paralelos** (la técnica de pigz, con un parche mínimo
  documentado en [`third_party/libdeflate/PATCHES.md`](third_party/libdeflate/PATCHES.md)).
- **Niveles** `-O0` (`--fast`) a `-O4` (`--max`: prueba todo).
- **Color**: perfil ICC → `iCCP` (y si el perfil es sRGB, el chunk `sRGB` de 13 bytes); códigos
  CICP → chunk **`cICP`** de PNG 3.ª edición (Display P3, BT.2020, HDR); `--icc srgb` convierte los
  píxeles (perfil ICC con el motor de color de Windows; CICP en luz lineal con adaptación de
  Bradford).
- **Metadatos** a elección (`--meta none|color|all`): EXIF (con la orientación ya aplicada), XMP,
  textos UTF-8, DPI.
- **Escritura atómica** (archivo temporal + reemplazo), rutas largas y Unicode.

---

## Rendimiento

Mediana de 3 corridas en un i7-1165G7 (4 núcleos / 8 hilos), trabajo completo (leer, decodificar,
codificar, escribir) y cada salida verificada sin pérdida antes de medir. Detalle completo y
condiciones en [`docs/benchmarks.md`](docs/benchmarks.md).

| caso | **prisma** | Pillow | Pillow `optimize` | OpenCV nivel 9 | ffmpeg |
|---|---|---|---|---|---|
| foto JPEG 3840×2400 | **843 ms · 2.231 KB** | 1.242 ms · 2.367 KB | 3.732 ms · 2.140 KB | 15,6 s · 2.254 KB | 1.265 ms · 2.487 KB |
| foto con alfa 3840×2400 | **1,35 s · 3.410 KB** | 1,84 s · 3.644 KB | 7,49 s · 3.280 KB | 26,6 s · 3.439 KB | 1,76 s · 3.810 KB |
| 16 bits 2048×1536 | **550 ms · 12,4 MB** | pierde los 16 bits | pierde los 16 bits | 3,53 s · 12,9 MB | 1,51 s · 12,4 MB |
| GIF de 72 cuadros → APNG | **474 ms · 113 KB** | 1.019 ms · 235 KB | — | — | 1.452 ms · 236 KB |
| carpeta de 24 capturas | **508 ms · 2.509 KB** | 1.270 ms · 2.700 KB | 3.718 ms · 2.644 KB | 6,17 s · 2.594 KB | 5,20 s · 2.795 KB |
| captura 1920×1080 | 215 ms · **107 KB** | 101 ms · 117 KB | 276 ms · 115 KB | 404 ms · 114 KB | 363 ms · 126 KB |
| logo 1024×1024 con alfa | 197 ms · **69 KB** | 71 ms · 85 KB | 160 ms · 83 KB | 324 ms · 86 KB | 387 ms · 92 KB |

Cómo leerla:

- prisma es una **línea de comandos**: en cada corrida paga crear el proceso, que en esta máquina
  cuesta 200–400 ms (el antivirus corporativo inyecta en cada proceso nuevo: `cmd /c exit` tarda
  419 ms de mediana). Pillow y OpenCV corren dentro del mismo Python y no lo pagan; por eso en una
  imagen chica suelta Pillow es más rápido, y en una carpeta —un solo proceso, archivos en
  paralelo— prisma es 2,5× más rápido que Pillow y deja todo más chico.
- En fotos, Pillow `optimize` deja el PNG un 4 % más chico que el nivel por defecto de prisma a
  4–5,5× el tiempo; `prisma --max` le gana a todos (1.975 KB en la foto JPEG) a cambio de ~1 minuto.
- Con `--fast` la foto JPEG 4K pasa en 345 ms.

---

## Cómo se prueba

| capa | qué | resultado |
|---|---|---|
| suite de punta a punta (`tests/suite.py`) | 236 casos: cada uno fabrica su entrada, corre el `.exe` real y compara con un oráculo exacto (la imagen con que se generó) o con un decodificador independiente (Pillow, OpenCV, un lector PNG/APNG propio escrito según la especificación) | 236/236 |
| la misma suite con **AddressSanitizer** | `build.ps1 -Mode asan`: en ese build la holgura de cada bloque de memoria está envenenada, así que un byte leído de más explota | 236/236, 0 reportes |
| pruebas unitarias en C (`tests/unit`) | conversiones enteras **exhaustivas**, el álgebra del grupo D4, matrices CICP contra la norma, deflate en tramos, filtros SIMD contra la definición escalar, UTF-8, y el lector ISOBMFF frente a **todos** sus truncamientos + 20.000 corrupciones | 20/20, 133.056 chequeos |
| fuzzing por mutación (`tests/fuzz.py`) | truncar, invertir bits, palabras "interesantes", bloques duplicados o borrados, CRC de PNG reparado para llegar más hondo; con operaciones al azar para ejercitar todo el pipeline; bisección automática de cualquier caída | **22.000 mutantes, 0 hallazgos** (16.000 con la holgura envenenada) |
| benchmarks (`tests/bench.py`) | contra Pillow, OpenCV y ffmpeg, verificando que cada salida sea sin pérdida antes de medir | [`docs/benchmarks.md`](docs/benchmarks.md) |

```
python tests/suite.py -j 6                      suite completa
build\release\unit.exe                          pruebas unitarias
python tests/fuzz.py -n 5000                    fuzzing (usa build\asan\image2png.exe)
python tests/bench.py                           benchmarks (reescribe docs/benchmarks.md)
```

Las pruebas necesitan Python 3.10+ con `numpy`, `Pillow` (con AVIF) y `opencv-python`; los benchmarks usan además `ffmpeg` si está en el PATH.

---

## Compilar

Requisitos: Windows 10/11 x64 y **Visual Studio 2022** con *Desarrollo de escritorio con C++* y
el componente *C++ Clang tools for Windows*, y PowerShell 7 (compila todas las fuentes en paralelo).

```
.\build.ps1                      release (LTO) -> bin\image2png.exe
.\build.ps1 -Mode dev            sin optimizar, con símbolos -> build\dev\
.\build.ps1 -Mode asan           AddressSanitizer -> build\asan\ (con su runtime al lado)
.\build.ps1 -Tools unit          pruebas unitarias -> build\release\unit.exe
.\build.ps1 -Check               solo compila: chequeo rápido de tipos y avisos
```

El entorno de `vcvars64` se cachea (`build\vcvars-env.json`): el build no paga sus 3 segundos
cada vez. Para ASan se usa el runtime del toolset MSVC, porque el de LLVM 19 que trae Visual
Studio no sabe interceptar las funciones de Windows 11 24H2 y se cae al arrancar.

---

## Diseño

```
 archivos ──► inputs (comodines, -r, stdin) ──► pool de hilos: un trabajo por archivo (LPT)
                                                          │
   leer (mapeo ≥ 64 MB) ─► decodificar ─────────────────► │  firma → decodificador propio
                            │ 13 propios, SIMD            │  si no → WIC (+ heif.c: alfa y color)
                            ▼                             │
   pipeline (16 etapas, orden fijo; las de a píxel fusionadas en una pasada)
                            ▼
   analizar (colores únicos, alfa, gris, 16→8) ─► plan de tipo de color ─► empaquetar
                            ▼
   filtrar por fila (paralelo, AVX2) ─► deflate en tramos paralelos ─► escribir atómico
```

| módulo | responsabilidad |
|---|---|
| `src/core` | memoria alineada (con modo ASan), pool de hilos con ayuda mutua, archivos (rutas largas, escritura atómica), texto UTF-8, terminal, CPU, espacio de color, CICP, ICC |
| `src/codec` | decodificadores propios, WIC, lector ISOBMFF (`heif.c`), zlib sobre libdeflate, BCn, EXIF, composición de animaciones |
| `src/ops` | las operaciones: pixelops fusionados, geometría D4, resize, fondo, propagación por BFS, cuantización, pipeline |
| `src/png` | análisis, plan, empaquetado, filtros, escritura de chunks, codificador |
| `src/app` | CLI declarativa (una tabla de opciones), entradas, componentes de interfaz |
| `src/tools/image2png` | la herramienta: configuración, trabajos, nombres de salida, reporte |

Decisiones que atraviesan todo:

- **Una sola representación interna**: RGBA con alfa directo, 8 o 16 bits. Cada operación se
  escribe una vez; el codificador después reduce a lo que la imagen realmente usa.
- **Multiversión SIMD**: los núcleos calientes se escriben una vez como plantilla inline y se
  instancian para SSE4 y AVX2; el despacho se decide al arrancar, no por píxel.
- **Complejidad declarada** en la cabecera de cada función no trivial; nada cuadrático donde el
  tamaño lo decide el archivo.
- **Errores**: el primero gana (explica la causa), los avisos se deduplican, un archivo roto nunca
  tira abajo el lote (excepciones estructuradas por trabajo).

---

## Límites conocidos

Lo que hoy **no** hace, dicho sin vueltas:

- **Formatos**: JPEG 2000, OpenEXR, SVG, PDF, XCF e ICNS no se leen (el error lo dice). PSD: solo
  el compuesto, no las capas. La salida es siempre PNG/APNG.
- **Codecs de Windows**: lo que pasa por WIC depende de lo instalado. HEIC/AVIF/WebP/JPEG XL piden
  las extensiones de Microsoft Store.
- **AVIF/HEIC por WIC**:
  - el codec entrega 8 bits aunque el AVIF sea de 10 o 12;
  - un AVIF con matriz identidad (RGB sin pérdida), YCgCo o de luminancia constante no se puede
    decodificar bien con el codec de Windows: prisma avisa;
  - un color muy saturado que el codec recortó al convertir de YCbCr no vuelve exacto (el resto
    queda en el piso de diferencias con libavif: ±4 niveles);
  - la corrección de matriz está medida y probada para AV1; en HEIC el color queda como lo entrega
    el codec (no hay codificador HEVC para probarlo);
  - AVIF animado (`avis`): se lee el primer cuadro;
  - HDR (PQ/HLG): se mapea desde la salida flotante del codec; `--icc srgb` no convierte CICP HDR.
- **WebP animado**: depende del codec de Windows (no está cubierto por las pruebas).
