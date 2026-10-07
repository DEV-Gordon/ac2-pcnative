# ac2native - notas para retomar (actualizado 2026-10-07)

## Estado general (2026-10-07) — LEER PRIMERO
Avance: port base ~85 %. Fase 1 (juego completo y estable) ~85 %; Fase 2 (instalador/distribución) ~40 %; Fases 3-5 0 %.
- Repo git iniciado por el usuario en ac2native/ (rama main, SIN commits aún); `.gitignore` recreado; LICENSE (GPL-2, por
  defecto, sin confirmar) y README.md (inglés, con Quick start y solución de problemas) creados. Compilación desde una
  copia limpia de los 61 archivos verificada: gen/ y ejecutable idénticos, capturas idénticas.
- Se eliminó todo lo de extracción/rework de assets (herramientas, datos y planes de mods). Solo se extraen los 3 ejecutables
  (tools/extract.py). No volver a proponerlo. También se borró el emulador (ac2launcher + AC2-PC → papelera).
- Búsqueda automática del disco (runtime/main.cpp find_disc): solo si la ruta guardada no existe; build.sh SIEMPRE exige la ruta.
- Release: camino A (fuente + instalador) vs B (launcher/motor precompilado + módulo del juego compilado en el PC del usuario con
  pestaña Setup; hitos M0 medir -O0, M1 módulo dlopen, M2 Setup, M3 empaquetado, M4 Windows) — decisión APLAZADA por el usuario.
- Pendiente inmediato: (1) efectos de sonido sin sonar en la misión 2 (la música XA va bien; hay huecos largos sin key-on en
  la SPU) y caídas a 4x; usar los estados del usuario slot2/slot3 y AC2_PERFLOG=1; revisar el desfase 0/1 de
  tools/test/state2script.py comparando con `--load-state`; (2) jugar resto de misiones y finales; (3) F2.1-F2.7 de PLAN.md;
  (4) preguntas abiertas: ¿Windows en la 1.0?, nombre público, nombre en el copyright.
- Entorno: /tmp es RAM (no usar para salidas grandes); pruebas en work/test-out/; disco en `../Rom + BIOS/`.

## Qué NO subir a un repositorio (lista que respalda el .gitignore actual)
`work/` (derivado del disco), `gen/` (código recompilado: derivado del juego), `build/`,
`run.log` (763 MB), `trace.txt`, `*.bin *.cue *.iso *.img` (discos/BIOS), `*.ppm *.wav *.raw` (capturas/audio),
`*.mcd *.mcr *.srm` (tarjetas), `main_old.c.txt`, `__pycache__/`, y restos de editor: `.Rhistory`, `.~lock.*`.

## Limpieza del emulador (2026-10-07)
Por decisión del usuario se eliminaron del proyecto `ac2launcher/` (lanzador con emulador libretro) y `AC2-PC/` (su
release). Están en la PAPELERA (~/.local/share/Trash/files/), recuperables hasta que se vacíe. Quedan solo
`ac2native/` y `Rom + BIOS/` (disco y BIOS originales; el port nativo NO necesita la BIOS). Consecuencias:
  - El "oráculo Beetle" (comparar contra el emulador con --save-at/--load/--ram-dumps) ya no está disponible
    salvo restaurando ac2launcher de la papelera. Las menciones a él más abajo son históricas.
  - Se conservan sin tocar las partidas del emulador en ~/.local/share/AC2Port (tarjeta .srm de 128 KB y un estado).
  - La ruta del disco en config.ini apunta a la carpeta vieja: ahora está en `Rom + BIOS/Ace Combat 2 (USA)/`.

## Dónde lo dejamos (2026-10-03 madrugada) — LEER PRIMERO
- **Incidente**: /tmp es tmpfs en RAM (5,8 GB). Las capturas de prueba lo llenaron dos veces → se cayó la sesión X
  del usuario (reinicio forzado). Ahora TODO lo voluminoso va a `work/test-out/` (en disco, ignorado por git).
