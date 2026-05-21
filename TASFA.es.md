# Análisis del protocolo de transferencia de archivos TASFA

## Qué es TASFA

TASFA es el **protocolo de carga/descarga de archivos basado en HTTP/XHR** que utiliza fly.board. No es simplemente un conducto de archivos, sino una capa de protocolo que integra cifrado por fragmentos, verificación de integridad, resiliencia en redes inestables y mitigación de DoS.

## Componentes principales

### 1. Cifrado de flujo AES-256-GCM por fragmentos

- Cada sesión de carga genera una `stream_key` y un `iv_seed` únicos.
- Cada fragmento (chunk) se cifra con `AES-256-GCM`, utilizando `upload_id:chunk_index` como AAD (Additional Authenticated Data).
- Si la verificación del tag falla, el fragmento se descarta inmediatamente.
- El servidor recibe el texto cifrado, lo descifra y escribe el texto plano directamente en un archivo temporal preasignado en el desplazamiento `chunk_index * chunk_size`.
- Este cifrado garantiza **aislamiento de sesiones**: una clave comprometida en una sesión no afecta a las demás. No es un reemplazo de TLS sino una capa adicional de verificación de integridad a nivel de sesión.

### 2. Lattice HTP (Hexagonal Tortoise Problem) de 6 ranuras para reparación local

- HTP **no es un protocolo de transporte**; es una **pista de reparación local**.
- Los fragmentos se agrupan en vértices consecutivos de 6 elementos.
- Los primeros 8 bytes del SHA-512 de cada fragmento se interpretan como un entero de 64 bits `H`, y se calcula `raw_scalar = H % modulus_M`.
- Para un grupo completo deben cumplirse tres invariantes de línea (`L1 == L2 == L3`); si no es así, solo se ajustan `v3` y `v5` para alcanzar el equilibrio con el delta mínimo.
- Al completar la carga, el servidor verifica las invariantes de cada grupo. Una discrepancia devuelve `400 Bad Request`.
- Esto proporciona una pista ligera de detección temprana para **sustitución o corrupción a nivel de fragmento** que un hash de archivo completo no puede revelar hasta el final.

### 3. Cola secuencial a nivel de archivo (estabilidad de conexiones del navegador)

- **Solo se carga un archivo a la vez**.
- Cuando se seleccionan varios archivos, se encolan en `FileUploadQueue`. El siguiente archivo comienza solo después de que el activo tenga éxito o falle.
- Esto respeta el límite del navegador de 6 conexiones concurrentes por origen y previene interbloqueos por agotamiento del pool de conexiones.

### 4. Mapa de bits binario para mitigación de DoS y seguimiento de estado

- El progreso de la carga se almacena en `state.bin` como un **mapa de bits binario denso** (1 byte por fragmento, `'0'` / `'1'`).
- El servidor rechaza cualquier fragmento cuyo índice ya esté marcado como `'1'`. Esto bloquea ataques DoS que reenvían el mismo fragmento para consumir I/O de disco.
- Las actualizaciones usan `pwrite(..., 1, chunk_index)` — una escritura atómica en O(1) **sin análisis JSON en la ruta crítica**.

### 5. Endurecimiento de sesiones

- Los ID de carga y los tokens son cadenas hexadecimales aleatorias de 16 y 24 bytes.
- `flock` previene condiciones de carrera en las actualizaciones del mapa de bits desde solicitudes concurrentes.
- Las sesiones de descarga tienen un TTL (`TASFA_DOWNLOAD_TTL = 86400`) y se descartan cuando expiran.

## ¿Vale la pena sacrificar RPS?

> **Nota**: El compromiso de RPS descrito a continuación aplica **solo a los endpoints de transferencia de archivos** (`/file/upload`, `/file/download`, etc.). El RPS de páginas medido en benchmarks (inicio, lista de tableros, etc.) no se ve afectado por TASFA.

TASFA incurre claramente en una **penalización de RPS (solicitudes por segundo)** en comparación con el servicio simple de archivos estáticos. Sin embargo, esta penalización es un compromiso intencional para obtener los siguientes valores:

| Lo que sacrificas | Lo que ganas |
|-------------------|---------------|
| Sobrecarga adicional de cifrado/descifrado en la ruta crítica | **Integridad por fragmento**: las etiquetas que no coinciden se detectan inmediatamente; los fragmentos corruptos nunca llegan al disco |
| CPU dedicada al cálculo de escalares HTP y validación de grupos | **Detección temprana de corrupción**: errores que un hash de archivo completo solo revelaría al final se capturan a nivel de grupo |
| I/O de archivo del mapa de bits y bloqueos `flock` | **Mitigación de DoS**: bloquea el abuso de reenvío de fragmentos repetidos y ataques de archivos truncados |
| Imposibilidad de cargar varios archivos simultáneamente por la cola secuencial | **Estabilidad de conexiones del navegador**: transferencias estables sin interbloqueos dentro del límite de 6 conexiones por origen |
| Uso de memoria y disco para el estado de sesión | **Eficiencia de reintentos**: tras una interrupción de red, el servidor sabe exactamente qué fragmentos faltan |

### Conclusión

TASFA no persigue "la transferencia de archivos más rápida posible". En cambio, está diseñado para que **los archivos lleguen al destino completos e intactos incluso en redes inestables y con clientes maliciosos**.

- **Perspectiva de carga**: flujo de fragmentos cifrados con aislamiento de sesiones + pistas HTP + defensa DoS mediante mapa de bits + cola secuencial = cargas estables e intactas.
- **Perspectiva de descarga**: sesión basada en handshake + seguimiento de mapa de bits + coalescing adaptativo = descargas ininterrumpidas.

Para un blog simple TASFA puede ser excesivo, pero tiene un mérito técnico claro para un **foro comunitario con muchos archivos adjuntos**, donde la penalización de RPS es un compromiso justificado.
