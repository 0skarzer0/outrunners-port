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
