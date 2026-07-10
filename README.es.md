# fly.board

![fly.board logo](img/logo.png)

> Uno de los pocos motores de blog sencillos que mantiene la memoria casi plana a medida que escalan las conexiones: **~82 MB RSS** en reposo (4 workers; mantiene **68–120 MB** en un servidor de producción real con un solo worker) y todavía **~146 MB** bajo C10k, C100k e incluso C1m.  
> Motor híbrido ligero de foro y blog construido sobre el framework web CWIST en C, con soporte para HTTPS/3, Argon2id, firmas PQC y mensajería NATS.

## Características

- **Eficiente en memoria y escalable en conexiones** – Implementación en C con pila y montón. **~82 MB RSS** en reposo; el RSS se mantiene alrededor de **~146 MB** desde C10k hasta C1m conexiones simultáneas.
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

- `blog.settings` – Título del blog, subtítulo, pie de página, puerto y límites de subida
- `admin.settings` – Cuenta de administrador (2 líneas: `username`\n`password`)

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

La cantidad de workers se escala con la carga para mantener cada prueba realista: **4 workers** para C10k, **12 workers** para C100k y **24 workers** para C1m. Esto también explica las diferentes cifras de uso de CPU entre las tres ejecuciones.

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
| CWIST | `/usr/local/lib/libcwist.a` |

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
| En reposo | **~82 MB** (83,708 KB) | — | 4 workers, no connections |
| C10k | **~146 MB** (145,928 KB) | +62.22 MB | 10,000 concurrent connections |
| C100k | **~146 MB** (146,076 KB) | +148 KB | 100,000 concurrent connections |
| C1m | **~146 MB** (146,420 KB) | +344 KB | 1,000,000 concurrent connections |

El crecimiento total de RSS de **C10k a C1m es solo ~492 KB** — básicamente ruido. Este es el resultado más importante de la prueba.

### Prueba de conexiones simultáneas C10k

Medido con `h2load` manteniendo 10,000 conexiones simultáneas.

| Elemento | Valor |
|------|-------|
| Workers | 4 |
| Conexiones simultáneas | 10,000 |
| Duración | 17.04 s |
| RSS máximo | **~146 MB** (145,928 KB) |
| Uso de CPU | ~480% |
| Tiempo de usuario | 73.54 s |
| Tiempo de sistema | 8.25 s |
| Fallos de página mayores | 51 |
| Fallos de página menores | 267,239 |
| Cambios de contexto voluntarios | 1,959,611 |
| Cambios de contexto forzosos | 17,100 |
| Salidas del sistema de archivos | 10,600 |
| Peticiones totales | 20000 |
| Exitosas totales | 20000 |
| Fallidas totales | 0 |
| RPS total aprox. | **2383.81** |
| Tasa de éxito | **100.00%** |
| Estado de salida | **0** |

### Prueba de conexiones simultáneas C100k

Medido con `h2load` manteniendo 100,000 conexiones simultáneas.

| Elemento | Valor |
|------|-------|
| Workers | 12 |
| Conexiones simultáneas | 100,000 |
| Duración | 1:30.30 |
| RSS máximo | **~146 MB** (146,076 KB) |
| Uso de CPU | ~824% |
| Tiempo de usuario | 700.38 s |
| Tiempo de sistema | 44.12 s |
| Fallos de página mayores | 0 |
| Fallos de página menores | 472,679 |
| Cambios de contexto voluntarios | 3,908,475 |
| Cambios de contexto forzosos | 165,739 |
| Salidas del sistema de archivos | 101,672 |
| Peticiones totales | 200000 |
| Exitosas totales | 200000 |
| Fallidas totales | 0 |
| RPS total aprox. | **2458.23** |
| Tasa de éxito | **100.00%** |
| Estado de salida | **0** |

### Prueba de conexiones simultáneas C1m

Medido con `h2load` manteniendo 1,000,000 conexiones simultáneas.

| Elemento | Valor |
|------|-------|
| Workers | 24 |
| Conexiones simultáneas | 1,000,000 |
| Duración | 7:02.81 |
| RSS máximo | **~146 MB** (146,420 KB) |
| Uso de CPU | ~654% |
| Tiempo de usuario | 2553.88 s |
| Tiempo de sistema | 211.70 s |
| Fallos de página mayores | 3 |
| Fallos de página menores | 895,633 |
| Cambios de contexto voluntarios | 24,007,690 |
| Cambios de contexto forzosos | 931,088 |
| Salidas del sistema de archivos | 366,248 |
| Peticiones totales | 2000000 |
| Exitosas totales | 722910 |
| Fallidas totales | 1277090 |
| RPS total aprox. | **1744.04** |
| Tasa de éxito | **36.14%** |
| Estado de salida | **0** |

> Nota: Valores medidos manteniendo conexiones reales de cliente sobre HTTP/2 (TLS 1.3). La cantidad de workers difiere en cada prueba; consulta "Qué mide esta prueba".

**Conclusiones clave**

- **Escalabilidad de conexiones**: El RSS se mantiene alrededor de **~146 MB** desde 10,000 hasta 1,000,000 conexiones simultáneas. El costo de memoria por conexión es efectivamente plano.
- **Estable bajo carga realista**: C10k y C100k terminaron con **100% de éxito** manteniéndose dentro del mismo margen de memoria.
- **El margen de memoria se mantiene en C1m**: Incluso cuando el hardware de prueba no pudo atender todas las 1,000,000 conexiones (36.14% de éxito), el uso de memoria permaneció esencialmente igual — el servidor no se descontroló.
- **Seguridad de datos**: SQLite persistió todos los datos de forma segura ante SIGINT (10,600 FS outputs en C10k).

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

Como comparación, se probó el mismo endpoint con `wrk` sobre HTTP/1.1:

| Elemento | Valor |
|------|-------|
| Command | `wrk -t12 -c512 -d60s https://127.0.0.1:8888/` |
| Duration | 60 s |
| Requests/sec | **1282.49** |
| Transfer/sec | 52.29 MB |

Estos números muestran el techo de rendimiento absoluto del motor bajo una carga enfocada y sin límite de tasa. Son independientes de las pruebas de escalabilidad de conexiones anteriores.
