# OutRunners en NAOMI — traspaso para otra sesión

Última actualización: 2026-11-13. Estudio y medidas: [docs/study.md](docs/study.md). Detalle técnico del port:
[README.md](README.md). Índice del proyecto (los demás juegos del mueble):
[naomi-wheel-games](https://github.com/0skarzer0/naomi-wheel-games). Este traspaso, el estudio y sus herramientas
estaban hasta el 2026-11-13 en `outrunners/` y `docs/outrunners.md` de naomi-wheel-games; se movieron aquí para que
todo OutRunners esté en su fork (como cada proyecto en su repo).

## Dónde está cada cosa

| Qué | Dónde |
|---|---|
| Código del port | [0skarzer0/outrunners-port, rama `naomi`](https://github.com/0skarzer0/outrunners-port/tree/naomi) (fork de [cheviman/outrunners-port](https://github.com/cheviman/outrunners-port): los commits del port sobre `12af5afe`; `git submodule update --init --depth 1`). El port, en `ports/naomi/` (su README) |
| Toolchain | [`ports/naomi/build_toolchain.sh`](build_toolchain.sh): sh-elf GCC 15.2.0 + KOS `368a2eb9` en `/opt/toolchains/dc/kos` (Dreamcast) y `/opt/toolchains/dc/kos-naomi` (`KOS_SUBARCH=naomi`, con `naomibintool`). ~35 min con 2 núcleos |
| Estudio de vídeo en PC | [`ports/naomi/study/`](study/): `census.patch`, `race_arcade.txt`, `analyze_census.py` (su README) |
| Herramientas de la Pi | las de [naomi-kit](https://github.com/0skarzer0/naomi-kit) (`tools/pi/`, en el submódulo `ports/naomi/naomi-kit`): `pi_env.sh`, `dimm_wait.sh`, `send.sh`, `install.sh`, `info.py`, `live.py`, `watch.py`, `profile.py`, `drive.py`. Las versiones anteriores propias de OutRunners (`send_and_log.sh`, `install_pi_tools.sh`, `or_info.py`) ya no se usan: siguen en la historia de naomi-wheel-games (`outrunners/pi/`, antes del 2026-11-13) |
| ROM | `orunners.zip` (set de MAME) del usuario: **pedírselo**; nunca subir nada derivado de él |

## Cómo reconstruir el entorno desde cero (workspace de Claude)

```sh
git clone -b naomi https://github.com/0skarzer0/outrunners-port && cd outrunners-port
bash ports/naomi/build_toolchain.sh                          # una vez, ~35 min
git submodule update --init --depth 1
ports/naomi/build_naomi.sh /ruta/orunners.zip build/naomi     # imagen: build/naomi/orunners_naomi.bin.gz
```

Para recompilar solo el programa: `source /opt/toolchains/dc/kos-naomi/environ.sh; make -C ports/naomi clean all`.
**Haz siempre `clean`**: el Makefile no sigue las dependencias de las cabeceras, y un `.o` viejo con otra versión de
`naomi.h` mueve el bloque de datos en vivo. Después, `naomibintool` + `naomi_pack.py` + `gzip`, como al final de
`build_naomi.sh`. Pruebas por etapas: `make EXTRA=-DORUNNERS_NAOMI_STAGE=N` (0: KOS y peeks; 1: assets leídos;
2: todo listo, espera un poke en `naomi_stop_at`).

## Cómo probar en el mueble (Crazy Taxi, NAOMI 2)

- La placa está en la Pi `192.168.68.60` (WiPi), usuario `root`; **la contraseña se le pide al usuario**. La NetDIMM se
  ve desde la Pi como `10.0.0.4`.
- Desde el workspace no se llega a su red: se entra con `device_bash` en su ordenador, que necesita una carpeta
  conectada (la usada fue `C:\Users\Usuario\Documents\dc2naomi`; en `outrunners\` están las imágenes que se envían).
  Para pasar la imagen del workspace a su ordenador: `device_commit_files` desde `/mnt/user-data/outputs/`.
  **Cada vez con un nombre de fichero nuevo**: con el mismo nombre puede llegar una versión anterior. Comprobar el
  md5 en su ordenador.
- Herramientas de naomi-kit (`ports/naomi/naomi-kit/tools/pi/`): `export PI_PASS=...` (pedírsela al usuario) y
  `install.sh` (una vez: las copia a `/sbin/piforce/naomi-kit/`). Después, `send.sh imagen.bin.gz
  h-outrunners_naomi 0x8c...` copia, envía (`dc2naomi_send.py`), espera y muestra la consola. La dirección del
  bloque en vivo sale de `sh-elf-nm orunners_naomi.elf | grep live` (magic `NKLV`).
- En la Pi: `NK_DIMM=10.0.0.4 python3 /sbin/piforce/naomi-kit/live.py ADDR fps 10` (`... draw N`, `... log`),
  `watch.py`, `profile.py` (pcs muestreados, para `sh-elf-addr2line -f`) y `drive.py` (moneda, START, gas y
  volante). Los `outrunners_live.py`, `or_watch.py`, `or_profile.py` y `or_race.py` antiguos de `/sbin/piforce` **no
  sirven** con imágenes sobre el kit (el bloque en vivo cambió).
- El menú del WiPi ya tiene la fila "OutRunners (prueba)" para `h-outrunners_naomi.bin.gz`.

**Trampas** (cada una costó un buen rato):
1. Un peek sin respuesta deja la NetDIMM ocupada hasta ~1 min y los envíos siguientes fallan (`send rc=124`).
   Antes de enviar, `dimm_wait.sh`. No matar peeks a medias.
2. `pkill -f PATRÓN` por ssh con un patrón que también aparece en la propia línea de órdenes mata la shell remota
   (`exit 255`).
3. Mientras el programa lee los assets (~7,5 s), los peeks no se atienden: el bucle de lectura retoma el candado
   de la NetDIMM antes de que el hilo de los peeks lo consiga. Leer la consola a partir de ~25 s del envío.
4. Una excepción no tratada queda en la consola y el programa sigue respondiendo peeks (versión del bloque en vivo
   `0xdeadXXXX`). Si no responde nada, el programa no llegó a `naomi_dimmcomms_start` o la CPU está colgada.
5. `timehack.py` del WiPi corre en la Pi; no molestó.

## Estado (2026-11-13, 16:10)

**Imagen más reciente: `or_155606.bin.gz`** (md5 `424e39a749a84bcabc6433543f3a46c5`, bloque en vivo
`0x8c0c7604`), **sin probar en la placa**: el enlace con el ordenador del usuario (app de escritorio) se cayó a las
14:30 y no volvió en esta sesión. Lleva todo lo de abajo. Lo primero en la próxima sesión con enlace: enviarla,
`outrunners_live.py 10.0.0.4 0x8c0c7604 fps 60` (columna `aica writes`: si pone `NO SAMPLES`, no encontró
`/dimm/orunners.pcm`) y preguntar al usuario si suena.

Probado en la placa (imagen `or_131xxx`/`or_14xxxx`): la imagen por la PowerVR2 a **60 fps** en attract y casi toda
la carrera (juego ~8 ms, sonido ~1 ms, escena ~4–5 ms, envío ~1,2 ms; "se ve estupendamente"), con bajones en
tramos de paleta animada.

Hecho después, comprobado solo en PC:
- **Bajones**: tablas de color por celda y bitmap redibujado solo en las filas visibles.
- **Sombras** (el *shadow pen* de los sprites: las sombras de las palmeras en la carretera): una segunda textura
  por sprite con esos texels, dibujada negra al 50 % en la lista translúcida.
- **Blending de TEXT sobre sprites** (lo único que usa OutRunners: capa de texto, peso 5/8): por celdas de 8x8
  sobre los rectángulos de los sprites. En el guion de carrera no aparece; sin comprobar con datos.
- **Fundidos exactos**: si todas las capas llevan el mismo offset negativo, la escena lo resta de toda la imagen
  (en la PVR, tres quads translucidos a pantalla completa: 1-d, +k con saturación, 1-d).
- Con todo eso, píxeles distintos de `video.c` en el guion de carrera: **del 7,6 % al 0,68 %**.
- **Fase 3, sonido**: las 28 voces del MultiPCM en canales del AICA (abajo).
- **START hace también de moneda** (al pulsar mete una moneda, al soltar pulsa START).
- **Fase 4, el conversor para Windows** *OutRunners NAOMI* (`ports/naomi/maker/`, objetivo `orunners_naomi_maker`,
  compilado con MinGW): `orunners.zip` + `orunners_naomi-prog.bin` (el programa de naomibintool, sin nada de la ROM)
  → `orunners_naomi.bin.gz`. Mismo programa y samples que `build_naomi.sh`; el pack solo difiere en los bloques de
  sprites (el de los makers omite las tiradas de 0xFFFFFFFF, que se leen igual). Se le dio al usuario un zip con el
  .exe, el programa de la imagen `or_155606` y un LEEME (no va en el repo: el programa se rehace con cada cambio).

Arreglos de la fase 2 que conviene recordar:
- **Accesos desalineados:** el código del V60 lee palabras en direcciones impares (scenery.c); el SH-4 da error de
  dirección. `naomi_log.c` los emula byte a byte, también en el hueco de retardo de un salto (contador
  `misaligned` en el bloque en vivo, +0x78; ~100 en una carrera).
- **No `vid_waitvbl`** con la PVR: `pvr_wait_ready` ya sincroniza con el vblank; con los dos iba a 30 fps.
- **`memcmp` de newlib es byte a byte** en SH-4: `same()` compara por palabras (de 9 a 4 ms la escena).
- Quads ordenados por textura y enviados con *direct rendering*: el envío de ~500 quads, de 3,5 a 1,2 ms.

## Prueba en placa sobre naomi-kit (2026-11-13, 21:35)

Imagen `or_213421.bin.gz` (md5 `53940196ce7cdb338ca2e5d04ee0c33c`, rama `naomi` con el kit, bloque live en
`0x8c0ca868`), enviada desde el PC del usuario a la Pi (`/boot/roms/h-outrunners_naomi.bin.gz`; la anterior guardada en
`/root/h-outrunners_naomi.prev.bin.gz`) y con `dc2naomi_send.py 10.0.0.4`. Lector: `live.py` de naomi-pi-tools en
`/sbin/piforce/naomi-kit/` (con `nk_live.py` y `drive.py` del kit).

- **Arranca y juega sobre el kit**: `live.py ADDR words` lee el bloque `NKLV` con los nombres de los campos; consola
  por `log`; 4 MB de muestras al AICA en 1952 ms; `sound_writes` crece (el AICA recibe registros).
- **Mandos por `nk_cabinet_read`**: volante, gas, freno y botones se mueven en `inputs` mientras el usuario juega.
- **fps**: 57-60 casi siempre; segundos sueltos a 28-49 (peor fotograma 70-130 ms) cuando `build_us` sube a 10-22 ms.
- **Memoria de vídeo**: baja de 6 MB a ~0.2-0.4 MB en minuto y medio y se queda ahí, con `missing 0` (la caché
  recicla). El campo `textures` da valores enormes (41 millones): revisar qué cuenta.
- `misaligned` 101 (resueltos por el kit, sin cuelgue).
- Falta lo que solo ve quien está delante: sonido, sombras, fundidos, START = moneda.

## Texturas con paleta y menos CPU (2026-11-13 noche y 2026-11-17)

- **Sprites de 4 bits como texturas con paleta** (`PAL4BPP`, gemelas en orden twiddled): la textura guarda solo los
  pens (0 = no se dibuja: pen 0, fin de fila y códigos de fin) y el quad lleva uno de los 64 bancos de 16 colores
  de la RAM de paleta de la PowerVR2, que se escriben tras `pvr_wait_ready` (el render anterior ya no la lee). Un
  cambio de color no redibuja nada y ocupan la cuarta parte. Si no quedan bancos, o el sprite es de 8 bits, o su
  tabla indirecta pinta el pen 0, va por el camino ARGB1555 de antes. La sombra usa la misma textura con una paleta
  negra al 50 %. En el headless (`ORUNNERS_SCENE_NO_PENS=1` para comparar sin ellas): mismo 0,675 %.
  Probado una vez en placa (`or_224236`): arranca y juega; falta que el usuario confirme los colores.
- **Medido en placa** (render_us, pt/tr quads y área en el bloque live; profiler del kit con pr y sin las muestras
  en espera): los tramos a 30 fps que quedaban no son de la PowerVR2 (pocos quads, 2-3 pantallas de área) sino de
  la CPU al límite: juego 8,4 + sonido 1,1 + escena 5-6 + envío 1,6 ms ≈ 16,7. En el perfil: `same` (comparaciones
  del bitmap, la paleta y el texto) 14 %, el Z80 6 %, `find`/`ram_outside` (copias de la ROM byte a byte) ~12 %,
  `image_write` 5 %.
- Arreglos (sin probar en placa aún, imagen `or_210303.bin.gz`, md5 `337dc13ede2ec323584298d922280e7a`):
  `ram_outside` copia por tramos (`data_span`) en vez de buscar cada byte; `same` pide las líneas de caché por
  adelantado; `image_write` y los pens por **store queues** (ráfagas de 32 bytes); las áreas de las estadísticas
  solo cada 30 fotogramas. Headless: estado del juego idéntico en 9001 fotogramas, escena igual (0,675 %).
- **Lo que queda: los picos al cambiar los colores del fondo.** En el headless son las páginas de tilemap
  (33 000 texeles por fotograma en los peores tramos), no los sprites. El port de Dreamcast de
  [MisterPezz82](https://github.com/MisterPezz82/OutRunners-Dreamcast) (0.4, BSD-3, sobre este mismo upstream)
  lo resolvió con un **almacén de tiles de 4 bits en memoria de vídeo y un quad por celda con paleta**
  (`docs/dreamcast/REDESIGN.md`, `ports/dreamcast/pvr/`), y además aceleró el Z80 (`board_time` sin división
  de 64 bits) y solo mira las celdas que cambian. Es el camino a seguir aquí (la NAOMI tiene el doble de memoria).

## Auditoría en placa: lentitud y coche que desaparece (2026-11-13, 22:00-22:30)

Medido con `live.py ADDR watch 150` mientras el usuario jugaba (trazas en la Pi: `/tmp/or_trace*.txt`).

1. **El coche desaparecía**: `missing` = `0x200` (S32_SCENE_MISSING_IMAGES) fijo tras unos minutos. Las 16 páginas de
   tilemap (512 x 256, 256 KB cada una, 4 MB) nunca se soltaban aunque no se dibujaran, y dejaban sin sitio a los
   sprites. Arreglo (`runtime/src/video_scene.c`, `image_with_room`): si no hay sprite viejo (más de 8 fotogramas sin
   usar), se suelta antes una página no usada en este fotograma. Además la tabla de sprites (2048 entradas) se llenaba
   y cada búsqueda la recorría entera: ahora cada sprite tiene una ventana de 16 entradas y se reutiliza la usada hace
   más tiempo; y la búsqueda de víctima es una «manecilla de reloj», no un recorrido de las 2048 por imagen.
   Comprobado en el headless (`ORUNNERS_SCENE_TEXELS=2000000`, presupuesto como en la placa): de 276 de 300 muestras con
   `missing 0200` y 55 % de píxeles distintos a 0 y 0,675 % (lo mismo que sin presión). En placa: `missing 0` siempre.
2. **Lento (30 fps)**: `build_us` 20-35 ms con `texels_drawn` ~2 millones por segundo, y `failed` subiendo con 2-3 MB
   libres: la memoria de vídeo de KallistiOS (`pvr_mem`, un malloc general) se fragmentaba. Arreglo
   (`ports/naomi/pvr_video.c`): un allocator buddy sobre arenas de 1 MB (12 en la NAOMI: 14 MB libres al arrancar);
   las listas libres van dentro de los bloques libres en la propia memoria de vídeo. Probado en el PC con 2 millones de
   operaciones al azar: al liberarlo todo vuelven las arenas enteras. Handles de imagen 2048 -> 4096.
   En placa: en carrera casi siempre 51-60 fps.
3. **Lo que queda** (imagen `or_221736.bin.gz`, md5 `471c8c6e8530a6ff3478abb5baa8d20d`):
   - Picos de 2,5 millones de texeles por segundo (22-30 fps unos segundos) sin falta de memoria: sprites que se
     redibujan porque su paleta cambia cada fotograma (`sprite_version` sube). Arreglo de fondo: **texturas con paleta**
     de la PowerVR2 (4 bpp: 64 bancos de 16 colores en su RAM de paleta; 8 bpp: 4 de 256), que cambian de color sin
     redibujarse; un banco por fotograma para los que se usen, y ARGB1555 de reserva si no caben.
   - En algunas pantallas fuera de carrera, `wait_us` ~16 ms con `build_us` 5 ms: la PowerVR2 tarda más de un
     fotograma en pintar (mucha superposición translúcida: fundidos y texto). Mirar qué capas hay ahí.
   - El campo `textures` del bloque es `imágenes | fallos << 16`, no un contador de texturas (por eso sale enorme).

## Fase 2 — render PowerVR2

Código: `runtime/src/video_scene.c` (escena de quads, portable, incluida por `video.c` con `ORUNNERS_VIDEO_SCENE`),
`runtime/src/video_scene_soft.c` (rasterizador de PC para comparar) y `ports/naomi/pvr_video.c` (texturas ARGB1555
no twiddled en VRAM; lista punch-through para lo opaco, translúcida para sombras, blending y fundidos;
profundidad = prioridad del mezclador). Los quads llevan `weight` (8 opaco, <8 mezcla) y el fotograma `subtract`.

Comparar con `video.c` en el PC: ver "Checked on a computer" en `ports/naomi/README.md`. La línea de cada 30
fotogramas da píxeles distintos, quads (con mezcla, sombras) y `subtract`; `ORUNNERS_SCENE_DUMP=N` y
`ORUNNERS_SCENE_QUADS=N` para un fotograma concreto. `diffimg.py`-style: original | escena | diferencias.

Lo que queda (pequeño): sombras solapadas oscurecen dos veces (la placa, una); en un fundido la sombra se aplica
antes de restar (la placa, después); sprites "sombra" (flag 11, OutRunners no los usa) no se dibujan; ventanas de
tilemaps y recorte "outside" de la lista de sprites (no vistos en OutRunners).

## Fase 3 — sonido en el AICA

- `runtime/src/multipcm_aica.c`: de los registros de cada voz del MultiPCM a los del canal del AICA (misma
  familia Yamaha: samples de 8 bits con signo, envolvente AR/D1R/DL/D2R/RR/KRS con tasas al doble de bits, nivel
  0,375 dB por paso, pan 3 dB, profundidades de LFO iguales). El tono se recalcula para 44100 Hz (error máximo
  1,7 cents), la escala de tasa de tecla se corrige por la nueva octava, los deslizamientos de nivel van una vez
  por fotograma, y los bancos (`s32_board.pcm_bank`) se aplican al dar la nota.
- `sound_chips.c`: `sound_chips_pcm_play_on(player)`: con un *player* el MultiPCM no genera samples y se le avisa de
  cada registro escrito y de cada fin de fotograma.
- `ports/naomi/aica_sound.c`: carga `/dimm/orunners.pcm` (4 MB, lo hace `naomi_assets.py`; `build_naomi.sh` ya lo
  mete en `dimm/`) en la RAM del AICA a 1 MB con *store queues*, volumen maestro 15, y escribe los registros por el
  bus G2. El brazo ARM del AICA se queda en el bucle de KallistiOS (no hace falta).
- **Lados:** las dos salidas del MultiPCM son los dos asientos del mueble doble (la mezcla de referencia pone la
  segunda en el altavoz izquierdo, y en carrera solo suena ese). Se toca la salida 1 (asiento izquierdo = pantalla
  izquierda, la que se ve) en mono por los dos altavoces.
- Comprobado en PC con `ports/headless/aica_model.c` (AICA aproximado en software, `ORUNNERS_AICA_WAV`): contra el
  sonido del MultiPCM del guion de carrera, la sonoridad por fotograma correlaciona 0,89, los espectros 0,98–0,997, y
  el nivel es el mismo (DISDL 0xC).
- Si no suena en la placa: mirar `aica writes` en `live.py` (crece = la placa escribe registros), el volumen maestro
  (registro 0x2800), y si la RAM del AICA tiene los samples (`spu_memread`). Si suena raro: comparar a oído con MAME.

Herramientas en la Pi: `drive.py` del kit (antes `race.py`, copiado como `or_race.py`) echa moneda, pulsa START,
acelera y gira; imprime fps y tiempos. `live.py` del kit muestra `misaligned` y `aica writes`.


## Aviso: port sobre naomi-kit (2026-11-13)

El port ya usa naomi-kit (submódulo `ports/naomi/naomi-kit`): arranque, controles, ficheros de la NetDIMM, bloque
live (magic `NKLV`, offsets nuevos) y herramientas de la Pi vienen del kit (ver «Cómo probar en el mueble»). Nada de
esto está probado en la placa todavía (rama `naomi` del fork, commit 9e3d282). La imagen `or_155606` de «Estado» es
anterior al kit: su bloque en vivo es el viejo y se lee con el `outrunners_live.py` antiguo que sigue en la Pi.
