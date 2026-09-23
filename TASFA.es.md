# TASFA

TASFA es el protocolo de transferencia de archivos utilizado en este proyecto para cargas y descargas.

Se construye sobre HTTP/XHR plano y añade cifrado por fragmentos, una cola secuencial a nivel de archivo, y una **retícula de recuperación HTP (Hexagonal Tortoise Problem) de 6 ranuras con autoridad del servidor** para el control de integridad.

## Rutas

Carga:
- `POST /file/upload/init`
- `POST /file/upload/status`
- `POST /file/upload/renegotiate`
- `POST /file/upload`
- `POST /file/upload/complete`
- `POST /file/upload/cancel`

Descarga:
- `GET /file/download/:id/handshake`
- `GET /file/download/:id/chunk/:chunk_index`
- `GET /file/download/:id`
- `GET /assets/tasfa/img/:filename/handshake`
- `GET /assets/tasfa/img/:filename/chunk/:chunk_index`
- `GET /assets/tasfa/uploads/:filename/handshake`
- `GET /assets/tasfa/uploads/:filename/chunk/:chunk_index`

## Protocolo de Carga

El navegador negocia primero una sesión de carga, luego envía **fragmentos** (predeterminado `16 MiB`, móvil `8 MiB`) con encabezados TASFA:

- `X-TASFA-Upload-ID`
- `X-TASFA-Upload-Token`
- `X-TASFA-Chunk-Index`
- `X-TASFA-Hash-Tag`
- `X-TASFA-Raw-Scalar`
- `X-TASFA-Magic-Scalar`

El servidor escribe cada fragmento directamente en el archivo temporal preasignado en el desplazamiento `chunk_index * chunk_size`. Ya no hay bloques de transporte.

Si un fragmento normal falla repetidamente, el navegador envía ese fragmento fallido a través de una solicitud de respaldo AES-256-GCM con `X-TASFA-Stream-Mode: aes-256-gcm`. El servidor acepta el texto cifrado más la etiqueta de autenticación GCM, y los fragmentos de respaldo permanecen dentro de la ventana paralela adaptativa. La solicitud de respaldo aún lleva los mismos encabezados de etiqueta hash HTP y escalar equilibrado.

### Fragmentos de paridad XOR

Las sesiones de carga llevan un **fragmento de paridad adicional por cada grupo de hasta 6 fragmentos de datos**: `chunk_count = data_chunks + ceil(data_chunks / 6)`. El fragmento de paridad contiene `P = C_0 xor ... xor C_n` de su grupo y se almacena por separado como `parity_<k>.bin` en el directorio de la sesión en lugar de hacerlo en `chunk_index * chunk_size`. Los fragmentos de paridad cuentan para el bitmap de fragmentos recibidos pero se excluyen de la validación de líneas HTP; alimentan la recuperación por pelado del lado del servidor descrita a continuación.

### Resto (último fragmento parcial)

Si el tamaño del archivo no es múltiplo del tamaño del fragmento, el último fragmento es un **resto**. Se envía como un único blob con su rango de bytes exacto; el servidor lo escribe en el desplazamiento correcto. No se realiza relleno ni división.

### Respuesta de inicio de sesión

El endpoint `init` devuelve, entre otros campos:

- `chunk_size` — tamaño de fragmento negociado
- `current_parallel_chunks` — ventana activa de carga aprobada por el servidor
- `max_parallel_chunks` — cuántos fragmentos el cliente puede cargar concurrentemente
- `dispatch_pacing_ms` — pequeño retardo de envío aplicado solo cuando el enlace medido es deficiente
- `upload_secret` — secreto secundario del servidor para verificación de flujo cifrado
- `stream_key_hex`, `stream_iv_seed_hex`, `stream_mode` — claves de cifrado de sesión (`aes-256-gcm`)
- `modulus_M` — módulo HTP utilizado para esta sesión
- `group_count` — número de grupos HTP completos de 6 ranuras
- `client_stripes` — valor fijo `32`, utilizado por el planificador de trabajadores del cliente

