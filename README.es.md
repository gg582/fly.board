# fly.board

![fly.board logo](img/logo.png)

> Uno de los pocos motores de blog sencillos que funciona con **~82 MB RSS** en reposo (con 4 workers; mantiene **90-200 MB** con un solo worker) y **~117 MB** bajo C10k (10 000 conexiones simultáneas).  
> Motor híbrido de foro y blog ligero construido sobre el framework web CWIST en C, con soporte para HTTPS/3, Argon2id, firmas PQC y mensajería NATS.
>
> **Fairly small, greater usability.**  
> TASFA sacrifica RPS intencionalmente. El cifrado por fragmentos, la validación de lattice HTP, las sesiones con mapa de bits y el pacing adaptativo garantizan que las cargas no se interrumpan incluso en redes degradadas, bloqueando ataques DoS y sustitución de fragmentos para perseguir una transferencia de confiabilidad maximizada.  
> Las firmas PQC absorben la sobrecarga de ML-DSA-65 para garantizar la detección de alteraciones en el cuerpo de las publicaciones en la era de la computación cuántica.  
> El soporte simultáneo de HTTP/1.1, HTTP/2 y HTTP/3 abandona el rendimiento máximo de un único protocolo a cambio de un denominador común accesible desde cualquier firewall, proxy y dispositivo heredado.

## Características

- **Eficiente en memoria** – Implementación en C con pila y montón. **~82 MB RSS** en reposo; **~117 MB** de RSS máximo con 10 000 conexiones simultáneas (C10k).
- **Transporte moderno** – TLS 1.3 + HTTP/3 (QUIC) por defecto. ECH (Encrypted Client Hello) opcional.
- **Autenticación segura** – Prehash SHA-512 del lado del cliente + **Argon2id** del lado del servidor (KDF de OpenSSL 3). Cookies de sesión JWT.
- **Híbrido foro / blog** – Publicaciones Markdown basadas en slug + múltiples tableros + comentarios anidados.
- **Vista previa en tiempo real** – Renderizado de vista previa del lado del servidor instantáneamente desde el editor Markdown.
- **Firmas PQC** – Adjuntar y verificar firmas criptográficas postcuánticas (PQC) en las publicaciones.
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
# o
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
| Tableros | `/boards` | Gestión de múltiples tableros (solo administradores) |
| Publicación | `/post/:slug` | Renderizado Markdown md4c + comentarios + adjuntos |
| Inicio de sesión/Registro | `/login`, `/register` | Argon2id + cookie JWT |
| Perfil | `/profile` | Apodo, biografía, foto de perfil, fecha de registro |
| Configuración de cuenta | `/account/settings` | Edición de perfil |
| Cambio de contraseña | `/account/password` | Verificar contraseña actual y volver a hashear con Argon2id |
| Administración | `/admin/users` | Cambiar roles de usuario, eliminar usuarios |
| Almacenamiento de archivos | `/files` | Subir/descargar/eliminar |

## Configuración

- `blog.settings` – Título del blog, subtítulo, pie de página, puerto
- `admin.settings` – Cuenta de administrador (2 líneas: `usuario`\n`contraseña`)

## Base de datos

SQLite3 (`data/blog.db`). El esquema se migra automáticamente al iniciar la aplicación.

```
users       – cuentas, hashes Argon2id, roles, perfiles
boards      – nombre/slug/descripción del tablero/admin_only
posts       – cuerpo Markdown, firma PQC, resumen
files       – ruta/tamaño/MIME del adjunto
comments    – comentarios anidados (target_type, parent_id)
board_permissions – permisos de acceso a tableros privados
```

## Arquitectura

```
CWIST (HTTP/3, TLS 1.3)
  ├── src/auth/     – Argon2id, JWT, sesiones
  ├── src/db/       – SQLite3 CRUD
  ├── src/handlers/ – enrutamiento/lógica de negocio
  ├── src/render/   – cwist_html_element SSR + md4c
  ├── src/crypto/   – firma/verificación PQC
  └── src/nats/     – mensajería Pub/Sub
```

## Licencia

MIT License

---

## Prueba de rendimiento

### Entorno del Host

