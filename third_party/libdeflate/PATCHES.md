# Cambios de prisma sobre libdeflate 1.26

libdeflate se incluye como fuente (sin submódulo) con **un solo cambio**: una función nueva para
comprimir un flujo DEFLATE en tramos independientes, la técnica de `pigz`. Todo lo demás es la
versión 1.26 original (`NEWS.md`, `COPYING` con su licencia MIT).

Cada línea tocada lleva la marca `[prisma]` en un comentario:

```
grep -n "prisma" lib/deflate_compress.c libdeflate.h
```

## Para qué

libdeflate comprime muy rápido pero de a un bloque de memoria por llamada y en un solo hilo. Una
imagen de 50 MP son ~200 MB de filas filtradas: con un hilo, el deflate se vuelve el cuello de
botella de todo el conversor. Partiendo la entrada en tramos de 1 MB que se comprimen **en
paralelo** y se concatenan, el tiempo de compresión se divide por la cantidad de núcleos, a cambio
de ~0,1 % de tamaño (cada tramo arranca con el diccionario de 32 KB vacío).

Para que la concatenación sea **un** flujo DEFLATE válido hacen falta dos cosas que la API pública
no ofrece:

1. Ningún bloque de un tramo que no sea el último puede llevar `BFINAL = 1`.
2. Cada tramo que no sea el último tiene que terminar alineado a byte, para que el siguiente empiece
   en un byte nuevo. Eso lo hace un bloque guardado vacío — el "sync flush" de zlib: 3 bits
   `000`, relleno hasta el byte, y `LEN = 0000`, `NLEN = FFFF`.

## Qué se cambió

### `lib/deflate_compress.c`

- `struct libdeflate_compressor` gana el campo `bool prisma_not_final`. Mientras vale `true`, todo
  bloque se emite con `BFINAL = 0`.
- `libdeflate_alloc_compressor_ex()` lo inicializa en `false` (la estructura no se pone en cero).
- `deflate_flush_block()`: `is_final_block = is_final_block && !c->prisma_not_final;` al principio.
- Función nueva `libdeflate_deflate_compress_chunk()`:
  - tramo final → es exactamente `libdeflate_deflate_compress()`;
  - tramo intermedio chico (camino de bloques guardados) → los bloques guardados ya quedan
    alineados; solo se escribe su cabecera con `BFINAL = 0`;
  - tramo intermedio normal → compresión con `prisma_not_final = true` y después el sync flush
    (`00 00 FF FF` tras vaciar los bits pendientes).

### `libdeflate.h`

- Declaración de `libdeflate_deflate_compress_chunk()` con su contrato: la salida de un tramo
  intermedio puede necesitar hasta 6 bytes más que `libdeflate_deflate_compress_bound()`.

## Lo que queda del lado de prisma (`src/codec/zlib.c`)

- La cabecera zlib (`78 xx`) va una vez al principio; el adler32 va una vez al final.
- El adler32 total se arma **combinando** los adler32 de cada tramo con aritmética modular
  (`adler32_combine`, igual que el de zlib), sin volver a recorrer los datos.
- Las pruebas unitarias (`tests/unit`) verifican la ida y vuelta con tramos de todos los tamaños
  de borde (0, 1, justo debajo y encima de 1 MB, varios MB) y la combinación de adler32 contra el
  cálculo directo.

## Actualizar libdeflate

1. Reemplazar la carpeta por la versión nueva.
2. Volver a aplicar los tres cambios de arriba (buscar `deflate_flush_block`, el `alloc` del
   compresor y agregar la función al final de la parte de compresión).
3. `.\build.ps1 -Tools unit` y `build\release\unit.exe deflate`.