El cliente trata a `chunk_size` como un contrato de sesión porque define el desplazamiento de cada fragmento en el archivo. Durante una carga activa ajusta la concurrencia y el comportamiento de reintento, mientras que la pista aprendida de tamaño de fragmento se aplica a la siguiente sesión TASFA: después de una racha rápida sostenida (3 fragmentos limpios con EWMA `>= 8 Mbps` o tiempos de fragmento inferiores a 30 segundos) la siguiente pista se duplica, y después de fallos se reduce una cuarta parte (`* 0.75`, redondeada a pasos de `1 MiB`, con piso de `8 MiB`).

### Respuesta de fragmento de carga

Cuando se acepta un fragmento, el servidor responde con `204 No Content` y encabezados:

- `X-TASFA-Accepted: 1`
- `X-TASFA-Chunk-Complete: 1`

Si el índice del fragmento ya está marcado como completo en `state.bin` **y** el índice no está en los `retry_targets` del servidor, el servidor devuelve `204` con los mismos encabezados pero el cuerpo del fragmento se descarta.

## Retícula de Recuperación HTP con Autoridad del Servidor

HTP **no es un protocolo de transporte ni una prueba criptográfica**. Es un motor de sospecha de fragmentos del lado del servidor que clasifica los fragmentos por probabilidad de corrupción para que el cliente solo retransmita los sospechosos de alta probabilidad en lugar del archivo completo.

### Agrupación de fragmentos

Los fragmentos se agrupan en vértices consecutivos de 6 elementos:

```
Grupo g: [ v0 , v1 , v2 , v3 , v4 , v5 ]
           chunk g*6+0  ...  g*6+5
```

El último grupo puede ser parcial; **los grupos incompletos nunca se rellenan con ceros y se excluyen completamente de la validación HTP**. El relleno con ceros inyectaría topología sintética y está prohibido.

### Escalar en bruto

Para cada fragmento, el cliente calcula el SHA-512 del fragmento en texto plano, toma los primeros 8 bytes como un entero de 64 bits sin signo big-endian `H`, y deriva:

```
raw_scalar = H % modulus_M
```

### Invariante de línea mágica

Para un grupo completo, se definen tres líneas:

```
L1 = v0 + v1 + v2   (mod M)
L2 = v2 + v3 + v4   (mod M)
L3 = v4 + v5 + v0   (mod M)
```

El invariante requiere `L1 == L2 == L3`. Si los escalares en bruto no lo satisfacen, el cliente **equilibra** ajustando solo `v3` y `v5`:

```
delta2 = (L1 - L2) mod M
delta3 = (L1 - L3) mod M

v3_balanced = (v3_raw + delta2) mod M
v5_balanced = (v5_raw + delta3) mod M
```

Todos los demás vértices mantienen su escalar en bruto. El cliente envía ambos:

- `X-TASFA-Raw-Scalar` — el `raw_scalar` sin modificar
- `X-TASFA-Magic-Scalar` — el valor equilibrado (`v3_balanced` o `v5_balanced`; en otros casos, igual que raw)

El servidor almacena ambos escalares por separado en `htp.bin`, de modo que el análisis puede referenciar la topología original independientemente de las restricciones de equilibrio artificiales.

### ¿Por qué solo v3 y v5?

La retícula hexagonal tiene dos grados de libertad. Fijando `v0,v1,v2,v4` y ajustando `v3,v5` se satisfacen únicamente las tres ecuaciones de línea manteniendo el delta mínimo y local al grupo.

## Recuperación HTP con Autoridad del Servidor

**El cliente es un agente de retransmisión tonto.** No calcula álgebra de reparación, no evalúa umbrales de costo, y no deriva clasificaciones de sospecha. Todo eso vive únicamente en el servidor.

### Flujo de validación del servidor

Durante `POST /file/upload/complete`, el servidor:

1. Carga todos los registros por fragmento desde `htp.bin` (etiqueta hash, escalar en bruto y escalar equilibrado).
2. Valida solo **grupos completos de 6 ranuras** (los grupos parciales se omiten).
3. Si el orden escalar dentro de un grupo de 6 ranuras está permutado (fuera de orden), el servidor busca las 720 permutaciones, encuentra una que restaure el invariante, y reordena los registros `htp.bin` en consecuencia sin requerir retransmisión.
4. Para cada grupo fallido, computa **puntajes de sospecha** por ranura analizando en qué ecuaciones de línea participa cada ranura.

### Puntuación de confianza de sospecha (por grupo)

