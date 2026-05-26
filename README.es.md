# fly.board

![fly.board logo](img/logo.png)

> Uno de los pocos motores de blog sencillos que funciona con **~577 MB RSS** en reposo (con 4 workers; mantiene **90-200 MB** con un solo worker) y **~658 MB** bajo C10k (10 000 conexiones simultáneas).  
> Motor híbrido de foro y blog ligero construido sobre el framework web CWIST en C, con soporte para HTTPS/3, Argon2id, firmas PQC y mensajería NATS.
>
> **Fairly small, greater usability.**  
> TASFA sacrifica RPS intencionalmente. El cifrado por fragmentos, la validación de lattice HTP, las sesiones con mapa de bits y el pacing adaptativo garantizan que las cargas no se interrumpan incluso en redes degradadas, bloqueando ataques DoS y sustitución de fragmentos para perseguir una transferencia de confiabilidad maximizada.  
> Las firmas PQC absorben la sobrecarga de ML-DSA-65 para garantizar la detección de alteraciones en el cuerpo de las publicaciones en la era de la computación cuántica.  
> El soporte simultáneo de HTTP/1.1, HTTP/2 y HTTP/3 abandona el rendimiento máximo de un único protocolo a cambio de un denominador común accesible desde cualquier firewall, proxy y dispositivo heredado.

## Características

- **Eficiente en memoria** – Implementación en C con pila y montón. **~577 MB RSS** en reposo; **~658 MB** de RSS máximo con 10 000 conexiones simultáneas (C10k).
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
| OpenSSL | 3.5.5 |
| Herramienta de Benchmark | wrk, h2load |
| CWIST | `patches/cwist` |

### Rendimiento Máximo (RPS)

`wrk -t4 -c400 -d30s` (TLS 1.3)

| Endpoint | RPS Pico | Latencia Media | Notas |
|----------|----------|-------------|-------|
| `/` (Home) | **941.67** | 174.60ms | DB query + markdown rendering |
| `/login` | **927.08** | 175.83ms | Static form |
| `/boards` | **920.36** | 178.16ms | DB-driven list |

> Nota: Se producen errores de lectura de socket durante el cierre de conexiones TLS, pero no afectan la medición de rendimiento.

### Uso de Memoria

| Estado | RSS | Notas |
|-------|-----|-------|
| Inactivo | **~577 MB** (590,528 KB) | 4 workers, no connections |
| C10k | **~658 MB** (673,688 KB) | 10,000 concurrent connections |
| C100k | **~692 MB** (708,300 KB) | 100,000 concurrent connections |

### Prueba de Conexiones Simultáneas C10k

Medido con `h2load` manteniendo 10,000 conexiones simultáneas.

| Elemento | Valor |
|------|-------|
| Conexiones simultáneas | 10,000 |
| Duración | 21.98 s |
| RSS máximo | **~658 MB** (673,688 KB) |
| Uso de CPU | ~199% |
| User time | 36.41 s |
| System time | 7.43 s |
| Major page faults | **0** |
| Minor page faults | 170,352 |
| Voluntary context switches | 2,197,128 |
| Involuntary context switches | 293,375 |
| File system outputs | 72 |
| Estado de salida | **0** |

### Prueba de Conexiones Simultáneas C100k

Medido con `h2load` manteniendo 100,000 conexiones simultáneas.

| Elemento | Valor |
|------|-------|
| Conexiones simultáneas | 100,000 |
| Duración | 2:38.55 |
| RSS máximo | **~692 MB** (708,300 KB) |
| Uso de CPU | ~91% |
| User time | 120.81 s |
| System time | 24.13 s |
| Major page faults | **0** |
| Minor page faults | 191,633 |
| Voluntary context switches | 6,371,528 |
| Involuntary context switches | 842,479 |
| File system outputs | 72 |
| Estado de salida | **0** |

> Nota: Valores medidos manteniendo conexiones de cliente reales sobre HTTP/2 (TLS 1.3).

**Puntos Fuertes del Benchmark C10k**
- **Eficiencia de Memoria**: RSS inferior a 660 MB con 10,000 conexiones simultáneas (~66 KB por conexión)
- **I/O de Disco Cero**: Major page faults 0, Swaps 0, FS inputs 0 — procesamiento puramente en memoria bajo carga
- **Alto Aprovechamiento de CPU**: ~199% de uso de CPU sostenido de forma estable
- **Estabilidad a Largo Plazo**: 21.98 s de carga C10k continua y cierre limpio (estado 0)
- **Seguridad de Datos**: SQLite persiste datos de forma segura tras SIGINT (72 FS outputs)