- **Mando Xbox**: funciona bien (confirmado por el usuario). `pad:` en el log con AC2_INPUTLOG=1.
- **Encargo abierto del usuario**: "en la misión 2 el rendimiento caía y no había audio". Tiene estados de
  depuración: slot2 (VBlank 21509, antes de la misión 2) y slot3 (28816, antes de la misión 3), juega a 4x+widescreen.
  Hallazgos hasta ahora:
  - Rendimiento 4x (i3 2C/4T): menús/briefing 3D ~50-55 fps, vuelo ~70 fps, algunos tramos ~52-58. En este PC
    4x no llega a 60 fijo; 2x sí. Posible mejora: más optimización del rasterizador o recomendar 2x.
  - Audio: la música XA se decodifica y mezcla bien (~-17 dB) también en la misión 2. Los EFECTOS (voces SPU)
    tienen huecos largos de silencio (sin key-ons) y casi ninguna voz activa (motor) en lo que parecía la
    misión 2, mientras en la misión 1 había 3-6 voces activas continuas. Sin confirmar porque:
  - La repetición por guion NO era fiable: (a) el mando/teclado vivo se mezclaba con el guion → ARREGLADO:
    con --input se ignora la entrada viva (platform.cpp poll_input); ahora dos pasadas salen idénticas.
    (b) desfase en tools/test/state2script.py (¿frame de pulsación -1 o 0?). PENDIENTE: comparar contra la carga
    real del estado (`--load-state slot3.state --frames 28820 --shot-every 28820`), que debe mostrar el menú
    System/Save con la misión "City on fire"; elegir el desfase que dé imagen idéntica. Después repetir las
    medidas de audio (AC2_PERFLOG=1: fps, XA played/filtered, energía xa/voces, key-ons, voces activas, DMA SPU).
  - Instrumentación nueva (AC2_PERFLOG=1, cada 300 frames): fps, sectores XA, energía de mezcla, key-ons,
    palabras DMA a la SPU, voces activas. Quitar o dejar tras depurar.

## Dónde lo dejamos (2026-10-02)
Fase 1 en curso. Hecho: guardado en tarjeta ARREGLADO y verificado (guardar+cargar), cobertura del
recompilador (+74 funciones), 4 películas verificadas, OPTION/dificultad/cambio de avión, estados de
depuración (F5/F9, menú Esc), prueba larga con películas OK (memoria plana). La prueba larga SIN películas
(1 h de juego, --speed 100) se cortó al parar la sesión: repetirla.
**Icono (2026-10-02, HECHO)**: ac2native no ponía icono (ac2launcher sí, con ac2port.bmp). Ahora
assets/icon/ac2.png + ac2.bmp (64x64 RGBA) y setWindowIcon() (frontend/config.cpp) en las ventanas del
launcher y del juego; verificado _NET_WM_ICON con alfa. `install-desktop-shortcut.sh` crea
~/.local/share/applications/ac2native.desktop (StartupWMClass=ac2native) — instalado en el PC del usuario.
Al empaquetar: copiar assets/icon/ junto al ejecutable (busca icon/, assets/icon/, ../assets/icon/).

**"out of events" (2026-10-02, ARREGLADO)**: cada ciclo intro<->main dejaba ~3 eventos abiertos (main abre 10
y cierra 8; intro deja 1) → con 32 ranuras se agotaba al 9.º ciclo. Ahora Exec (A0:43) libera todos los
eventos del programa reemplazado. Regresión A/B con tarjeta fija vacía: idéntico. Regresiones: usar SIEMPRE
$S/regress.ini (card_dir fija vacía, borrar su contenido antes de cada pasada).

**Controles muertos en vuelo (2026-10-02, RESUELTO)**: el usuario no podía controlar el avión (ni teclado
ni mando; menús sí). Causa: el archivo de partida de 8 KB de ceros (fallo de FD ya corregido); el juego carga
al arrancar sus opciones (KEY CONFIG, unidades HUD) desde la tarjeta y con ceros las acciones de vuelo
quedan sin botón. Reproducido con las entradas del estado del usuario (convertidas a guion --input) y
verificado que con tarjeta vacía responde. Archivo apartado a AceCombat2/BASLUS-00404ACECMBT2.zeros.bak y
borrado su slot1.state (grabado con esa tarjeta). Idea: la BIOS podría validar/rechazar archivos de
tarjeta corruptos, pero un archivo así ya no puede volver a crearse.