Para un grupo fallido, el servidor evalúa cada ranura contra las tres ecuaciones de línea:

| Ranura | Ecuaciones |
|--------|-----------|
| v0     | L1, L3    |
| v1     | L1        |
| v2     | L1, L2    |
| v3     | L2        |
| v4     | L2, L3    |
| v5     | L3        |

La puntuación de sospecha de cada ranura se deriva del análisis de síndrome topológico:
- Cuando exactamente dos líneas coinciden y una difiere ($L_a == L_b \neq L_c$), la ecuación fallida aísla dos candidatos: la ranura del lado exclusivo en $L_c$ (puntaje `1.0`, síndrome dominante) y la ranura de la esquina opuesta compartida por $L_a, L_b$ (puntaje `0.5`).
- Las 4 ranuras restantes que no tocan la ecuación fallida se **eliminan** inmediatamente (puntaje `0.0`).
- Cuando las tres líneas difieren ($L_1 \neq L_2 \neq L_3$), los puntajes son proporcionales a la participación en las magnitudes residuales normalizadas $|L_i - L_j|$.

Los puntajes se agregan entre todos los grupos fallidos; si un fragmento aparece en múltiples grupos, se conserva su puntaje máximo.

### Decodificador de Pelado HTP-XOR Completo

Para resolver la corrupción o pérdida de fragmentos sin costosas inversiones de matrices en campos de Galois $GF(2^8)$, TASFA combina la localización de síndrome topológico con un decodificador de pelado XOR iterativo:
- **Generación de Paridad XOR**: Cada grupo de hasta 6 fragmentos de datos tiene un fragmento de paridad $P = C_0 \oplus C_1 \oplus C_2 \oplus C_3 \oplus C_4 \oplus C_5$ cargado con la sesión.
- **Localización de Sospechosos sin Coste**: Cuando un grupo falla en las comprobaciones de integridad, las sumas de línea de HTP señalan el índice del único fragmento corrompido $i$ con operaciones de enteros en $O(1)$.
- **Recuperación por Pelado Iterativo Acotado**: El servidor reconstruye $C_i = P \oplus \bigoplus_{j \neq i} C_j$ utilizando XOR a nivel de bits con SIMD. Un grupo se recupera cuando tiene exactamente un sospechoso, o dos sospechosos donde el puntaje máximo es `1.0` (síndrome dominante de error único); todos los sospechosos de un grupo recuperado se eliminan entonces. El escaneo se repite hasta que ningún grupo progresa, con un máximo de 6 pasadas.
- **Reconstrucción de Fragmentos Faltantes**: Independientemente del análisis de sospecha, si el bitmap de finalización muestra exactamente un fragmento de datos faltante en un grupo y el fragmento de paridad de ese grupo ha sido recibido, el servidor reconstruye el fragmento faltante de la misma forma antes de validar las líneas HTP.
- **Alternativa para Borrados Múltiples**: Si dos o más fragmentos independientes del mismo grupo no pueden aislarse, el servidor recurre a la retransmisión ARQ a través de `retry_targets`.

### Umbral de costo de reparación

Antes de solicitar cualquier reparación, el servidor evalúa si la contracción es más barata que el reintento directo:

```
repair_worthwhile(sospechosos, total, tamaño_chunk, rtt_ms):
    if sospechosos < 12               → false  (muy pocos para topología)
    retry_cost  = sospechosos * tamaño_chunk * rtt_factor(rtt_ms)
    repair_cost = bytes_metadata + cpu_servidor + costo_rtt_extra
    return retry_cost > repair_cost
```

El modelo de costo abstracto compara los bytes que el cliente retransmitiría contra la sobrecarga de análisis del servidor. Chunks grandes o alta latencia hacen la contracción más atractiva, mientras que muchos sospechosos pequeños hacen el reintento directo más barato. Los números concretos son detalles de implementación del servidor, no constantes del protocolo.

Si el umbral rechaza la reparación, el servidor devuelve `needs_retry` con **todos** los fragmentos sospechosos como objetivos de reintento. El cliente los retransmite a través del endpoint de carga normal.

### Contracción recursiva del lado del servidor

