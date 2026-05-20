# Análisis de la firma PQC (Criptografía Post-Cuántica)

## Visión general

fly.board anuncia firmas "resistentes a la computación cuántica" en las publicaciones. Este documento explica **cómo funciona realmente esta característica** y cuáles son sus **limitaciones reales**.

## Cómo se logra la PQC

### 1. Algoritmo: ML-DSA-65

- Según los comentarios en `src/crypto/fly_crypto.h` y el `README`.
- ML-DSA-65 es un **esquema de firma basado en retículas (lattice) estandarizado por NIST**.
- A diferencia de RSA/ECDSA, que dependen de la factorización de enteros y los logaritmos discretos (vulnerables al algoritmo de Shor en una computadora cuántica), los problemas de retículas se consideran actualmente resistentes a ataques cuánticos en tiempo polinomial.

### 2. Ruta de implementación: Framework CWIST → BoringSSL

- `fly_crypto.c` incluye condicionalmente `<cwist/security/pqc/pqc_sig.h>`.
- El framework CWIST incrusta BoringSSL, que contiene una implementación de ML-DSA en `crypto/mldsa/`.
- Por lo tanto, fly.board **no implementa su propio algoritmo PQC**; simplemente **invoca la API estándar de ML-DSA proporcionada por CWIST/BoringSSL**.

### 3. Payload firmado

- Cuando se crea o actualiza una publicación, se firma la siguiente cadena:
  ```
  title + "\n" + content
  ```
- La firma se almacena en Base64 en la tabla SQLite `posts`, columna `pqc_signature`.
- Al visualizar la publicación, `fly_crypto_verify()` la valida y se muestra una insignia "verified" en la interfaz.

### 4. Gestión de claves

- `fly_crypto_init()` genera **una única clave de firma global** y la mantiene en memoria.
- La clave se genera una vez al iniciar la aplicación y se libera al cerrarse mediante `fly_crypto_cleanup()`.

## Limitaciones y advertencias

### 1. Atestación del servidor, no autenticación del autor

- Hay **una sola clave por servidor**, sin separación de claves por usuario.
- Por lo tanto, la firma no prueba *qué* usuario escribió la publicación; solo prueba que **el servidor atestigua esta publicación**.
- Si el servidor se ve comprometido, un atacante puede usar la clave privada del servidor para falsificar firmas válidas para publicaciones arbitrarias.

### 2. Trampa de la compilación condicional

- Todas las funciones criptográficas en `fly_crypto.c` están envueltas en `#ifdef HAVE_PQC`.
- Si el entorno de compilación carece de `<cwist/security/pqc/pqc_sig.h>`, todas las funciones se convierten en **no-ops (ficticias)**: `fly_crypto_sign()` devuelve `false`, y `fly_crypto_verify()` siempre falla.
- El soporte de PQC **no está garantizado**; depende enteramente de que el módulo CWIST PQC esté presente.

### 3. Alcance limitado de la firma

- Solo se firman `title` y `content`.
- El ID del autor, la marca de tiempo, los metadatos del tablero y los adjuntos **no se incluyen en la carga firmada**.
- Un atacante podría cambiar el campo del autor dejando el cuerpo intacto, y la firma seguiría verificándose.

### 4. La capa de transporte aún usa criptografía clásica

- fly.board utiliza HTTP/3 (QUIC) sobre TLS.
- El handshake TLS y el cifrado de sesión **siguen basados en ECC/RSA**.
- La firma PQC se aplica solo a la *integridad del cuerpo de la publicación*; **no** hace que toda la comunicación cliente-servidor sea segura contra la computación cuántica.

### 5. Sin rotación ni revocación de claves

- La clave global se genera una vez y no se rota hasta el reinicio.
- Un solo compromiso de clave colapsa la confianza en todas las firmas de publicaciones simultáneamente.
- No hay integración con HSM, calendario de rotación de claves ni mecanismo de revocación.

## Conclusión

La firma PQC de fly.board utiliza **ML-DSA-65 estandarizado por NIST para proteger la integridad del cuerpo de la publicación**. Para un motor de blog, este es un experimento inusual y digno de mención: significa que la alteración de publicaciones sigue siendo detectable incluso en la era de la computación cuántica.

Sin embargo, la ausencia de **autenticación asimétrica por autor, protección de metadatos, PQC en la capa de transporte y gestión del ciclo de vida de las claves** hace que la afirmación comercial de ser "resistente a ataques cuánticos" sea una exageración. Es más apropiado considerar esta función como una **demostración de firma PQC** en lugar de una capa de seguridad post-cuántica completamente endurecida.
