# ac2native - port nativo de Ace Combat 2 por recompilación estática

Ver `NOTES.md` para el estado detallado, los hallazgos y cómo retomar.

## Qué hay en el disco (analizado)
| Archivo | Contenido |
|---|---|
| `SLUS_004.04` | cargador, PS-X EXE en 0x801C8000, 92 KB |
| `ACE2.DAT` | contenedor de 226 entradas: `(tamaño_en_palabras | flags<<28, sector_2048)` |
| `ACE2.DAT[0]` | **el juego**: PS-X EXE 0x80018000-0x800B8000 (640 KB), entry 0x8001BF2C. Sin overlays |
| `ACE2.DAT[1]` | módulo intro (logos + FMV): PS-X EXE en 0x80160000 (140 KB), entry 0x80160CC4 |
| `ACE2.DAT[2..225]` | datos: modelos, texturas, misiones, sonido |
| `ACE2.STH` | índice del streaming: 67 entradas `(tipo<<28 | sectores, sector_en_STP)`. Tipo 8 = las 4 películas FMV (63-66); el resto, pistas XA |
| `ACE2.STP` | streaming (LBA 47884): vídeos STR (MDEC) y música/voces XA |

## Arquitectura
1. **CPU → C nativo** (`tools/recomp.py`): 1814 funciones MIPS → C. Ramas con delay slot, 86 tablas de salto,
   llamadas indirectas por tabla, `setjmp` → puntos de reanudación, GTE como funciones C.
   Reloj virtual: cada bloque suma ciclos (`CYC`), los bucles comprueban eventos (`IRQ_CHECK`).
2. **Runtime nativo** (`runtime/`): BIOS de alto nivel, interrupciones, DMA, timers, CD (lee el .bin),
   GPU (rasterizador por software), SPU (mezclador), MDEC, GTE, SDL2+OpenGL.
3. **Sustituciones nativas (hooks)** de rutinas concretas del juego cuando conviene (hecho: reproductor FMV).
4. **Launcher** (frontend/, ImGui) + menú de pausa en juego.
5. (Fase 2) **Instalador**: extrae los ejecutables del disco del usuario, recompila y compila en su PC (el disco
   se sigue leyendo en cada arranque).

## Hitos cumplidos
- [x] H1 Recompilador: el cargador corre nativo y carga el módulo intro.
- [x] H2 Cadena completa cargador → intro → juego principal, interrupciones y VBlank estables.
- [x] H3 Imagen: logos, FMV del intro (reproductor nativo), título y demo de vuelo 3D visibles.
- [x] H4 Entrada (InitPad), FMV nativo, audio verificado (música XA y SPU), tarjeta detectada.
- [x] H5 Misión completa jugada por el usuario (2026-10-01) con buen rendimiento; guardado escrito en la
  tarjeta (card0/BASLUS-00404ACECMBT2, 8 KB).
- [x] H6 Resolución interna 1x/2x/4x con hilos (salida idéntica bit a bit), widescreen, launcher,
  build desde el disco del usuario (extract.py + build.sh), .gitignore.

## Fases a partir de ahora (estado global del port base: ~75-80 %)

### Fase 1 — Juego 100 % completo y estable
- Cargar partida guardada; varias tarjetas; tarjeta llena / sin formatear.
- Todas las misiones, ramas alternativas y secretas (cada una carga código/datos distintos; vigilar
  funciones no descubiertas por el recompilador → error en el log, se añaden como semilla).
- FMV de los finales (películas 64-66 del STH) y créditos.
- Menú OPTION, cambio de avión, dificultades.
- Mando real (Xbox/PS), vibración si es viable.
- Sesiones largas (1 h+): sin fugas ni desincronización de audio.

### Fase 2 — Pulido, instalador y distribución
El port lee el .bin del usuario en cada arranque; lo único que se extrae del disco es lo mínimo para compilar
(los 3 ejecutables → `work/`, con tools/extract.py).
Principio legal: ni el código recompilado (gen/) ni el binario ni datos del juego se distribuyen; el ejecutable
se genera en el PC de cada usuario con su propio disco. Por eso NO hay binario descargable ni AppImage del juego.

- **F2.1 Limpieza** (~1 día): quitar/condicionar el código de depuración (lista en NOTES.md), logs con niveles,
  mensajes de error útiles (disco no encontrado, disco incorrecto, BIOS no necesaria), `--version`.
- **F2.2 Gestión del disco** (~1 día): buscar el disco en rutas habituales y recordar la última; verificar
  (tamaño + hash de SLUS_004.04, solo USA) con mensaje claro si es otra versión; aceptar .bin+.cue y .iso;
  arreglar la ruta rota tras mover el disco; el launcher permite cambiarlo siempre.