**(Histórico, ya arreglado)**: en la prueba larga sin películas aparece `[E] out of events` (9 veces
tras ~10 ciclos intro→main, 110 OpenEvent en 20 Exec): la tabla de eventos de la BIOS HLE (bios.c, evs[],
open_event) se agota porque cada módulo abre eventos en cada Exec y no se liberan. En la consola real el
bucle de demo es infinito, así que algo los libera/reinicia: investigar (¿Exec/ResetEntryInt/_96_init
reinician la tabla? ¿CloseEvent con otro número?) y arreglar; luego repetir la prueba larga.
Prueba larga tras el arreglo: 45 ciclos intro<->main (antes fallaba al 9.º), 0 errores, memoria plana
~142 MB; se cortó a las 08:23 al caerse la sesión X del usuario (no fue el juego).
Siguiente: (1) [hecho] "out of events"; (2) mando Xbox BT: con el mando encendido ejecutar
`AC2_INPUTLOG=1 ./build/ac2native -v` y mirar "pad:" en el log; probar reglas udev de Steam (steam-devices)
para /dev/hidraw*; (3) [hecho, fiel al original] franjas; (4) el usuario juega
misiones y reporta. Scripts de prueba copiados a tools/test/ (mkscript.py, sheet.py, coverage.py, gapptrs.py;
rutas del scratchpad dentro: ajustarlas).

## Compilar y ejecutar
    cd ~/Documents/Ac2Decomp/ac2native
    python3 tools/recomp.py recomp.json gen      # regenera el C (solo si cambia recomp.py o recomp.json)
    cmake -S . -B build -G Ninja && cmake --build build      # ~35 s
    ./build/ac2native [-v|-vv] [--trace] [--frames N] [--shot-every N prefijo] "../Ace Combat 2 (USA)/Ace Combat 2 (USA).bin"

Opciones de depuración:
- `-v` debug, `-vv` traza (llamadas BIOS, escrituras CD/DMA).
- `--trace`: anillo de 1M entradas de funciones; se vuelca a `trace.txt` cuando el juego imprime "time out"/"rror".
- `--shot-every N pref`: capturas PPM del área de display cada N VBlanks.
- `--frames N`: salir tras N VBlanks. `--speed X`: escala de tiempo.
- `AC2_NOPAD=1`: simula mando desconectado.
- El log `[tty]` es la salida de depuración propia del juego (printf por FileWrite fd 1).

Herramientas (`tools/`): `disasm.py` (capstone), `kernel.py` (desensambla el kernel BIOS real
de un volcado de RAM de Beetle, solo para análisis), `recomp.py`.
En `work/`: los 3 ejecutables extraídos (`SLUS_004.04`, `dat/000.bin`, `dat/001.bin`; los regenera `build.sh`),
desensamblados `*.asm` y volcados de RAM de Beetle en `work/ram/` (restos del emulador ya eliminado).

## Estado actual (actualizado)
- Cargador → intro → **FMV del intro (reproductor nativo)** → juego principal → título → briefing →
  mapa → hangar → **primera misión en vuelo** (HUD, cielo, mar) con entrada por guion. Todo nativo.
- La entrada YA funciona por los búferes de InitPad (no hace falta SIO0 para el mando; los 95 accesos
  SIO del juego son seguramente de la tarjeta de memoria).
- Navegar hasta el vuelo: `--speed 10 --input "$(cat nav_native.txt)"` (Cross en el vídeo, luego
  Start/Cross alternos cada 120 VBlanks desde el 1300; el vuelo empieza hacia el VBlank 8500).
  El guion está en el scratchpad de la sesión; regenerarlo: 700:cross + start/cross alternos de 1300 a 9500.
- Reproductor FMV nativo: `runtime/movie.c`, hook en `F_801614E0` (recomp.json "hooks").
  a0 apunta a {longitud|tipo<<28, CdlLOC BCD}. BS v3 validado bit a bit con tools/strdec.py.
- Fallos visuales que se estaban corrigiendo (sesión 2026-10-01 tarde):
  - texto y fondo del hangar como rectángulos morados → corregido: E1 bit 11 (texture disable) solo
    vale si GP1(09h) lo permite. PENDIENTE de verificar en captura.
  - líneas diagonales largas en vuelo → corregidas las polilíneas Gouraud (color por segmento);
    si persisten, sospechar del GTE (proyecciones) o de polígonos degenerados.
  - rasterizador reescrito con gradientes en coma fija (más rápido). Verificar que no cambia la imagen.
- **Resuelto: corrupción de VRAM en la misión** (texto/hangar morados, líneas diagonales, HUD roto).
  Causa: el analizador de polilíneas metía el primer vértice por la rama de "siguiente vértice" y en
  ciertas secuencias perdía la sincronía: vértices ejecutados como comandos (0x02 relleno, 0x80 copia).
  Reescrito como máquina de estados (poly_line 2 = primer vértice, 1 = siguientes, color previo en
  Gouraud). Hallado con `AC2_WATCH="x0,y0,x1,y1"` (vigía de escrituras en VRAM).
  Ahora mapa de misión, hangar, carga, HUD de vuelo, pausa y subtítulos se ven como en el hardware.