Si la reparación es viable, el servidor ejecuta una **contracción a nivel de grupo**: cada grupo original completo de 6 ranuras se colapsa a un único escalar que codifica su **signature invariante**. El servidor calcula los tres valores de línea `L1, L2, L3` del grupo, deriva los residuales `r12 = (L1-L2) mod M` y `r23 = (L2-L3) mod M`, y fija el agregado del grupo a `(r12 * r23) mod M`. Un grupo que pasa tiene `r12 = r23 = 0`, por lo que su agregado es `0`; un grupo fallido recibe una signature determinística no nula que preserva la topología de las inconsistencias de línea. Estos agregados de grupo se convierten en los vértices de un lattice HTP de nivel superior. Grupos consecutivos de 6 de estos agregados forman super-grupos de nivel 1, y se reevalúan los mismos invariantes de línea:

- Si un super-grupo de nivel 1 pasa, se eliminan los sospechosos de sus grupos de nivel 0 subyacentes (el patrón de fallo es consistente a nivel de grupo).
- Si un super-grupo de nivel 1 falla, se conservan los sospechosos de sus grupos de nivel 0 subyacentes.
- Si la contracción reduce el conjunto sospechoso (menos fragmentos), el servidor almacena los objetivos reducidos y devuelve `needs_retry` con la lista reducida.
- Si la contracción no reduce el conjunto, el servidor recae al reintento directo de los sospechosos originales.
- El nivel de contracción se incrementa en los metadatos de sesión para que el cliente pueda reportar diagnósticos.

El cliente nunca ve grupos de contracción ni los computa. Solo recibe `retry_targets`.

### Aceptación de retransmisión

Cuando el cliente retransmite un fragmento ya marcado como recibido, el endpoint de carga normal **acepta la retransmisión solo si ese índice de fragmento está actualmente en la lista `retry_targets` del servidor**. Después de almacenar el fragmento retransmitido, el servidor lo elimina de `retry_targets`.

### Respuesta de reparación visible al protocolo

Cuando HTP falla y el servidor decide reparación o reintento, el endpoint `complete` devuelve `409` con:

- `htp_status`: `"needs_retry"`
- `retry_targets`: array de índices de fragmentos a retransmitir (ordenados por puntaje de sospecha descendente)
- `suspicion_scores`: array de objetos `{chunk_index, score}`
- `contraction_level`: cuántas pasadas de contracción del lado del servidor se han aplicado
- `retry_reason`: explicación legible (ej. `"htp group inconsistency detected"`)

Si el umbral de costo dice que el reintento directo es más barato, `retry_targets` contiene la lista completa de sospechosos y `contraction_level` permanece en `0`.

Si el servidor reduce exitosamente los sospechosos mediante contracción, `retry_targets` contiene la lista reducida y `contraction_level` se incrementa.

Después de que todos los sospechosos se retransmiten y validan con éxito, la siguiente llamada `complete` procede a la finalización SHA-256.

## Cola Secuencial a Nivel de Archivo

**Solo se carga un archivo a la vez.** Cuando se seleccionan múltiples archivos:

1. Cada archivo obtiene su propio recurso, tarjeta de vista previa, y sesión HTP.
2. Los archivos se encolan en `FileUploadQueue`.
3. Cuando el archivo activo termina (éxito o fallo), la cola avanza automáticamente al siguiente archivo.
4. El botón de lote "Cargar archivos en cola" siempre está habilitado; al hacer clic encola todos los archivos pendientes e inicia la bomba.

Esto evita el agotamiento del pool de conexiones del navegador y mantiene confiable la detección de bloqueos.

## Configuración en Tiempo de Ejecución

