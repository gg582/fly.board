# fly.board

![fly.board logo](img/logo.png)

> Uno de los pocos motores de blog sencillos que mantiene la memoria casi plana a medida que escalan las conexiones: **~82 MB RSS** en reposo (4 workers; mantiene **68–120 MB** en un servidor de producción real con un solo worker) y todavía **~94–96 MB** bajo C10k, C100k e incluso C1m.
> Motor híbrido ligero de foro y blog construido sobre el framework web CWIST en C, con soporte para HTTPS/3, Argon2id, firmas PQC y mensajería NATS.

## Características

- **Eficiente en memoria y escalable en conexiones** – Implementación en C con pila y montón. **~82 MB RSS** en reposo; el RSS se mantiene alrededor de **~94–96 MB** desde C10k hasta C1m conexiones simultáneas.
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
| C10k | **~96 MB** (96,252 KB) | +12.25 MB | 10,000 concurrent connections |
| C100k | **~94 MB** (94,352 KB) | -1,900 KB | 100,000 concurrent connections |
| C1m | **~95 MB** (94,944 KB) | +592 KB | 1,000,000 concurrent connections |

El cambio total de RSS de **C10k a C1m es de -1,308 KB** — básicamente ruido de medición. Este es el resultado más importante de la prueba.

Los valores RSS son el **Maximum resident set size (kbytes)** reportado por `/usr/bin/time -v` para el proceso del servidor.

### Costo de memoria

| Transición | Δ RSS | Δ Conexiones | Costo aproximado por conexión adicional |
|---|---|---|---|
| Idle → C10k | +12.25 MB | 10,000 | ~1.3 KB / conexión |
| C10k → C1m | -1,308 KB | 990,000 | ~-1.4 bytes / conexión adicional (ruido) |

El salto inicial de Idle a C10k paga por adelantado el estado TLS, los búferes de conexión y la sobrecarga de workers. Después de eso, el cambio de RSS de C10k a C1m se mantiene dentro del ruido de medición — el costo de memoria por conexión es efectivamente plano.

### Prueba de conexiones simultáneas C10k

Medido con `h2load` manteniendo 10,000 conexiones simultáneas.

| Elemento | Valor |
|------|-------|
| Workers | 4 |
| Conexiones simultáneas | 10,000 |
| Duración | 13.62 s |
| RSS máximo | **~96 MB** (96,252 KB) |
| Uso de CPU | ~578% |
| Tiempo de usuario | 72.53 s |
| Tiempo de sistema | 6.23 s |
| Fallos de página mayores | 0 |
| Fallos de página menores | 82,722 |
| Cambios de contexto voluntarios | 1,689,448 |
| Cambios de contexto forzosos | 18,959 |
| Salidas del sistema de archivos | 200 |
| Peticiones totales | 20000 |
| Exitosas totales | 20000 |
| Fallidas totales | 0 |
| RPS total aprox. | **2490.60** |
| Tasa de éxito | **100.00%** |
| Estado de salida | **0** |

### Prueba de conexiones simultáneas C100k

Medido con `h2load` manteniendo 100,000 conexiones simultáneas.

| Elemento | Valor |
|------|-------|
| Workers | 12 |
| Conexiones simultáneas | 100,000 |
| Duración | 1:24.88 |
| RSS máximo | **~94 MB** (94,352 KB) |
| Uso de CPU | ~871% |
| Tiempo de usuario | 701.20 s |
| Tiempo de sistema | 38.28 s |
| Fallos de página mayores | 0 |
| Fallos de página menores | 292,073 |
| Cambios de contexto voluntarios | 3,522,910 |
| Cambios de contexto forzosos | 184,003 |
| Salidas del sistema de archivos | 208 |
| Peticiones totales | 200000 |
| Exitosas totales | 200000 |
| Fallidas totales | 0 |
| RPS total aprox. | **2541.24** |
| Tasa de éxito | **100.00%** |
| Estado de salida | **0** |

### Prueba de conexiones simultáneas C1m

Medido con `h2load` manteniendo 1,000,000 conexiones simultáneas.

| Elemento | Valor |
|------|-------|
| Workers | 24 |
| Conexiones simultáneas | 1,000,000 |
| Duración | 7:05.71 |
| RSS máximo | **~95 MB** (94,944 KB) |
| Uso de CPU | ~641% |
| Tiempo de usuario | 2517.01 s |
| Tiempo de sistema | 215.52 s |
| Fallos de página mayores | 0 |
| Fallos de página menores | 789,451 |
| Cambios de contexto voluntarios | 23,921,809 |
| Cambios de contexto forzosos | 943,712 |
| Salidas del sistema de archivos | 208 |
| Peticiones totales | 2000000 |
| Exitosas totales | 711274 |
| Fallidas totales | 1288726 |
| RPS total aprox. | **1694.48** |
| Tasa de éxito | **35.56%** |
| Estado de salida | **0** |

> Nota: Valores medidos manteniendo conexiones reales de cliente sobre HTTP/2 (TLS 1.3). La cantidad de workers difiere en cada prueba; consulta "Qué mide esta prueba".

**Conclusiones clave**

- **Escalabilidad de conexiones**: El RSS se mantiene alrededor de **~94–96 MB** desde 10,000 hasta 1,000,000 conexiones simultáneas. El costo de memoria por conexión es efectivamente plano.
- **Estable bajo carga realista**: C10k y C100k terminaron con **100% de éxito** manteniéndose dentro del mismo margen de memoria.
- **El margen de memoria se mantiene en C1m**: Incluso cuando el hardware de prueba no pudo atender todas las 1,000,000 conexiones (35.56% de éxito), el uso de memoria permaneció esencialmente igual — el servidor no se descontroló.
- **Seguridad de datos**: SQLite persistió todos los datos de forma segura ante SIGINT (200 FS outputs en C10k).

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
| Latencia percentil aproximada* | p50 ~30.7 ms, p95 ~49.1 ms, p99 ~56.7 ms |

\* Los percentiles se aproximan a partir de la media y la desviación estándar reportadas; h2load imprime min/max/mean/sd por defecto. Ejecute con `--latency-collect` para histogramas percentiles exactos.

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
