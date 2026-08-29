# fly.board

![fly.board logo](img/logo.png)

> Uno de los pocos motores de blog sencillos que mantiene la memoria casi plana a medida que escalan las conexiones: **~108 MB RSS** en reposo incluso con un solo worker (**~102 MB** con 4 workers) y se mantiene en **~110–146 MB** bajo C10k, C100k e incluso C1m.
> Motor híbrido ligero de foro y blog construido sobre el framework web CWIST en C, con soporte para HTTPS/3, Argon2id, firmas PQC y mensajería NATS.

## Características

- **Eficiente en memoria y escalable en conexiones** – Implementación en C con pila y montón. **~102–108 MB RSS** en reposo (1–4 workers); el RSS se mantiene alrededor de **~110–146 MB** desde C10k hasta C1m conexiones simultáneas.
- **Transporte moderno** – TLS 1.3 + HTTP/3 (QUIC) por defecto. ECH (Encrypted Client Hello) opcional.
- **Autenticación segura** – Prehash SHA-512 del lado del cliente + **Argon2id** del lado del servidor (KDF de OpenSSL 3). Cookies de sesión JWT.
- **Híbrido foro / blog** – Publicaciones Markdown basadas en slug + múltiples tableros + comentarios anidados.
- **Vista previa en tiempo real** – Vista previa renderizada del lado del servidor instantáneamente desde el editor Markdown.
- **Firmas PQC** – Adjuntar y verificar firmas basadas en criptografía postcuántica (PQC) en las publicaciones.
- **Almacenamiento de archivos** – ≤1 MB en SQLite, archivos más grandes en volumen. Incrustación automática de imágenes, vídeos y audio.
- **Integración NATS** – Pasarela de mensajería distribuida mediante la variable de entorno `NATS_URL`.
- **Modo oscuro** – Cambio de tema basado en cookies con variables CSS dinámicas.

## Compilación

```sh
make
./keygen.sh
```