- **Audio verificado** (grabación `--wav` + `tools/wavstats.py`, comparado con Beetle vía `AC2_WAV` en
  ac2launcher): música continua -20..-23 dBFS en título/briefing/vuelo, igual que Beetle. Arreglo clave:
  el CD conserva un sector reconocido-no-pedido solo un periodo de sector y luego lo descarta (las
  pistas XA intercalan sectores de datos vacíos que nadie lee; antes el lector se paraba).
- **Tarjeta de memoria**: detectada ("No file on this Memory card" en LOAD). Guardado real sin probar
  (requiere completar una misión). Archivos en ~/.local/share/ac2native/card0/.
- **Launcher nativo integrado** (frontend/: config, ui, launcher; ImGui). Config en
  ~/.local/share/AC2Native/AceCombat2/config.ini. `main.cpp`: con disco en la línea de comandos salta
  el launcher (pruebas). Menú de pausa en juego (Esc/Guide): filtro, aspecto, volumen, pantalla
  completa, volver al launcher (re-exec), salir. Los savestates NO son posibles (estado = pila nativa).
- **Resolución interna 2x/4x** (`--scale N` o launcher): VRAM almacenada a S×; polígonos a S×; texturas,
  CLUT, sprites, líneas, rellenos y transferencias en bloques S×S (gpu.c, macros VR/VN).
  Verificado hasta el vuelo a 2x y 4x (+widescreen). 4x se cerraba: búfer `pixels` de 1024 filas (arreglado).
- **Rendimiento GPU (2026-10-01 noche)**, medido con 7800 frames sin limitador, i3-7020U 2C/4T:
  1x 55→29 s, 2x 156→45 s, 4x ≈120 s (≈65 fps de media; vuelo >70). Salida idéntica bit a bit
  (comparado con capturas PPM de referencia en 1x/2x/4x). Cambios:
  - triangle: span exacto por fila (span_edge) en vez de probar 3 aristas por píxel; bucle `span`
    especializado por tipo (texturizado/gouraud/raw/profundidad) con estado en locales.
  - rect: fila nativa una vez y replicada S veces; ruta rápida vectorizable para rects lisos (los
    borrados de pantalla 320x480 y las capas semitransparentes del menú eran ~1/3 del tiempo).
  - `vnat`: espejo de la VRAM a resolución nativa (muestra superior izquierda de cada bloque) para
    leer texturas/CLUT sin fallos de caché a 4x. TODA escritura en una muestra superior izquierda debe
    actualizarlo (span, plot, rect, fill, copy, native_block).
  - Hilos de render (S>1; `AC2_THREADS=n` para forzar, 0 = sin hilos): cada comando de dibujo es un Job
    con copia del estado (DS); cada hilo ejecuta todos los jobs en orden pero solo sus franjas de 8
    filas escaladas → resultado idéntico. Riesgos controlados con rejillas de celdas 16x16 (pend_w /
    pend_r): leer textura de algo pendiente de dibujar, o dibujar sobre algo que un job pendiente aún
    va a leer → gpu_sync; un job que lee su propia zona de dibujo se ejecuta entero en el hilo principal.
    Transferencias, copias VRAM y la captura (gpu_vram) sincronizan. `AC2_SYNCSTAT=1` muestra motivos.
    Una unión de cajas como zona pendiente NO sirve (las CLUT/páginas cubren toda la VRAM → sync continuo).
  - Presentación: la VRAM se sube tal cual como textura GL_RGB5_A1 con GL_BGRA+1_5_5_5_REV (formato
    nativo de la GPU; R/B se intercambian en el shader). Con GL_RGBA Mesa convertía en CPU (lento).
    Las capturas `--shot-every` siguen la ruta antigua (LUT a RGBA8).
  - Esperas: gpu_sync duerme (nanosleep 20 µs) tras un spin corto; hacer spin robaba CPU al hilo
    hermano (HyperThreading) y costaba un 20%.
- **Widescreen** (`--widescreen` o launcher): GTE RTPS escala la X proyectada por 3/4 (g_gte_widescreen).
- **Distribución (importante, legal):** gen/ y el binario contienen código derivado del juego. Lo limpio
  es distribuir tools+runtime+frontend y un script que, con el disco del usuario, extraiga (work/),
  recompile (gen/) y compile: HECHO (`tools/extract.py` + `build.sh`, verificados). `.gitignore` creado
  (excluye work/, gen/, build/, imágenes de disco, BIOS, capturas).