- **F2.3 Instalador Linux** (~3 días): `install.sh` (y `uninstall.sh`): comprueba dependencias (python3, cmake,
  ninja, gcc/g++, SDL2-dev) e indica el comando exacto de la distro; pide/localiza el disco; verifica; extrae →
  recompila → compila (ver tiempos abajo); instala en `~/.local/share/AC2Native/` (o carpeta portátil elegida);
  crea acceso directo con icono; informe final y log del proceso (`install.log`).
- **F2.4 Paquete fuente** (~1 día): `ac2native-X.Y.Z-src.tar.gz` (tools, runtime, frontend, third_party, assets de
  UI, scripts, licencias, README) generado por script y comprobado en una máquina limpia (contenedor Docker de
  Debian/Ubuntu/Fedora/Arch) sin acceso al disco hasta el paso correspondiente.
- **F2.5 Windows** (~1-2 semanas, el bloque más grande): ver abajo.
- **F2.6 Documentación y licencias** (~2 días): README (instalar, jugar, controles, opciones, solución de
  problemas), aviso legal (sin material del juego, marcas de sus dueños), licencia del proyecto, atribuciones
  (ImGui MIT, SDL zlib, Roboto Apache-2.0).
- **F2.7 Publicación** (~1 día): repositorio, versionado (0.9 → 1.0), changelog, plantilla de informe de errores
  con "Copiar diagnóstico" en el launcher (versión, SO, GPU, log).

Windows (F2.5) — tareas:
  1. Capa de portabilidad: hilos (pthread → winpthreads o SDL_Thread), nanosleep/clock_gettime, mkdir/readdir,
     readlink("/proc/self/exe") y execv (volver al launcher), sysconf, system("mkdir -p"), rutas de usuario.
  2. OpenGL: constantes/funciones de GL ≥ 1.2 vía carga dinámica (ya se usa SDL_GL_GetProcAddress en parte).
  3. Compilador: el instalador necesita uno en el PC del usuario. Opción recomendada: descargar llvm-mingw (~150 MB,
     permisiva) + Python embebido (~10 MB) + CMake/Ninja al directorio del instalador; alternativa: MSYS2 instalado
     por el usuario. El .exe del instalador NO contiene nada del juego.
  4. Pruebas: sin máquina Windows propia → CI de GitHub Actions (compilar el runtime con un `gen/` sintético) y
     pruebas reales del usuario o de voluntarios; Wine solo como comprobación previa.
  5. Mando/teclado/audio: SDL2 ya es multiplataforma; revisar rutas de configuración y partidas.

Estimación de tiempos del usuario final: extraer + recompilar ~1 s, compilar ~1 min (4 núcleos, 9,4 MB de C).
Fuera de alcance de la Fase 2: macOS, AppImage/Flatpak del juego, cualquier descarga de datos del juego.

Decisiones abiertas (necesito tu respuesta antes de F2.3): licencia del proyecto (¿GPL-2 como ac2launcher, MIT?),
nombre público del proyecto, si Windows es obligatorio para la 1.0 o puede ir después, y dónde alojarlo.

### Fase 3 — Mejoras visuales sobre el renderer por software
- Precisión subpíxel (estilo PGXP; el GTE es nuestro, se conservan decimales) → sin temblor de polígonos.
- Texturas con corrección de perspectiva (guardar Z por vértice).
- Color de 24 bits sin dithering, filtrado de texturas, supermuestreo.
- HUD bien colocado en widescreen (interceptar las funciones que lo dibujan).

### Fase 4 — Renderer por GPU (OpenGL; Vulkan aplazado por el usuario)
- Los comandos GP0 se dibujan en la tarjeta gráfica: 8x/16x o 4K nativo sin coste de CPU, MSAA.
- Imprescindible para que las texturas HD rindan bien.

### Fase 5 — "Remaster" (opcional, ambicioso)
- Z-buffer real (adiós a los cruces de polígonos por orden manual).
- Mayor distancia de dibujado: parchear constantes de recorte lejano y LOD en el C generado/hooks
  (límite: niebla y datos de terreno; los enemigos aparecen según la lógica del juego).
- Iluminación y post-proceso modernos (sombras, bloom, cielo).
- 60 fps con interpolación entre fotogramas (difícil: la lógica va a ritmo fijo).

### Resultado esperado
- Tras Fases 1-2: ejecutable nativo Linux/Windows generado desde la copia del usuario (una sola vez),
  launcher, mando/teclado, guardado, 2x/4x + widescreen. Legal de distribuir (sin material del juego).
- Tras Fases 3-4: calidad visual al nivel de los mejores emuladores (PGXP, HD), nativo y más rápido,
 .
- Tras Fase 5: algo cercano a un remaster, mismo juego y misiones con imagen moderna.