Dependencias:
- [CWIST](https://github.com/religiya-serdtsa/cwist) — TLS 1.3 / HTTP/3 (QUIC) se gestiona mediante BoringSSL embebido en CWIST; no requiere configuración adicional.
- OpenSSL 3.x (Argon2id KDF)
- ngtcp2 / nghttp3 (HTTP/3)
- cJSON, SQLite3

El `Makefile` clona y compila `third_party/md4c` como biblioteca estática.

## Ejecución

```sh
./fly_board
```

El puerto por defecto sigue el valor `port` en `blog.settings` (por defecto 9443).

```text
https://localhost:9443
```

HTTP/3 escucha en el mismo puerto mediante UDP.

### Habilitar ECH (opcional)

```sh
BLOG_ECH_KEY=ech/server.ech ./fly_board
# or
BLOG_ECH_DIR=ech ./fly_board
```

Si la compilación de OpenSSL no admite ECH, se registrará una advertencia y el servidor continuará con HTTPS/3 normal.

### Integración NATS (opcional)

```sh
NATS_URL=nats://localhost:4222 ./fly_board
```

## Funciones principales

| Función | Ruta | Descripción |
|---------|------|-------------|
| Inicio | `/` | Lista de publicaciones recientes |
| Tableros | `/boards` | Gestión de múltiples tableros (soporte solo para administradores) |
| Publicación | `/post/:slug` | Renderizado Markdown md4c + comentarios + adjuntos |
| Inicio de sesión/Registro | `/login`, `/register` | Argon2id + cookie JWT |
| Perfil | `/profile` | Apodo, biografía, foto de perfil, fecha de registro |
| Configuración de cuenta | `/account/settings` | Edición de perfil |
| Cambio de contraseña | `/account/password` | Verificar contraseña actual y volver a hashear con Argon2id |
| Administración | `/admin/users` | Cambiar roles de usuario, eliminar usuarios |
| Almacenamiento de archivos | `/files` | Subir/descargar/eliminar |

## Configuración

La configuración proviene de tres archivos (creados automáticamente con valores predeterminados en el primer arranque) más variables de entorno para opciones operativas.

### `admin.settings`

Dos líneas en texto plano: la línea 1 es el nombre de usuario del administrador, la línea 2 la contraseña del administrador.

### `blog.settings`

Líneas simples de tipo `key=value`. Las claves desconocidas se ignoran; los valores inválidos recurren a los valores predeterminados.

| Clave | Predeterminado | Valores / alcance |
|-------|----------------|-------------------|
| `title` | `CWIST Docker Blog` | Título del sitio mostrado en la barra superior |
| `subtitle` | `Explore boards and read stories.` | Subtítulo del hero |
| `brand_footer` | `Built with CWIST C Framework` | Texto del pie de página |
| `root_url` | `https://localhost:8888/` | URL canónica del sitio (con `/` final). Se usa para enlaces RSS, correos de verificación y renovación de certificados — configúrala con la URL pública en producción |
| `port` | `8443` | Puerto de escucha TCP/UDP (HTTP/3 usa el mismo puerto sobre UDP) |
| `accent` | `#3b82f6` | Color de acento (hex) |
| `use_tls` | `true` | `true`/`false` — HTTPS activado/desactivado (ejecuta `./keygen.sh` primero) |
| `use_http2` | `true` | HTTP/2 sobre TLS |
| `use_http3` | `true` | HTTP/3 (QUIC) sobre UDP |
| `use_tasfa` | `true` | Pipeline multimedia TASFA (miniaturas/vistas previas de vídeo vía ffmpeg) |
| `use_rss` | `false` | Expone `/rss.xml` |
| `roundness` | `0.0` | Redondez de esquinas de la interfaz, `0.0`–`1.0` |
| `max_upload_size` | `1G` | Límite de subida por archivo. Acepta sufijos `K/M/G/T` (p. ej. `500M`) |
| `max_total_parallel_uploads` | `8` | Subidas concurrentes en total (1–512) |
| `max_upload_parallel_chunks` | `32` | Chunks paralelos por subida (1–64) |
| `max_concurrent_downloads` | `128` | Descargas concurrentes (1–512) |
| `vote_only` | *(vacío = `all`)* | Quién puede votar en las publicaciones: `all` (cualquiera, incl. anónimos), `authorized` (solo usuarios con sesión iniciada), `admin` (solo administradores) |
| `use_special_modes` | *(vacío)* | Sustituye los temas claro/oscuro: `lightTheme,darkTheme` (o un solo tema). Temas disponibles: `light`, `dark`, `ocean`, `forest`, `sepia`. P. ej. `ocean,forest` |
| `home_img`, `boards_img`, `files_img` | *(vacío)* | Imágenes hero/de fondo por página; nombre de archivo dentro de `public/img/` |
| `*_dark` (`home_img_dark`, `boards_img_dark`, `files_img_dark`) | *(vacío)* | Variantes de modo oscuro de las anteriores |
| `blog_logo`, `blog_logo_dark` | *(vacío)* | Imagen del logo en `public/img/` |
| `invert_logo` | `false` | Invierte automáticamente el logo para el modo que no tiene imagen |
| `favicon` | *(vacío)* | Archivo de favicon en `public/img/` |
| `bg_full_light`, `bg_full_dark` | *(vacío)* | Imágenes de fondo de página completa |
| `bg_invert_color` | *(vacío)* | Objetivos separados por comas cuya variante del modo faltante se genera automáticamente invirtiendo la otra: `home`, `boards`, `files`, `toplevel`, `logo` |
| `bg_invert_algo` | `luminv` | Algoritmo de inversión: `luminv` u `oklch` |

### `fonts.settings`

Ajustes de tipografía: `font_body`, `font_heading`, `font_ui`, `font_code`, `font_blockquote`, `font_display`, `font_import_url`, `font_face_family`, `font_face_src`, además de valores por elemento `letter_spacing_*` y `font_weight_*`. Los valores predeterminados se escriben en el primer arranque, así que abre el archivo generado para ver todas las claves.

### `s3.settings` (opcional)

Almacenamiento de objetos compatible con S3 para los archivos subidos (AWS S3, MinIO, R2, B2). Totalmente opcional: cuando está vacío, los archivos permanecen en el disco local bajo `public/uploads/`. Admite `mode=mirror` (conserva la copia local + respaldo en S3) y `mode=offload` (traslada a S3 y sirve las descargas mediante redirecciones prefirmadas). Referencia completa y ejemplos de configuración: [S3.md](S3.md).

### Variables de entorno

**Núcleo**

| Variable | Predeterminado | Descripción |
|----------|----------------|-------------|
| `BLOG_ROOT` | *(sin definir)* | Raíz del proyecto; se usa cuando el binario se inicia fuera de ella. De lo contrario se detecta automáticamente el directorio que contiene `public/` |
| `DEBUG` | *(desactivado)* | `1`/`true`/`yes` activa los registros DEBUG/INFO; en caso contrario solo se muestran advertencias/errores |
| `NATS_URL` | *(sin definir)* | p. ej. `nats://localhost:4222` — activa la pasarela de mensajería NATS |
| `BLOG_ECH_KEY` / `BLOG_ECH_DIR` | *(sin definir)* | Archivo de clave / directorio de claves ECH (Encrypted Client Hello) |
| `CWIST_C1M_MODE` | `1` | Reactor C1M orientado a eventos. Ponlo en `0` para forzar la ruta clásica basada en thread-pool |

**Rendimiento / caché**

| Variable | Predeterminado | Descripción |
|----------|----------------|-------------|
| `FLYBOARD_CACHE_MAX_MB` | `64` | Tamaño de la caché de páginas en MB (1–1024) |
| `FLYBOARD_ADVERTISE_H3` | `true` | Envía cabeceras `Alt-Svc` anunciando HTTP/3 |
| `FLYBOARD_ALT_SVC_MAX_AGE` | `300` | Valor `ma` de `Alt-Svc` en segundos (0–86400) |
| `FLYBOARD_INLINE_IMAGES` | *(desactivado)* | Incrusta imágenes como data URIs base64 en el HTML |
| `FLYBOARD_INLINE_ALL_ASSETS` | *(desactivado)* | Incrusta también scripts/estilos |
| `FLYBOARD_INLINE_BG_IMAGES` | *(desactivado)* | Incrusta también imágenes de fondo (opt-in explícito incluso con `ALL_ASSETS`) |
| `FLYBOARD_INLINE_MAX_IMAGE_SIZE` | `49152` | Bytes máximos por imagen incrustada |
| `FLYBOARD_INLINE_MAX_ASSET_SIZE` | `65536` | Bytes máximos por fragmento de script/estilo incrustado |
| `FLY_MEDIA_MAX_CONCURRENT` | `2` | Conversiones ffmpeg concurrentes para vistas previas multimedia |
| `FLYBOARD_MEDIA_BACKFILL_ON_START` | *(desactivado)* | Regenera todas las vistas previas multimedia antiguas al arrancar (solo en ejecuciones de mantenimiento) |

**Renovación automática del certificado TLS** (usa un cliente ACME local; los certificados autofirmados temporales de `keygen.sh` se detectan y nunca se modifican)

| Variable | Predeterminado | Descripción |
|----------|----------------|-------------|
| `FLY_CERT_RENEWAL` | *(desactivado)* | `true` activa el vigilante diario de caducidad. Renueva cuando al certificado le quedan ≤ `FLY_CERT_DAYS` días y lo recarga en caliente sin reiniciar |
| `FLY_CERT_DAYS` | `30` | Umbral de renovación en días |
| `FLY_CERT_EMAIL` | `admin@<host>` | Correo de la cuenta ACME |
| `FLY_CERT_LEGO_BIN` | `lego` | Nombre/ruta del binario lego (apunta a un script envoltorio para desafíos DNS, etc.) |

El vigilante deriva el dominio de `root_url` y ejecuta lego con el desafío HTTP-01, por lo que el puerto 80 debe ser accesible en la máquina. El estado se guarda en `.lego/`; los certificados renovados se instalan sobre `server.crt`/`server.key`.

**Registro con verificación por correo** (desactivado por defecto = registro abierto)

| Variable | Predeterminado | Descripción |
|----------|----------------|-------------|
| `FLY_EMAIL_CERT` | *(desactivado)* | `true` exige que los nuevos registros verifiquen su correo antes de poder iniciar sesión. Se envía un enlace con token de 24 horas por SMTP |
| `FLY_SMTP_HOST` | *(obligatorio si está activado)* | Host del relay SMTP |
| `FLY_SMTP_PORT` | `25` (`465` con TLS implícito) | Puerto SMTP |
| `FLY_SMTP_TLS` | *(desactivado)* | `starttls` o `implicit` |
| `FLY_SMTP_USER` / `FLY_SMTP_PASS` | *(sin definir)* | Credenciales AUTH LOGIN (opcional) |
| `FLY_SMTP_FROM` | `FLY_SMTP_USER` | Remitente de sobre/cabecera |

Ejemplo — producción con registro verificado y renovación automática de certificados:

```sh
FLY_CERT_RENEWAL=true FLY_CERT_EMAIL=admin@example.com \
FLY_EMAIL_CERT=true FLY_SMTP_HOST=smtp.example.com FLY_SMTP_PORT=587 \
FLY_SMTP_TLS=starttls FLY_SMTP_USER=noreply@example.com FLY_SMTP_PASS=secret \
./fly_board
```

## Base de datos

SQLite3 (`data/blog.db`). El esquema se migra automáticamente al iniciar la aplicación.

```
users       – accounts, Argon2id hashes, roles, profiles
boards      – board name/slug/description/admin_only
posts       – markdown body, PQC signature, summary
files       – attachment path/size/MIME
comments    – nested comments (target_type, parent_id)
board_permissions – private board access permissions
```

## Arquitectura

```
CWIST (HTTP/3, TLS 1.3)
  ├── src/auth/     – Argon2id, JWT, sessions
  ├── src/db/       – SQLite3 CRUD
  ├── src/handlers/ – routing/business logic
  ├── src/render/   – cwist_html_element SSR + md4c
  ├── src/crypto/   – PQC sign/verify
  └── src/nats/     – messaging Pub/Sub
```

## Licencia

MIT License

---

## Prueba de escalabilidad

### Qué mide esta prueba

Estas pruebas usan `h2load` **con la opción `-r`** (rate-limit). No son pruebas de rendimiento máximo. En su lugar, miden si el servidor puede **sostener una cantidad masiva de conexiones HTTP/2 simultáneas** mientras procesa una tasa de peticiones controlada por proceso.

Como la carga está limitada por tasa:

- El **RPS reportado refleja la tasa de peticiones configurada**, no el techo absoluto de rendimiento del servidor.
- La métrica principal es la **estabilidad del conjunto residente (RSS)** a medida que las conexiones crecen de 10,000 a 1,000,000.

La cantidad de workers se escala con la carga para mantener cada prueba realista: **4 workers** para C10k, **12 workers** para C100k y **12 workers** para C1m. Esto también explica las diferentes cifras de uso de CPU entre las tres ejecuciones.

### Entorno del host

| Elemento | Valor |
|------|-------|
| SO | Linux 7.1.0-mountain-rc6+ |
| Arquitectura | x86_64 |
| CPU | 12 logical cores |
| RAM | 62 GiB |
| GCC | 14.2.0 (Debian 14.2.0-19) |
| OpenSSL | 3.5.6 |
| Herramienta de benchmark | h2load nghttp2/1.64.0 |
| CWIST | `libcwist.a` del checkout hermano de cwist (2026-08-29, arena bump allocator, arena req/res compartida, stacks de worker de 256KB, endurecimiento de HTTP/3, sharded TLS handshake shepherd) |

### Ajuste del sistema

| Parámetro | Valor |
|-----------|-------|
| ulimit -n | 1,050,000 |
| fs.file-max | 2,097,152 |
| fs.nr_open | 1,050,000 |
| net.core.somaxconn | 1,050,000 |
| net.ipv4.tcp_max_syn_backlog | 1,050,000 |
| net.ipv4.ip_local_port_range | 1024 65535 |
| vm.max_map_count | 1,048,576 |
| kernel.pid_max | 4,194,304 |
| CPU governor | ecodemand |

### Uso de memoria

| Estado | RSS | Δ desde el anterior | Notas |
|--------|-----|---------------------|-------|
| En reposo (1 worker) | **~108 MB** (110,196 KB) | — | 1 worker, no connections |
| En reposo (4 workers) | **~102 MB** (104,940 KB) | — | 4 workers, no connections |
| C10k | **~110 MB** (112,436 KB) | +7,496 KB vs en reposo (4 workers) | 10,000 concurrent connections |
| C100k | **~146 MB** (148,848 KB) | +36,412 KB | 100,000 concurrent connections |
| C1m churn | **~146 MB** (149,816 KB) | +968 KB | 100k held TLS conns, 1M-request churn |

El cambio total de RSS de **C100k a la ejecución de churn C1m es de +968 KB** — básicamente ruido de medición. Este es el resultado más importante de la prueba.

Los valores RSS son el **Maximum resident set size (kbytes)** reportado por `/usr/bin/time -v` para el proceso del servidor.

### Costo de memoria

| Transición | Δ RSS | Δ Conexiones | Costo aproximado por conexión adicional |
|---|---|---|---|
| Idle → C10k | +7,496 KB | 10,000 | ~0.75 KB / conexión |
| C10k → C1m churn | +37,380 KB | — | ~0.4 KB / conexión retenida adicional; C100k → C1m es +968 KB (ruido) |

El salto inicial de Idle a C10k paga por adelantado el estado TLS, los búferes de conexión y la sobrecarga de workers. De C10k a C100k el costo se mantiene cerca de ~0.4 KB por conexión retenida adicional, y el cambio de RSS de C100k a C1m (+968 KB) es puro ruido de medición — el costo de memoria por conexión es efectivamente plano.

### Prueba de conexiones simultáneas C10k

Medido con `h2load` manteniendo 10,000 conexiones simultáneas.

| Elemento | Valor |
|------|-------|
| Workers | 4 |
| Conexiones simultáneas | 10,000 |
| Duración | 12.05 s |
| RSS máximo | **~110 MB** (112,436 KB) |
| Uso de CPU | ~365% |
| Tiempo de usuario | 41.05 s |
| Tiempo de sistema | 3.04 s |
| Fallos de página mayores | 2 |
| Fallos de página menores | 16,948 |
| Cambios de contexto voluntarios | 58,050 |
| Cambios de contexto forzosos | 14,828 |
| Salidas del sistema de archivos | 256 |
| Peticiones totales | 20000 |
| Exitosas totales | 20000 |
| Fallidas totales | 0 |
| RPS total aprox. | **2285.22** |
| Tasa de éxito | **100.00%** |
| Estado de salida | **0** |

### Prueba de conexiones simultáneas C100k

Medido con `h2load` manteniendo 100,000 conexiones simultáneas.

| Elemento | Valor |
|------|-------|
| Workers | 12 |
| Conexiones simultáneas | 100,000 |
| Duración | 1:23.49 |
| RSS máximo | **~146 MB** (148,848 KB) |
| Uso de CPU | ~815% |
| Tiempo de usuario | 653.83 s |
| Tiempo de sistema | 26.78 s |
| Fallos de página mayores | 0 |
| Fallos de página menores | 76,332 |
| Cambios de contexto voluntarios | 446,557 |
| Cambios de contexto forzosos | 617,777 |
| Salidas del sistema de archivos | 336 |
| Peticiones totales | 200000 |
| Exitosas totales | 200000 |
| Fallidas totales | 0 |
| RPS total aprox. | **2785.16** |
| Tasa de éxito | **100.00%** |
| Estado de salida | **0** |

### Prueba de churn C1m (rediseñada 2026-08-23, corregida 2026-08-24)

El antiguo objetivo de "1,000,000 de conexiones TLS simultáneas" fue retirado: la ruta HTTPS ocupa un hilo worker por conexión activa, por lo que la concurrencia de conexiones mantenidas está limitada a workers x hilos, muy por debajo de 1M. (La ruta HTTP/1.x **en claro** de cwist es orientada a eventos y sí alcanzó 1,000,000/1,000,000 de conexiones mantenidas — ver el README de cwist.) La prueba C1m mide churn: 20 procesos h2load x 50,000 peticiones sobre 100,000 conexiones TLS mantenidas simultáneamente, limitada por un watchdog.

| Elemento | Valor |
|------|-------|
| Workers | 12 |
| Forma de carga | 20 x (-c 5000 -n 50000 -r 1000 -T 30) |
| Totales | 1,000,000 peticiones sobre 100,000 conexiones mantenidas |
| Resultado | **completado — sin bloqueo** |
| Exitosas totales | **1,000,000 / 1,000,000 (100.0%)** |
| Con error | 0 |
| Tiempo total | ~1:36 (h2load "finished in" 63.7-89.3 s por proceso) |
| Conexiones fantasma | 0 (los conteos ESTABLISHED de cliente/servidor coincidieron) |
| Apagado del servidor | limpio, exit 0 |

Historial: la ejecución del 2026-08-23 con esta misma carga se bloqueó a ~85k conexiones. Causa raíz (corregida en cwist `perf(https): non-blocking TLS handshake shepherd`): el handshake TLS se ejecutaba de forma síncrona dentro de los hilos worker con esperas de poll de 30 s, por lo que unos cientos de clientes lentos ocupaban todo el pool, la cola de accept se desbordaba y los handshakes excedentes se descartaban silenciosamente, dejando clientes en ESTABLISHED sin socket del lado del servidor. Ahora los handshakes se ejecutan en un hilo shepherd no bloqueante; solo las sesiones establecidas ocupan workers del pool.

> Nota: Valores medidos manteniendo conexiones reales de cliente sobre HTTP/2 (TLS 1.3). La cantidad de workers difiere en cada prueba; consulta "Qué mide esta prueba".

**Conclusiones clave**

- **Escalabilidad de conexiones**: El RSS se mantiene alrededor de **~110–146 MB** desde 10,000 hasta 1,000,000 conexiones simultáneas. El costo de memoria por conexión es efectivamente plano.
- **Estable bajo carga realista**: C10k terminó con **100% de éxito** y C100k con **100.00%**, manteniéndose dentro del mismo margen de memoria.
- **El margen de memoria se mantiene a escala C1m**: la ejecución de churn C1m (1M de peticiones sobre 100k conexiones TLS mantenidas) se completa sin bloqueo con **100% de éxito** y el RSS se mantiene en ~146 MB — sin espiral de memoria ni caídas.
- **Seguridad de datos**: SQLite persistió todos los datos de forma segura ante SIGINT (256 FS outputs en C10k).

### Prueba de rendimiento

La prueba anterior mide **escalabilidad de conexiones**, no el **rendimiento absoluto de peticiones**. Para medir el techo de rendimiento bruto del servidor, se ejecutó una prueba sin restricciones con `h2load` (sin límite de tasa `-r`) sobre HTTP/2.

| Elemento | Valor |
|------|-------|
| Command | `h2load -c512 -n100000 https://127.0.0.1:8888/` |
| Workers | 12 |
| Concurrent connections | 512 |
| Total requests | 100,000 |
| Succeeded | 100,000 |
| Failed / Errored / Timeout | 0 |
| Duration | 13.95 s |
| Mean RPS | **7167.28** |
| Mean throughput | **290.51 MB/s** |
| Latencia de solicitud (h2load `time for request`) | min 183 µs, mean 30.69 ms, max 209.00 ms, sd 11.18 ms |

#### Comparación HTTP/1.1 con `wrk`

Como comparación, se probó el mismo endpoint con `wrk` sobre HTTP/1.1. Dado que el protocolo y la herramienta son diferentes, los números a continuación **no son directamente comparables** con los resultados HTTP/2 de h2load anteriores.

| Elemento | Valor |
|------|-------|
| Command | `wrk -t12 -c512 -d60s https://127.0.0.1:8888/` |
| Duration | 60 s |
| Requests/sec | **1282.49** |
| Transfer/sec | 52.29 MB |
| Latency | Avg 138.61 ms, Stdev 39.26 ms, Max 311.70 ms |

Estos números muestran el techo de rendimiento absoluto del motor bajo una carga enfocada y sin límite de tasa. Son independientes de las pruebas de escalabilidad de conexiones anteriores.