- tamaño de fragmento de carga: `16 MiB` escritorio, `8 MiB` móvil
- pista adaptativa de tamaño de fragmento de carga: mínimo `8 MiB`, hasta `32 MiB` escritorio / `16 MiB` móvil en la negociación del servidor (la preferencia del cliente puede llegar a `64 MiB` / `32 MiB`); el éxito duplica la pista después de una racha rápida, el fracaso la reduce por `* 0.75` en pasos de `1 MiB`
- tamaño de fragmento de descarga: `8 MiB` escritorio, `6 MiB` móvil, hasta `32 MiB` cuando el cliente indica una sesión mayor
- paralelismo de carga del navegador predeterminado: `8`, éxito `*1.15` (`*1.3` durante una ventana de recuperación rápida), fracaso `*0.85`
- paralelismo máximo de carga del navegador: `max_upload_parallel_chunks` en `blog.settings`, límite `40`
- sesiones de carga concurrentes máximas: `max_total_parallel_uploads` en `blog.settings`, límite `512`
- tamaño máximo de carga: `max_upload_size` en `blog.settings`
- sesiones de descarga máximas del navegador: definido por el servidor, hasta `48` solicitudes de fragmento por sesión
- coalescencia de descarga (tamaño de grupo span): éxito `*1.2`, fracaso `*0.8`; el perfil del servidor sugiere hasta `16` fragmentos, y el span del cliente puede crecer hasta `64` fragmentos con un límite de `128 MiB` por solicitud
- vistas previas de imagen: solicitan un bucket de tamaño de 128px consciente del DPR (mínimo `256px`, máximo `1920px`) para que las transferencias de imagen no se inflen a una vista previa de tamaño escritorio cuando se renderizan pequeñas
- reproducción de vídeo/audio: precalienta solo el handshake de la sesión TASFA ante la intención de puntero/foco, y luego conserva la reproducción nativa por solicitudes de rango; no se obtiene ningún contenido multimedia anticipadamente y el planificador progresivo permanece sin cambios
- tiempo de espera xhr de carga: watchdog de inactividad adaptativo escalado por el tamaño de la transferencia, `30-120 s`
- tiempo de espera fetch de sesión de carga: `120 s`

TASFA está ajustado para despliegues de servidores de alto ancho de banda generales, no para un perfil integrado/aeroespacial de bajo ancho de banda. El límite de conexiones HTTP por origen del navegador se respeta naturalmente por el pool de workers.

### Adaptación del Cliente

El cliente de carga mide el tiempo de finalización del fragmento, reintentos, tiempos de espera, y pistas de la API de Información de Red cuando están disponibles. Envía esas entradas a `/file/upload/init` y `/file/upload/renegotiate`; el servidor responde con una ventana paralela actual y una ventana máxima. Las finalizaciones limpias aumentan la ventana activa por `*1.15` hasta el máximo negociado (`*1.3` dentro de una ventana de recuperación rápida de 5 segundos justo después de una caída), mientras que los patrones de fallo repetidos y predecibles la reducen por `*0.85` hasta un piso de `4` móvil / `8` escritorio en enlaces buenos. El respaldo AES-GCM también se ejecuta dentro de la ventana adaptativa.

Un tiempo de espera absoluto por fragmento previene bloqueos causados por conexiones que reciben datos parciales pero nunca cierran. Un watchdog muestrea la actividad de carga cada 2 segundos: ante un bloqueo suave (`30 s` sin progreso de fragmentos) realiza una **recuperación suave** — renegociación, un incremento de paralelismo y una ventana de recuperación rápida de 5 segundos — sin desarmar las conexiones vivas; ante un bloqueo duro (`90 s`) realiza un **reinicio agresivo**: los XHR en vuelo se abortan y la carga se reanuda desde el bitmap del servidor, renegociando desde la ventana completa `maxParallel` (los perfiles de transferencia de alto rendimiento también restablecen `targetParallel` a `maxParallel` y `dispatchPacingMs` a `0`). La pista aprendida del tamaño de fragmento se mantiene **local a la sesión** y no se persiste inmediatamente en `localStorage`; solo se aplica a la siguiente sesión TASFA.

La descarga usa el mismo sesgo de alto rendimiento: el handshake lleva la pista de tamaño de fragmento preferida del cliente, y las descargas activas aumentan el `span` y el paralelismo por `*1.2` después de grupos de fragmentos exitosos. En caso de fallo ambos se reducen por `*0.8`. Las lecturas cortas, tiempos de espera y errores de red se reencolan por índice de fragmento; un grupo fallido no falla toda la descarga hasta que el mismo fragmento haya agotado un alto presupuesto de reintentos.

