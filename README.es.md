# fly.board

![fly.board logo](img/logo.png)

> Uno de los pocos motores de blog sencillos que mantiene **1.000.000 de conexiones concurrentes** en un solo host de escritorio (medido sobre HTTP/1.1 en claro, TLS HTTP/1.1 y TLS HTTP/2) y supera las baterías de carga TLS C10k/C100k con un 100% de éxito.
> Motor híbrido ligero de foro y blog construido sobre el framework web CWIST en C, con soporte para HTTPS/3, Argon2id, firmas PQC y mensajería NATS.

## Características

- **Escalable en conexiones** – Implementación en C con pila y montón sobre el reactor basado en eventos de cwist. **1.000.000 de conexiones concurrentes** mantenidas y atendidas en ejecuciones medidas sobre HTTP/1.1 en claro, TLS HTTP/1.1 y TLS HTTP/2; las páginas públicas anónimas se sirven desde una caché Big Dumb Reply.
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

### Qué miden estas pruebas

Tres cosas distintas, que las versiones anteriores de esta sección mezclaban:

- **Conexiones concurrentes** (el sentido clásico de C10K/C1M): cuántas conexiones mantiene abiertas y atiende el servidor al mismo tiempo. Se mide con `tools/connhold` mediante `run_c1m_held_bench.sh`.
- **Churn de peticiones sobre conexiones mantenidas**: la batería `h2load` (`run_c10k_bench.sh`, `run_c100k_bench.sh`, `run_c1m_bench.sh`). `h2load` se ejecuta con `-r` (límite de tasa), así que las cifras de RPS reflejan la carga configurada, no un techo de rendimiento. `run_c1m_bench.sh` es concurrencia C100K con 1.000.000 de peticiones; el nombre es histórico.
- **Rendimiento**: ejecuciones sin límite de `h2load` y `wrk` contra la portada.

### Entorno del host

| Elemento | Valor |
|------|-------|
| SO | Linux 6.12.107+deb13-amd64 (Debian 13) |
| CPU | AMD Ryzen 5 5600X (6 núcleos / 12 hilos) |
| RAM | 62 GiB |
| GCC | 14.2.0 (Debian 14.2.0-19) |
| Generadores de carga | h2load nghttp2/1.64.0, wrk, `tools/connhold` (BoringSSL) |
| CWIST | `main` en `11f3518d` (v3.7.1, 2026-09-29) |
| Certificado TLS | ECDSA P-256 (predeterminado de `keygen.sh`) |
| Modo de servicio | `CWIST_C1M_MODE=1` (reactor basado en eventos) |

Cliente y servidor se ejecutan en el mismo host.

### Ajustes del sistema

| Parámetro | Valor |
|-----------|-------|
| ulimit -n | 1.050.000 |
| fs.file-max | 8.388.608 (1M de conexiones mantenidas necesitan 2M de fds, cliente + servidor) |
| fs.nr_open | 1.050.000 |
| net.netfilter.nf_conntrack_max | 4.194.304 (las conexiones loopback también se rastrean) |
| net.core.somaxconn | 1.050.000 |
| net.ipv4.tcp_max_syn_backlog | 1.050.000 |
| net.ipv4.ip_local_port_range | 1024 65535 |
| vm.max_map_count | 1.048.576 |
| kernel.pid_max | 4.194.304 |
| Gobernador de CPU | ecodemand |

### C1M: 1.000.000 de conexiones concurrentes (2026-09-29)

`run_c1m_held_bench.sh`: 12 workers, 1.000.000 de conexiones repartidas entre 48 direcciones loopback. Cada conexión envía `GET /robots.txt` y lo repite cada 60–120 s para que nunca venza el temporizador de keep-alive (HTTP/2: un stream nuevo cada vez). Las conexiones fallidas se cuentan y nunca se reintentan.

| | HTTP/1.1 en claro | TLS 1.3 + HTTP/1.1 | TLS 1.3 + HTTP/2 |
|---|---|---|---|
| Conexiones abiertas | 1.000.000 | 1.000.000 | 1.000.000 |
| Tasa de conexión | 40.000 / s | 8.000 / s | 8.000 / s |
| Máximo mantenidas | **1.000.000** | **1.000.000** | **1.000.000** |
| Máximo atendidas a la vez | **1.000.000** | **1.000.000** | **1.000.000** |
| Respuestas (incl. GET de keep-alive) | 2.124.089 | 2.632.665 | 2.052.855 |
| Conexiones fallidas | 0 | 0 | 0 |
| Memoria del servidor en el pico (PSS, todos los workers) | 24,0 GB | 19,9 GB | 30,1 GB |