| Elemento | Valor |
|------|-------|
| SO | Linux 7.0.0-mountain+ |
| Arquitectura | x86_64 |
| CPU | AMD Ryzen 5 5600X @ 3.70GHz (6 cores / 12 threads) |
| RAM | 64 GB |
| Disco | Samsung SSD 980 1TB (NVMe) |
| OpenSSL | 3.5.6 |
| Herramienta de Benchmark | wrk, h2load |
| CWIST | `patches/cwist` |

### Ajuste del Sistema

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

### Uso de Memoria

| Estado | RSS | Notas |
|-------|-----|-------|
| Inactivo | **~82 MB** (83,708 KB) | 4 workers, no connections |
| C10k | **~117 MB** (120,184 KB) | 10,000 concurrent connections |
| C100k | **~174 MB** (178,056 KB) | 100,000 concurrent connections |
| C1m | **~216 MB** (220,888 KB) | 1,000,000 concurrent connections |

### Prueba de Conexiones Simultáneas C10k

Medido con `h2load` manteniendo 10,000 conexiones simultáneas.

| Elemento | Valor |
|------|-------|
| Conexiones simultáneas | 10,000 |
| Duración | 21.72 s |
| RSS máximo | **~117 MB** (120,184 KB) |
| Uso de CPU | ~200% |
| User time | 35.19 s |
| System time | 8.39 s |
| Major page faults | **1** |
| Minor page faults | 57,581 |
| Voluntary context switches | 2,235,918 |
| Involuntary context switches | 405,099 |
| File system outputs | 8 |
| Total de peticiones | 20000 |
| Total exitosas | 20000 |
| Total fallidas | 0 |
| RPS total aprox. | **1291.35** |
| Tasa de éxito | **100.00%** |
| Estado de salida | **0** |

### Prueba de Conexiones Simultáneas C100k

Medido con `h2load` manteniendo 100,000 conexiones simultáneas.

| Elemento | Valor |
|------|-------|
| Conexiones simultáneas | 100,000 |
| Duración | 2:46.70 |
| RSS máximo | **~174 MB** (178,056 KB) |
| Uso de CPU | ~88% |
| User time | 118.41 s |
| System time | 28.31 s |
| Major page faults | **0** |
| Minor page faults | 150,669 |
| Voluntary context switches | 6,984,249 |
| Involuntary context switches | 1,081,830 |
| File system outputs | 8 |
| Total de peticiones | 200000 |
| Total exitosas | 200000 |
| Total fallidas | 0 |
| RPS total aprox. | **1244.21** |
| Tasa de éxito | **100.00%** |
| Estado de salida | **0** |

### Prueba de Conexiones Simultáneas C1m

Medido con `h2load` manteniendo 1,000,000 conexiones simultáneas.

| Elemento | Valor |
|------|-------|
| Concurrent connections | 1,000,000 |
| Duration | 10:13.39 |
| Max RSS | **~216 MB** (220,888 KB) |
| CPU usage | ~55% |
| User time | 201.98 s |
| System time | 136.96 s |
| Major page faults | **1** |
| Minor page faults | 220,927 |
| Voluntary context switches | 38,926,712 |
| Involuntary context switches | 4,460,022 |
| File system outputs | 8 |
| Total de peticiones | 2000000 |
| Total exitosas | 607048 |
| Total fallidas | 1392952 |
| RPS total aprox. | **1000.39** |
| Tasa de éxito | **30.35%** |
| Estado de salida | **0** |

> Nota: Valores medidos manteniendo conexiones de cliente reales sobre HTTP/2 (TLS 1.3).

**Puntos Fuertes del Benchmark C10k**
- **Eficiencia de Memoria**: RSS inferior a 120 MB con 10,000 conexiones simultáneas (~12 KB por conexión)
- **I/O de Disco Cero**: Major page faults 1, Swaps 0, FS inputs 0 — procesamiento puramente en memoria bajo carga
- **Alto Aprovechamiento de CPU**: ~200% de uso de CPU sostenido de forma estable
- **Estabilidad a Largo Plazo**: 21.72 s de carga C10k continua y cierre limpio (estado 0)
- **Seguridad de Datos**: SQLite persiste datos de forma segura tras SIGINT (8 FS outputs)
