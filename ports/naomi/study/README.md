# Herramientas del estudio de OutRunners (en PC)

Estudio y medidas: [../docs/study.md](../docs/study.md). **Para continuar el trabajo: [../HANDOFF.md](../HANDOFF.md).**

| Fichero | Para qué |
|---|---|
| `census.patch` | Censo de vídeo para `runtime/src/video.c` de [cheviman/outrunners-port](https://github.com/cheviman/outrunners-port) (commit `12af5afe`; este fork ya lo incluye en la rama `naomi`, así que el parche se aplica sobre el original): con `ORUNNERS_CENSUS=fichero`, una línea por fotograma con los registros de vídeo y mezclador, los sprites y los bancos de paleta visibles |
| `race_arcade.txt` | Guion de controles para `orunners_game_headless`: moneda, START, gas y volante; llega a la carrera hacia el fotograma 1700 |
| `analyze_census.py` | Resume el censo |

Receta:

```sh
git clone https://github.com/cheviman/outrunners-port && cd outrunners-port
git submodule update --init --depth 1
git apply /ruta/fork/ports/naomi/study/census.patch
mkdir rom && cp /ruta/orunners.zip rom/
cmake -B build -DCMAKE_BUILD_TYPE=Release -DORUNNERS_ROM=$PWD/rom/orunners.zip
make -C build -j2 orunners_game_headless orunners_assets
ORUNNERS_CENSUS=/tmp/census.txt build/bin/orunners_game_headless --script /ruta/fork/ports/naomi/study/race_arcade.txt \
    --hashes /dev/null --video-hashes /dev/null --screens 1
python3 /ruta/fork/ports/naomi/study/analyze_census.py /tmp/census.txt 1700
```

Coste por partes (lo de la tabla 2.1): dos ejecuciones con `valgrind --tool=callgrind` cuyo guion termina en los
fotogramas 2900 y 3500 (línea `N end`), y restar la columna exclusiva de `callgrind_annotate`.