- `--no-movies` + `nav_nomovie.txt` (en ac2native/) = navegación determinista hasta el vuelo (~VBlank 7000).
- Opciones nuevas: `--input`, `--wav fichero` (graba el audio para analizarlo), `AC2_VRAM=1`
  (vuelca la VRAM con cada captura), `AC2_DUMP_MDEC=dir`, `AC2_NOHOOK=1`.
- **Jugabilidad verificada (2026-10-01 noche)**: en la misión 1 con guion se disparan misiles (Circle),
  hay explosiones, choque con el terreno → "MISSION FAILED" → repetición con cámara externa. Todo nativo.
  Intentos para completarla: misiles cada 40 frames desde el 7000 (se estrella contra una montaña hacia
  el 10500); con "down" cada 50 frames sube demasiado y acaba en el mar (~frame 8000). Siguiente intento:
  tirones más suaves (down:6 cada 120) o apuntar a los TGT. Scripts generadores en el scratchpad
  (mkscript.py: `boton:inicio:fin:paso:duracion`; sheet.py: hoja de capturas); recrearlos si se pierden.
- **2026-10-01 (tarde-noche): el usuario jugó una misión completa** "con mucha comodidad y rendimiento".
  El guardado escribió `~/.local/share/AC2Native/AceCombat2/card0/BASLUS-00404ACECMBT2` (8 KB, 1 bloque).
  OJO: la ruta real de tarjetas es la de AC2Native/AceCombat2 (la antigua ~/.local/share/ac2native/ está
  vacía y en desuso). Cargar partida: SIN probar todavía.
- **ARREGLADO: el guardado en tarjeta escribía ceros** (2026-10-01 noche). La BIOS HLE daba a los archivos
  de tarjeta los descriptores 0.. y FileWrite trata 0/1 como la TTY → los datos de la partida iban al log.
  Ahora empiezan en 2 (FD_BASE) como en la BIOS real; añadido FileSeek (B0:33). Verificado con guion:
  misión fallida → menú System → Save → ranura 1 ("0:01'08''") → título → LOAD → CONTINUE/NEW GAME.
  Guion de referencia: mkscript.py circle:7000:10400:40:6 start:10900:11201:150:6 cross:11700:11701:1:6
  right:12000..12200 (x3) cross:12300 cross:12700 start:12900. La partida que guardó el usuario antes del
  arreglo es inservible (8 KB de ceros).
- **Cobertura del recompilador** (2026-10-01 noche): análisis estático (huecos de .text sin función que
  parecen código: prólogos / jr ra) → 73 candidatas en el main, casi todas funciones de librería PsyQ sin
  usar; ningún puntero a ellas ni en los ejecutables ni en los 226 archivos de ACE2.DAT. Aun así se añaden
  como `weak_seeds` (nueva clave de recomp.json; se descartan solas si el análisis ve que no son funciones):
  1814 → 1888 funciones. Regresión A/B (código viejo vs nuevo, misma tarjeta): idéntico bit a bit.
  OJO regresiones: la tarjeta real (prefDir/card0) cambia el arranque; usar siempre la misma tarjeta.
- **Películas** (2026-10-01 noche): ACE2.STH entradas 65-68 (tipo 8): intro 76 s, final malo 26 s (ataque
  a la ciudad), dos finales con créditos (130 s, 145 s). `--play-movie N` (0-3) las reproduce directamente:
  las cuatro decodifican bien (~30 fps). El main NO tiene reproductor (no toca el MDEC): hace Exec del
  módulo intro (F_80063xxx → stub Exec 8009d5ec) y el intro elige la película por un índice (s0 → tabla en
  sp+0x30 → 80161f48) y llama SIEMPRE a 801614E0 = nuestro hook. Los finales usan pues el reproductor nativo;
  falta confirmarlo llegando al final de verdad.
- **Fase 1, verificado con guiones (2026-10-02)**: menú OPTION (KEY CONFIG cambia NOVICE/EXPERT, GAME CONFIG
  cambia opciones, ADJUST SCREEN muestra la carta), dificultad HARD, cambio de avión en el hangar (F-4, 64
  misiles) y vuelo con enemigos ("ENGAGE!", fijación de blanco). Prueba larga con películas: 8 ciclos
  completos intro→main (Exec repetido) sin errores, memoria plana ~138 MB.
  Franjas azul/blanco en la transición hangar→carga: RESUELTO, son FIELES. Beetle (ac2launcher, estado guardado
  en el frame 7480 con --save-at/--load y capturas cada 4 frames) muestra las mismas bandas sobre el hangar
  antes de la pantalla "CODE NAME". `AC2_GP1LOG=1` registra GP1 03/05 (útil para casos así).