Para la reproducción progresiva de vídeo/audio, TASFA es más estricto: el cliente ensambla y alimenta los fragmentos secuencialmente, mientras un planificador acotado mantiene el paralelismo adaptativo solo dentro de una ventana hacia adelante desde el siguiente fragmento que el reproductor necesita. Los fragmentos fallidos o con tiempo de espera agotado registran una hora de reintento y liberan el slot de red, de modo que el planificador puede seguir llenando otros fragmentos elegibles en la misma ventana. La reproducción puede detenerse en el borde del búfer, pero nunca omite un rango de bytes, deja un fragmento faltante sin resolver, ni cierra el flujo multimedia anticipadamente.

### Predicción de RTT de Cola (Extrapolación de Lagrange)

Para archivos grandes (sesiones con muchos fragmentos), el servidor acumula muestras de RTT por fragmento reportadas por el cliente, hasta un máximo de 8. Una vez que se han recopilado 3 o más muestras, una extrapolación polinomial de Lagrange estima el RTT en el índice del último fragmento, y se multiplica la cantidad de fragmentos restantes para derivar el tiempo estimado restante.

- Carga: después de que cada fragmento termina, el cliente puede adjuntar un encabezado `X-TASFA-Chunk-RTT` (milisegundos) a la siguiente solicitud de fragmento.
- Descarga: el cliente puede reportar por separado el RTT del fragmento anterior a través del parámetro de consulta `chunk_rtt_ms`.

El servidor agrega `predicted_remaining_ms` a la respuesta JSON de estado, y la respuesta de fragmento de descarga lleva un encabezado `X-TASFA-Predicted-Remaining-Ms`. Las matemáticas se mantienen intencionalmente ligeras: límite de muestras de 8, Lagrange simple O(n²), y las predicciones se recortan con un techo de 30 segundos.

### Guardia Predictiva Acelerada de Wynn

TASFA no predice bytes de texto plano ni confía en contenidos de fragmentos predichos. La predicción se limita a la secuencia de calidad de transferencia observada por el cliente: tiempo de finalización, rendimiento, reintentos, tiempos de espera y lecturas cortas. El cliente aplica la transformación epsilon de Wynn en su forma Aitken `Delta^2` a las últimas muestras de calidad:

```
S_hat = S0 - ((S1 - S0)^2 / (S2 - 2*S1 + S0))
```

`S_hat` se limita a `[0, 1]` y se usa solo como señal de guardia para el siguiente fragmento. Una calidad predicha alta mantiene la ruta normal y permite que la ventana adaptativa crezca. Una calidad predicha media-baja marca el siguiente fragmento como `guarded`, reduciendo la ventana paralela activa en uno antes del despacho. Una calidad predicha muy baja marca el siguiente fragmento de carga para el fallback AES-GCM paralelo, o reduce el `span` y el paralelismo de descarga antes de emitir el siguiente grupo de rangos. Esta es una defensa de congestión y fallo prospectiva, no un oráculo criptográfico.

## Modelo de Almacenamiento

Cada sesión de carga preasigna un archivo temporal:

- archivo temporal: `data/tasfa/uploads/<upload_id>/upload.bin.part`
- metadatos: `data/tasfa/uploads/<upload_id>/meta.json`
- meta binaria rápida: `data/tasfa/uploads/<upload_id>/meta.bin`
- estado: `data/tasfa/uploads/<upload_id>/state.json`, `data/tasfa/uploads/<upload_id>/state.bin`
- HTP nivel 0: `data/tasfa/uploads/<upload_id>/htp.bin`
- bloqueo de sesión: `data/tasfa/uploads/<upload_id>/session.lock`

El servidor ya no mantiene `blocks.bin` ni `chunk_counts.bin`. La finalización de un fragmento es una sola escritura de bitmap.

### Almacenamiento de Objetos Compatible con S3 (Opcional)

Los archivos completados pueden almacenarse adicionalmente en un bucket compatible con S3 configurado a través del archivo opcional `s3.settings` (todo vacío significa deshabilitado y el almacenamiento en disco local no cambia). En `mode=mirror` (predeterminado) el archivo local se conserva y también se sube (PUT) al bucket; las descargas siguen sirviéndose desde disco. En `mode=offload` los bytes se mueven a S3 después de la carga, la fila del archivo se marca como `s3://<key>`, y las descargas se responden con una redirección `302` a una URL GET prefirmada de 1 hora. Los PUT fallidos a S3 recurren al almacenamiento local, y todas las rutas de eliminación limpian ambas copias. Consulta `S3.md` para la referencia completa de claves.

