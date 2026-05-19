# fly.board

![fly.board logo](img/logo.png)

> Uno de los pocos motores de blog sencillos que funciona con **100-200 MB RSS** en reposo y **~369 MB** bajo C10k (10 000 conexiones simultáneas).  
> Motor híbrido de foro y blog ligero construido sobre el framework web CWIST en C, con soporte para HTTPS/3, Argon2id, firmas PQC y mensajería NATS.

## Características

- **Eficiente en memoria** – Implementación en C con pila y montón. **100-200 MB RSS** en reposo; **~369 MB** de RSS máximo con 10 000 conexiones simultáneas (C10k).
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

> Para conocer la metodología detallada y los resultados completos, consulta [`benchmarks/README.md`](benchmarks/README.md).

### Entorno del host

| Elemento | Valor |
|------|-------|
| SO | Linux 7.0.0-mountain+ |
| Arquitectura | x86_64 |
| CPU | AMD Ryzen 5 5600X @ 3.70GHz (6 núcleos / 12 hilos) |
| RAM | 64 GB |
| Disco | Samsung SSD 980 1TB (NVMe) |
| OpenSSL | 3.5.5 |
| Herramienta de prueba | wrk |
| CWIST | `patches/cwist` (parche SIGPIPE aplicado) |

### Rendimiento máximo (RPS)

`wrk -t4 -c400 -d30s` (TLS 1.3, sin serialización)

| Endpoint | RPS pico | Latencia media | Notas |
|----------|----------|-------------|-------|
| `/` (Inicio) | **3.409,92** | 121,84 ms | Consulta a BD + renderizado Markdown |
| `/login` | **3.948,77** | 18,03 ms | Formulario estático (almacenable en caché) |
| `/boards` | **3.901,77** | 17,26 ms | Lista basada en BD |

### Uso de recursos (carga pico)

| Elemento | Valor |
|------|-------|
| Uso de CPU | ~600% (en sistema de 12 hilos) |
| RAM (RSS) | ~12 MB |
| Memoria virtual (VSZ) | ~1,2 GB |

> Nota: Las pruebas se ejecutaron **sin** serialización de solicitudes (`pthread_mutex_t`).  
> `ulimit -n` se configuró en 20.000, permitiendo mediciones estables hasta 400 conexiones.

### Prueba de conexiones simultáneas C10k

Medición manteniendo 10 000 conexiones simultáneas en un entorno real (`sudo -E /usr/bin/time -v ./fly_board`).

| Elemento | Valor |
|------|-------|
| Conexiones simultáneas | 10 000 |
| Duración | 24 min 46 s |
| RSS máximo | **~369 MB** (368 644 KB) |
| Uso medio de CPU | ~93% |
| User time | 444,17 s |
| System time | 951,76 s |
| Major page faults | **0** (sin E/S de disco) |
| Minor page faults | 219.629 |
| Swaps | **0** |
| File system inputs | **0** |
| File system outputs | 89.208 (persistencia segura) |
| Voluntary context switches | 346.110.015 |
| Involuntary context switches | 1.690.588 |
| Estado de salida | **0** (cierre limpio tras SIGINT) |

> Nota: Valores medidos manteniendo 10 000 conexiones de cliente reales sobre HTTP/3 (QUIC) con TLS 1.3.

**Puntos fuertes del benchmark C10k**
- **Eficiencia de memoria**: RSS inferior a 400 MB con 10 000 conexiones simultáneas (~37 KB por conexión)
- **Sin E/S de disco**: Major page faults 0, Swaps 0, FS inputs 0 — procesamiento puramente en memoria bajo carga
- **Alto aprovechamiento de CPU**: ~93% de uso sostenido sin pérdida de estabilidad
- **Estabilidad a largo plazo**: 24 min 46 s de carga C10k continua y cierre limpio (estado 0)
- **Seguridad de datos**: NukeDB persistió los datos de forma segura tras SIGINT (89 208 FS outputs)