- **Estados de depuración (2026-10-01 noche)**: menú de pausa (Esc) → "Debug states", 3 ranuras; F5 guarda
  la 1, F9 carga la 1. Archivos en ~/.local/share/AC2Native/AceCombat2/states/slotN.state.
  Funcionan por repetición: la entrada que ve el juego se fija una vez por VBlank (platform_latch_input,
  llamado en core antes de bios_vblank; la BIOS usa platform_game_buttons) y se graba como cambios; el
  archivo guarda VBlank, widescreen (afecta al GTE), copia de la tarjeta al arrancar y los cambios.
  Cargar = re-exec con `--load-state`; se repite sin pausa, sin dibujo (g_gpu_skip_draw, se reactiva 30
  frames antes), sin audio y saltando películas, contra una copia temporal de la tarjeta; luego vuelve a la
  tarjeta real y core_pace_reset(). Verificado: imagen idéntica bit a bit en el frame 7300 tras cargar
  un estado del 7000; carga ~5 s por cada 2 min de juego. `AC2_SAVE_AT=frame:slot` guarda solo (tests).
  El juego lee VRAM (0xC0) a veces: con el dibujo saltado esos datos difieren; no ha afectado, vigilar.
- **Mando Xbox Series por Bluetooth (045E:0B13, hid_microsoft)**: el usuario notó controles raros con él
  conectado (ni teclado funcionaba para misiles/select). Hipótesis: entrada leída como pulsada en reposo.
  Añadido: entradas "pulsadas al conectar" se ignoran hasta soltarse + `AC2_INPUTLOG=1` (con -v) registra
  mapeo y cambios. PENDIENTE de diagnosticar con el mando encendido (el usuario lo aplazó).
- **Decisión (2026-10-07): sin extracción de assets ni VFS.** El port lee el disco en cada arranque; del disco solo se
  extrae lo mínimo para compilar. Fase 2 = limpieza + gestión del disco + instalador + paquete fuente + Windows + docs.
  El disco del usuario se movió a `Rom + BIOS/Ace Combat 2 (USA)/`: la ruta guardada en config.ini está rota.
- Pendiente (detalle en PLAN.md, Fases 1-5): cargar partida, resto de misiones, probar mando real, limpiar código
  de depuración, empaquetado (AppImage de las herramientas + build.sh).

## Hallazgos importantes (no obvios)
- PsyQ `ResetCallback` usa setjmp + `SetCustomExitFromException`: el recompilador genera puntos de
  reanudación `R_xxxxxxxx` tras cada `jal setjmp`; `ReturnFromException` hace longjmp nativo.
- `EnterCriticalSection/ExitCriticalSection` deben tocar IEc (bit 0) e IM2 directamente (no IEp).
- `VSync` y `libcd` usan timeouts por número de iteraciones → hace falta el reloj virtual (CYC por
  bloque, 2 ciclos/instrucción) sincronizado con el reloj real en cada VBlank.
- `libcd CD_init` asume que la BIOS dejó habilitadas las IRQ del CD (enable=0x1F al arrancar).
- `_patch_pad`: el juego lee B0:5B de la tabla B0 y usa desplazamientos del kernel real:
  +0x884 activa y +0x894 desactiva la lectura de mandos en VBlank; +0x7A0 registra búferes
  secundarios de 7 bytes que el kernel NO rellena (los datos van a los búferes de InitPad, 0x22 bytes).
  Rellenar los de 7 bytes hacía que los de verdad quedaran a 0 = todos los botones pulsados.
- El CD real sobrescribe un sector no pedido tras reconocer su INT1; solo espera mientras INT1 está
  pendiente (cambio hecho en `sector_event`; no resolvió por sí solo el bloqueo del FMV).
- Tablas BIOS en RAM (A0 0x200, C0 0x674, B0 0x874) apuntan a "thunks" 0x80001000+ que el runtime
  convierte en llamadas BIOS nativas.
- SDL captura SIGTERM: se desactiva con `SDL_HINT_NO_SIGNAL_HANDLERS` para que `timeout` funcione.

## Código de depuración temporal a limpiar al final
`AC2_NOPAD`, logs `cd sector`, `mdec cmd`, `cd write`, `dma%d`, `dicr write`, `cd_debug()`,
`irq state` (en -v). Útiles mientras se depura.