Los metadatos de sesión también almacenan arrays por vértice:

- `hash_tags` — array de cadenas hexadecimales SHA-512, uno por fragmento
- `magic_scalars` — array de escalares equilibrados, uno por fragmento
- `htp_retry_targets` — lista de objetivos de reintento emitida actualmente por el servidor
- `htp_suspicion_scores` — clasificación de sospecha actual
- `htp_contraction_level` — número de pasadas de contracción del lado del servidor aplicadas

Estos se actualizan en cada carga de fragmento y se validan al completar.

## PIN de Eliminación

Las cargas completadas reciben un PIN de eliminación de un solo uso. El PIN en claro es una cadena hexadecimal de 12 caracteres (6 bytes aleatorios); se devuelve una vez; solo se almacena su hash.

## Protocolo de Descarga

1. El cliente solicita un handshake a través de `GET /.../handshake`.
2. El servidor devuelve `session_id`, `session_token`, `chunk_size`, `chunk_count`, sugerencias de concurrencia y claves de cifrado de sesión (`stream_key_hex`, `stream_iv_seed_hex`).
3. El cliente obtiene grupos de fragmentos con un `span=...` adaptativo. Cuando hay claves de sesión, todos los fragmentos están cifrados con **AES-256-GCM**.
4. El cliente descifra los fragmentos en el navegador utilizando la **Web Crypto API** y las claves de sesión.
5. El navegador ensambla la respuesta en un búfer contiguo.
6. **Compresión de Carga Útil (Respaldo Zstd/Brotli/Gzip)**:
   Durante las bajadas, el cliente anuncia los algoritmos de descompresión soportados mediante el encabezado `X-TASFA-Accept-Encoding` (ej. `br, zstd, gzip`; el servidor recurre a `Accept-Encoding` cuando falta el encabezado TASFA). El encabezado es una lista de capacidades, no un orden de prioridad: el servidor aplica la compresión **antes** del cifrado GCM usando exactamente una pasada de búfer completo del códec soportado que más prefiere, ordenado **Zstd (nivel 1) → Brotli (calidad 1) → gzip (best speed)**; es un orden de preferencia, no una cadena de respaldo — si el códec seleccionado no ahorra suficientes bytes, se sirve la carga útil en bruto y no se prueba ningún otro códec. Esto evita la doble compresión y garantiza que los clientes solo reciban cargas útiles que pueden descomprimir de forma nativa (usando `DecompressionStream`). Si la compresión no produce ganancias suficientes (`TASFA_COMPRESS_MIN_GAIN_BYTES`, 1,024 bytes; se requiere estrictamente menos), el servidor omite la compresión y sirve la carga útil binaria en bruto directamente. Las respuestas de fragmento comprimidas llevan `X-TASFA-Content-Encoding` y `X-TASFA-Uncompressed-Length` para que el cliente pueda descomprimir antes del descifrado.

## Cifrado de Bloques en Sandbox WASM (Opcional)

El cifrado de bloques AES-256-GCM (cifrado/descifrado de fragmentos) normalmente se ejecuta a través de la ruta nativa OpenSSL EVP. Cuando la variable de entorno `TASFA_CRYPTO_WASM` apunta a un módulo WASI compilado (`make wasm-tasfa-crypto` produce `build-wasm/tasfa_crypto.wasm`), el servidor en su lugar ejecuta cada operación de bloque dentro de un sandbox: lanza `wasmtime run <módulo>` como subproceso e intercambia una solicitud/respuesta emmarcada (protocolo en `wasm/tasfa_crypto_protocol.h`) por stdin/stdout. El módulo realiza la misma derivación de IV y disposición de AAD que la ruta nativa, y la paridad de bytes con OpenSSL EVP se garantiza con `wasm/test_parity.py`. El backend es estrictamente opt-in: si `TASFA_CRYPTO_WASM` no está definida o el módulo es ilegible, `tasfa_wasm_crypto_available()` devuelve falso y se usa la ruta nativa. El binario `wasmtime` puede anularse con `TASFA_WASMTIME`. Esto cambia solo dónde se ejecuta el cripto de fragmentos, nunca el formato en el cable.

## Mitigación DoS vía Bitmap

