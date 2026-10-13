# shadPS4 para inFAMOUS Second Son (fork no oficial)

Versión modificada del emulador de PS4 **[shadPS4](https://github.com/shadps4-emu/shadPS4)**, ajustada para jugar **inFAMOUS Second Son** (CUSA00004, actualización 1.07) en PC con Windows.

> [!IMPORTANT]
> - **El juego NO viene incluido.** Este proyecto no contiene el juego ni la actualización, y no da enlaces para conseguirlos. Necesitas tu propia copia, volcado de tu disco o de tu consola, más la actualización 1.07.
> - **Es un proyecto no oficial.** No está afiliado al equipo de shadPS4, a Sony Interactive Entertainment ni a Sucker Punch Productions. Las marcas pertenecen a sus dueños.
> - **El juego NO ha sido probado al 100 %.** Se probó sobre todo el inicio del juego y las primeras zonas, incluida la escena del fuego. Puede haber fallos más adelante que todavía no conocemos (ver [Informes de fallo](#informes-de-fallo)).

---

## Índice
1. [Qué incluye](#qué-incluye)
2. [Requisitos y PC de pruebas](#requisitos-y-pc-de-pruebas)
3. [Instalación y uso](#instalación-y-uso)
4. [Ajustes](#ajustes)
5. [Informes de fallo](#informes-de-fallo)
6. [Cambios respecto a shadPS4](#cambios-respecto-a-shadps4)
7. [Limitaciones conocidas](#limitaciones-conocidas)
8. [En qué estamos trabajando](#en-qué-estamos-trabajando)
9. [Compilar desde el código](#compilar-desde-el-código)
10. [Licencia y créditos](#licencia-y-créditos)

---

## Qué incluye

El paquete de la sección **Releases** trae:

| Archivo o carpeta | Qué es |
|---|---|
| `shadPS4QtLauncher.exe` y sus DLL | La interfaz **oficial** de shadPS4 ([shadps4-qtlauncher](https://github.com/shadps4-emu/shadps4-qtlauncher)), sin modificar. |
| `emulador\shadPS4.exe` | El emulador con las mejoras de este fork. |
| `config\CUSA00004.json` | Ajustes recomendados para inFAMOUS Second Son. |
| `Configurar_primera_vez.bat` | Registra el emulador en la interfaz y copia los ajustes del juego. |
| `LEEME.txt` | Instrucciones rápidas. |
| `LICENSE` | Licencia GPL-2.0. |

---

## Requisitos y PC de pruebas

**Requisitos:**
- Windows 10 o 11 de 64 bits.
- Una tarjeta gráfica con Vulkan 1.3 y drivers recientes.
- [Microsoft Visual C++ 2015-2022 (x64)](https://aka.ms/vs/17/release/vc_redist.x64.exe), que necesita la interfaz.
- Tu copia de inFAMOUS Second Son (CUSA00004) con la **actualización 1.07**.

**PC donde se hicieron todas las pruebas:**

| Componente | Modelo |
|---|---|
| Equipo | Laptop Lenovo LOQ 15IRX10 |
| Procesador | Intel Core i7-13650HX (14 núcleos: 6 P + 8 E) |
| Gráfica | NVIDIA GeForce RTX 5050 Laptop GPU, driver 617.14 |
| RAM | 32 GB DDR5 |
| Sistema | Windows 11 Home |
| Almacenamiento | SSD |

En otros equipos el rendimiento puede ser distinto. Tampoco se ha probado con gráficas AMD ni Intel.

---

## Instalación y uso

### 1. Prepara el juego
El emulador lee el juego como una carpeta ya extraída. La estructura debe quedar así:

```
PS4Games\
├── CUSA00004\           ← el juego base (contiene eboot.bin, sce_sys\...)
└── CUSA00004-UPDATE\    ← la actualización 1.07, extraída en una carpeta hermana
```

- Saca el juego base de **tu** disco o consola.
- Extrae el `.pkg` de **tu** actualización 1.07 (`...-A0107-V0100.pkg`) en una carpeta llamada exactamente `CUSA00004-UPDATE`, junto a `CUSA00004`. Para extraerlo hace falta una herramienta de extracción de PKG de PS4.
- Aquí no se distribuyen el juego, la actualización ni las claves.

### 2. Descomprime el paquete
Descomprime el zip del Release en una carpeta donde tengas permiso de escritura. Por ejemplo, `Documentos\inFAMOUS-shadPS4`. No uses `Archivos de programa`.

### 3. Configura (solo la primera vez)
Haz doble clic en **`Configurar_primera_vez.bat`**. El script:
- registra `emulador\shadPS4.exe` en la interfaz, con el nombre **"inFAMOUS SS (shadPS4 mod)"**;
- copia los ajustes recomendados a `%APPDATA%\shadPS4\custom_configs\CUSA00004.json`. Si ya tenías ese archivo, lo guarda antes como `CUSA00004.json.bak`.

Si mueves la carpeta a otro sitio, vuelve a ejecutarlo.

### 4. Juega
1. Abre **`shadPS4QtLauncher.exe`** desde esa misma carpeta.
2. La primera vez te pedirá la carpeta de juegos. Elige la carpeta que contiene `CUSA00004` (en el ejemplo, `PS4Games`).
3. Arriba a la derecha, comprueba que la versión seleccionada sea **"inFAMOUS SS (shadPS4 mod)"**.
4. Haz doble clic en inFAMOUS Second Son.

**La primera partida va más lenta:** el emulador compila los shaders del juego y los guarda en caché. Por eso algunos efectos pueden tardar un instante en aparecer. Desde la segunda vez va más fluido.

### Controles
- Mando: un DualSense o DualShock 4 funciona directamente. Un mando de Xbox también funciona.
- Teclado y ratón: se configuran desde la interfaz.
- **Agitar el mando (escena del grafiti con la lata de spray):** mantén pulsada la tecla **G**. Así se simula la sacudida del sensor de movimiento, aunque tu mando no tenga giroscopio. Puedes cambiar la tecla en la configuración de controles (acción `motion_shake`).

---

## Ajustes

Los ajustes del juego están en `%APPDATA%\shadPS4\custom_configs\CUSA00004.json`. También se pueden cambiar desde la interfaz, en la configuración específica del juego.

| Ajuste | Sección | Valor recomendado | Para qué sirve |
|---|---|---|---|
| `fps_limit` | GPU | `30` | Límite de fps. `30` da un ritmo de imagen estable. `0` lo quita. `-1` es automático (30 en este juego). |
| `readbacks_mode` | GPU | `2` | Lecturas precisas de la GPU. Hacen falta para efectos como el fuego y el humo. |
| `copy_gpu_buffers` | GPU | `true` | Necesario para que algunos efectos se vean bien. |
| `pipeline_cache_enabled` / `_archived` | Vulkan | `true` | Guarda los shaders compilados para no recompilarlos en cada partida. |
| `redzone_patches` | General | `true` | Protección de memoria que este juego necesita en Windows. |

**¿Por qué 30 fps por defecto?** En el PC de pruebas, sin límite, el juego va a unos 33-40 fps según la escena. Esa variación se nota más que un 30 fijo. Si tu PC es más potente, prueba con `"fps_limit": 0`.

Para pruebas puntuales, la variable de entorno `SHADPS4_FPS_CAP=N` tiene prioridad sobre el ajuste.

---

## Informes de fallo

Si el juego o el emulador se cierra por un error, en la carpeta de logs se crean automáticamente estos archivos:

```
%APPDATA%\shadPS4\log\
├── crash_AAAAMMDD_HHMMSS.txt      ← resumen: qué falló, dónde, registros y pila
├── crash_AAAAMMDD_HHMMSS.dmp      ← volcado de memoria (minidump) para depurar
└── crash_AAAAMMDD_HHMMSS_log.txt  ← copia del log en el momento del fallo
```

**Envíanos los 3 archivos.** Agrega una descripción de qué hacías en el juego: misión, zona y qué acababa de pasar. Puedes mandarlos abriendo un *Issue* en este repositorio.

Notas:
- Para abrir la carpeta, pega `%APPDATA%\shadPS4\log` en la barra del Explorador.
- El log normal (`shad_log.txt`) se sobrescribe cada vez que abres el juego. La copia `crash_..._log.txt` no se pierde.
- El `.dmp` no contiene tus partidas guardadas ni archivos personales. Sí contiene parte de la memoria del emulador en el momento del fallo.
- Si el juego siguió funcionando con normalidad después de la hora del informe, puedes ignorarlo.

---

## Cambios respecto a shadPS4

Este fork parte de shadPS4 (rama `main`, commit `06e813f`). Todo el código experimental de pruebas se **eliminó**; aquí solo queda lo que se probó y funcionó.

### Rendimiento y fluidez
- **Compilación de shaders en segundo plano:** los pipelines de Vulkan se compilan en hilos de baja prioridad, sin congelar el juego. Antes, cada pipeline nuevo costaba entre 100 y 300 ms en el driver y las cinemáticas bajaban a 0-12 fps. Ahora el juego sigue y el efecto aparece cuando está listo. Los pipelines terminados se guardan en la caché de disco.
- **Caché de shaders versionada** (`CUSA00004_v10`): una versión nueva del emulador no mezcla shaders de versiones viejas.
- **Lecturas de la GPU asíncronas:** cuando el juego lee datos que escribió la GPU, se descarga una ventana de 512 KB en segundo plano. Además, se anticipan las zonas que el juego lee seguido.
- **Menos trabajo repetido por dibujo:** se evitan llamadas repetidas para enlazar pipelines, buffers de vértices e índices, *push constants* (cerca del 80 % se ahorran) y descriptores.
- **Cachés internas:** búsqueda de texturas, descriptores de textura, residencia de buffers y especializaciones de shaders.
- **Hilos y CPU:**
  - El hilo que procesa los comandos de la GPU tiene prioridad alta y se mantiene en los núcleos de rendimiento (P-cores) de CPUs híbridas.
  - Windows ya no reduce el rendimiento del emulador cuando la ventana pierde el foco (EcoQoS desactivado).
- **Menos fallos de página:** al escribir en memoria vigilada se desprotegen 64 KB de una vez, no página por página.
- **Límite de fps configurable** (`fps_limit`), con 30 por defecto para este juego.

### Gráficos
- **Fuego, humo y exposición:**
  - Sincronización más amplia de la memoria compartida (LDS) en *compute shaders*.
  - Sincronización de memoria global en shaders *wave64*.
  - Estos dos cambios arreglan el parpadeo y los cuadros rotos en el fuego y el humo.
- **Bucles en shaders wave64:** los bucles uniformes ya no se tratan como divergentes, así que los shaders se ejecutan correctamente.
- **Partículas:** barrera compute→compute en el ordenamiento de partículas, para evitar que se lean datos a medio escribir.
- **Stencil:** corrección del stencil frontal/trasero cuando el juego usa valores distintos para cada cara.

### Estabilidad
- **Copias de command buffers estables:** antes, al crecer el buffer de copia, se movía la memoria y quedaban punteros inválidos, lo que podía cerrar el juego. Ahora se usa una arena de bloques fijos.
- **Si se llena la VRAM**, se reservan buffers e imágenes por encima del presupuesto en vez de cerrar el emulador.
- **Informes de fallo automáticos:** texto, minidump y copia del log (ver arriba).
- **Más información en el log** cuando hay un acceso inválido a memoria: dirección y registros.

### Accesibilidad
- **Agitar el mando con una tecla** (`motion_shake`, por defecto **G**), para las partes que piden sacudir el mando.

### Otros
- La compilación automática de GitHub (CI) de este fork solo genera la versión de Windows.

El detalle técnico de cada cambio está en el historial de commits de esta rama.

---

## Limitaciones conocidas

- **Pulsación o parpadeo intermitente de la iluminación:** a veces la escena "palpita" con un parpadeo de luz. Ocurre de forma aleatoria, no en todas las partidas. Se sigue investigando.
- **Rendimiento:** 60 fps todavía **no** se alcanzan. En el PC de pruebas, sin límite, las escenas pesadas con fuego van a unos 33-35 fps y otras zonas a 40-45. Por eso el límite por defecto es 30.
- **Primera vez en cada zona:** algunos efectos u objetos pueden aparecer con un pequeño retraso mientras se compilan sus shaders. Las siguientes veces salen de la caché.
- **Solo Windows.** No hay versiones para Linux ni macOS de este fork.
- **Pruebas limitadas:** solo se probó en un PC (ver arriba) y sobre todo el inicio del juego. El resto de la historia, las misiones secundarias y el DLC *First Light* no se han probado.
- **Las partidas guardadas** se guardan como en shadPS4 normal: `%APPDATA%\shadPS4\home\...\savedata\CUSA00004`. Haz copias de seguridad.

---

## En qué estamos trabajando

- Encontrar la causa de la **pulsación intermitente de la iluminación**.
- **Más fps:**
  - El juego pasa mucho tiempo esperando lecturas de la GPU, unos 0,5 s por cada segundo en la escena del fuego.
  - El costo restante está repartido: vigilancia de páginas, enlace de buffers y búsquedas de texturas.
  - La meta a largo plazo es 60 fps.
- Probar más partes del juego, con la ayuda de los informes de fallo que nos envíen.

---

## Compilar desde el código

Requisitos:
- Visual Studio 2022 o posterior, con "Desarrollo para el escritorio con C++" y las herramientas de Clang (clang-cl).
- CMake.
- Ninja.
- Git.

```bat
git clone --recursive -b infamous-second-son https://github.com/Nuigvarpu/shadPS4
cd shadPS4
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl
cmake --build build --parallel
```

Ejecuta estos comandos desde la consola "x64 Native Tools Command Prompt for VS". El ejecutable queda en `build\shadPS4.exe`.

---

## Licencia y créditos

- **shadPS4** es © shadPS4 Emulator Project y sus colaboradores. Se distribuye bajo la **GNU General Public License v2.0 o posterior** (`GPL-2.0-or-later`); ver [`LICENSE`](LICENSE). Algunos componentes de terceros tienen sus propias licencias, en la carpeta [`LICENSES`](LICENSES).
- **Este fork** se distribuye bajo la misma licencia GPL-2.0-or-later. El código fuente completo de cada versión publicada está en esta rama (`infamous-second-son`).
- **shadPS4 QtLauncher** es la interfaz oficial del proyecto shadPS4 ([shadps4-emu/shadps4-qtlauncher](https://github.com/shadps4-emu/shadps4-qtlauncher)), también bajo GPL-2.0-or-later. Se incluye sin modificaciones; su código fuente está en ese repositorio.
- *inFAMOUS* y *inFAMOUS Second Son* son marcas de Sony Interactive Entertainment. "PlayStation" y "PS4" son marcas de Sony Interactive Entertainment Inc. Este proyecto no está afiliado ni respaldado por ellos.
- La documentación original de shadPS4 está en [`README_shadPS4.md`](README_shadPS4.md).

Agradecimientos al equipo y la comunidad de shadPS4 por el emulador sobre el que se construye todo esto.