- **Los tres modos mantienen y atienden 1.000.000 de conexiones concurrentes sin ningún fallo.**
- TLS necesita CWIST v3.7.1 (`11f3518d`). Antes, un hilo del pool esperaba en cada conexión TLS inactiva, así que solo se atendían a la vez tantas conexiones TLS como hilos tenía el pool: la misma ejecución mantuvo 395.729 conexiones TLS pero atendió como máximo 25. v3.7.1 aparca las conexiones TLS inactivas en un conjunto epoll y las devuelve al pool cuando llegan datos.
- La tasa de conexión TLS la limitan los handshakes completos: a 20.000 conexiones nuevas por segundo, ~37% de los handshakes agotaron el presupuesto de 45 s (523.654 conexiones HTTP/1.1 y 510.023 HTTP/2 atendidas); a 8.000/s ninguno.
- Trampa de la carga: `connect()` de Linux reparte primero los puertos efímeros pares y luego pasa a una búsqueda lenta de impares, así que cada dirección de destino da unas 32k conexiones rápidas. Con 24 direcciones todas las ejecuciones se atascaron cerca de 774k; usa al menos `conexiones / 32.000` direcciones.

### Memoria

PSS sumado sobre todos los procesos del servidor (maestro y workers bifurcados).

| Caso | Total del servidor | Por conexión |
|---|---|---|
| En reposo, 1 worker | 115 MB | — |
| En reposo, 4 workers | 406 MB | — |
| 100k conexiones TLS/HTTP/2 (h2load C100k) | 1,77 GB (1,11 GB en reposo tras calentar) | ~6,7 KB |
| 1M conexiones HTTP/1.1 en claro (connhold) | 24,0 GB | ~24 KB |
| 1M conexiones TLS HTTP/1.1 (connhold) | 19,9 GB | ~20 KB |
| 1M conexiones TLS HTTP/2 (connhold) | 30,1 GB | ~30 KB |

Costes del lado cliente para planificar ejecuciones en el mismo host: h2load ~60 KB por conexión, connhold ~0,06 KB más la memoria de sockets del kernel (~12,7 KB por par de conexiones en C100k).

> **Corrección:** versiones anteriores de este README afirmaban ~102–108 MB en reposo y ~110–146 MB de C10k a C1m. Eran el RSS máximo de `/usr/bin/time -v` solo del proceso maestro; `cwist_app_listen()` hace fork de los workers que sirven, y su memoria nunca se contó. Los totales de arriba las sustituyen.

### Batería h2load: churn de peticiones (2026-09-29, CWIST `11f3518d`)

| Prueba | Conexiones concurrentes | Peticiones | Éxito | Tiempo | RPS (suma de procesos) |
|---|---|---|---|---|---|
| C10k (4 workers) | 10.000 | 20.000 | **100%** | 8,50 s | 9.253 |
| C100k (12 workers) | 100.000 | 200.000 | **100%** | 29,83 s | 8.958 |
| C1m churn (12 workers) | 100.000 | 1.000.000 | **100%** | 57,28 s | 19.754 |

El tiempo es la vida del proceso servidor (incluye el arranque y el drenado de apagado de 5 s). Las respuestas son la portada completa de 79 KB; h2load no pide compresión.

Ese mismo día, antes y con un certificado RSA-4096, C100k cayó al 72,6% y el churn C1m al 65,2%: casi toda la CPU ocupada se iba en la firma RSA CertificateVerify de cada handshake completo de TLS 1.3, así que los handshakes en cola agotaban el presupuesto de 45 s. Cambiar a ECDSA P-256 (predeterminado de `keygen.sh`), ejecutar los manejadores de ruta en workers de peticiones y servir las páginas públicas anónimas desde la caché Big Dumb Reply de rutas devolvió el 100%.

### Prueba de rendimiento (2026-09-29, CWIST `11f3518d`)

Carga sin límite (sin `-r`) contra la portada, 12 workers.

| Herramienta | Comando | Resultado |
|---|---|---|
| h2load (HTTP/2) | `h2load -c512 -n100000 https://127.0.0.1:8888/` | **12.337 req/s**, 970,71 MB/s, 100.000/100.000 con éxito, 8,11 s; tiempo por petición mín. 410 µs, media 20,69 ms, máx. 205,85 ms |
| wrk (HTTP/1.1) | `wrk -t12 -c512 -d60s https://127.0.0.1:8888/` | **48.829 req/s**, 3,76 GB/s; latencia media 9,50 ms, desviación 4,15 ms, máx. 132,20 ms; sin errores de socket |

Las dos herramientas usan protocolos distintos y no son directamente comparables. En 2026-08 los mismos comandos dieron 7.167 req/s (h2load) y 1.282 req/s con 77.027 errores de lectura (wrk); ahora las peticiones anónimas a la portada se responden desde la caché Big Dumb Reply de rutas.

**Conclusiones clave**

- **C1M:** 1.000.000 de conexiones concurrentes mantenidas y atendidas en un solo host de escritorio sobre HTTP/1.1 en claro, TLS HTTP/1.1 y TLS HTTP/2, cero fallos.
- **La memoria por conexión es real:** ~20–30 KB por conexión mantenida en el servidor; dimensiona la RAM en consecuencia.
- **La tasa de conexión TLS la limitan los handshakes:** usa un certificado ECDSA; en este host ~8.000 handshakes completos por segundo terminaron sin pérdidas.