Tanto el estado de carga como el de descarga se rastrean con un **bitmap binario denso** (un byte por fragmento, `'0'` / `'1'`).

### Lado de carga

- El servidor rechaza cualquier fragmento cuyo índice ya esté marcado `'1'` en `state.bin`, **a menos que el fragmento esté explícitamente en la lista de objetivos de reintento del servidor**. Un atacante no puede reproducir fragmentos arbitrarios para consumir I/O de disco.
- `state.bin` se actualiza con `pwrite(..., 1, chunk_index)` — una escritura atómica en O(1). No hay análisis JSON en la ruta crítica.
- El manejador `complete` reabre el bitmap, cuenta los bits establecidos, y se niega a finalizar hasta que `received_chunks == chunk_count`. Esto previene ataques de archivos truncados.

### Endurecimiento de sesión

- Los ID de carga y tokens son cadenas hexadecimales aleatorias de 16 y 24 bytes.
- Los bloqueos de sesión (`flock` en `session.lock`) previenen actualizaciones de bitmap competidoras desde solicitudes concurrentes.

## Planificador de Trabajadores

El servidor usa un planificador de trabajadores de round-robin fijo. El número de trabajadores es igual al recuento de CPU (mínimo 2, máximo 64). Los fragmentos que pertenecen al mismo `upload_id` siempre se despachan al mismo trabajador para la localidad de caché y el ordenamiento. Los trabajos se envían a través de una cola por trabajador y se señalan con variables de condición.

## Finalización Asíncrona

`POST /file/upload/complete` se procesa de forma asíncrona. La primera llamada devuelve `202 Accepted` con `{"processing": true}` y programa un trabajador de finalización en segundo plano en el pool del motor, en un dominio de planificación aislado `TTAK_TASK_DOMAIN_THREAD` para que el trabajo de finalización (transcodificaciones de medios, suma de verificación SHA-256, escrituras de base de datos) nunca comparta trabajadores con los bucles del servicio HTTP. Si el pool rechaza el trabajo (p. ej., está apagándose), el registro de finalización se marca como `503` y el cliente reintenta en lugar de inmovilizar un trabajador HTTP. Las llamadas posteriores sondean la caché de finalización; cuando el trabajador termina, el estado y el cuerpo en caché se devuelven inmediatamente. Esto evita que el trabajo de finalización de larga duración bloquee la conexión HTTP.

## Lista de Verificación de Autorrevisión

| Pregunta | Respuesta |
|----------|-----------|
| Q1: ¿El cliente calcula algún álgebra de reparación? | **No.** El cliente es un agente de retransmisión tonto. Toda la derivación de sospechas, puntuación de confianza, umbrales de costo y lógica de contracción son exclusivamente del lado del servidor. |
| Q2: ¿Se evalúa explícitamente el umbral de costo de reparación antes de la contracción? | **Sí.** `htp_repair_worthwhile` compara `retry_cost_estimate` (bytes × RTT) contra `repair_cost_estimate` (sobrecarga de análisis del servidor). Rechaza la reparación cuando `suspect_count < 12` o el reintento es más barato, volviendo al reintento directo. |
| Q3: ¿Los grupos parciales se rellenan con ceros? | **No.** Solo los grupos completos de 6 ranuras (`data_chunks / 6`) se validan. El grupo final incompleto se excluye por completo. |
| Q4: ¿La respuesta contiene puntajes de sospecha, no solo banderas binarias? | **Sí.** Cada respuesta `needs_retry` incluye `suspicion_scores` como objetos `{chunk_index, score}`. |
| Q5: ¿La contracción preserva la topología de grupo original? | **Sí.** `htp_contract_groups` trata cada grupo original completo como un único vértice de nivel superior; los sospechosos nunca se reordenan entre grupos. |
| Q6: ¿Se borran los objetivos de reintento al retransmitir exitosamente? | **Sí.** `handler_file_upload` elimina el fragmento de `htp_retry_targets` después de aceptar una retransmisión de reintento. |
| Q7: ¿El servidor repara escalares fuera de orden intercambiándolos? | **Sí.** `htp_try_swap_repair` fuerza bruta las 720 permutaciones de un grupo de 6 ranuras; si alguna restaura el invariante, los registros `htp.bin` se reordenan inmediatamente y el grupo pasa sin retransmisión. |
