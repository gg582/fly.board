# Análisis del soporte simultáneo de HTTP/1.1 · HTTP/2 · HTTP/3

## Visión general

fly.board habilita tanto `cwist_app_use_https2()` como `cwist_app_use_https3()`, e inyecta el encabezado `Alt-Svc: h3=":%d"; ma=86400, h2=":%d"; ma=86400` en cada respuesta. Esto crea una arquitectura que **prioriza HTTP/3, retrocede a HTTP/2 cuando no está disponible y, si es necesario, se degrada a HTTP/1.1**.

## Por qué soportar los tres "incluso a costa de rendimiento"

### 1. HTTP/3 (QUIC) — Ideal pero limitado por el entorno

- **Ventajas**: handshake 0-RTT, eliminación completa del bloqueo de cabecera de línea en TCP, migración de conexiones (la sesión persiste al cambiar de red).
- **Costo de rendimiento**: QUIC basado en UDP introduce sobrecarga adicional de cifrado, reensamblado de paquetes y control de congestión, lo que resulta en **mayor uso de CPU que HTTP/2**. Además, la pila BoringSSL + ngtcp2/nghttp3 dentro de CWIST ocupa más memoria y rutas de código que una pila TCP pura de HTTP/2.
- **Por qué mantenerlo**: los navegadores modernos prefieren HTTP/3, pero **los firewalls empresariales, ciertos NAT, proxies heredados y redes de operadoras móviles bloquean o limitan UDP mediante QoS**. Si solo se soportara HTTP/3, los usuarios detrás de tales restricciones no podrían acceder al servicio.

### 2. HTTP/2 (TLS 1.3) — El "máximo común divisor" estable actual

- **Ventajas**: multiplexación sobre una única conexión TCP resuelve el desperdicio de conexiones de HTTP/1.1; server push opcional; compresión de cabeceras (HPACK).
- **Costo de rendimiento**: HTTP/2 tiene una máquina de estados más compleja que HTTP/1.1, consumiendo ciclos de CPU en la priorización de flujos y el control de ventana. Cabe destacar que **los benchmarks de fly.board se midieron sobre HTTP/2** — lo cual implica que HTTP/3 aún no produce resultados predecibles y reproducibles en todas las herramientas de benchmarking y condiciones de red.
- **Por qué mantenerlo**: sirve como **alternativa principal (primary fallback)** cuando HTTP/3 está bloqueado. Además, herramientas de prueba de rendimiento como `h2load` se estandarizan en HTTP/2, y el ecosistema de depuración y monitoreo es mucho más maduro que para HTTP/3.

### 3. HTTP/1.1 — El "último recurso" y la línea base de compatibilidad

- **Ventajas**: Simplicidad. Protocolo basado en texto fácil de depurar; todo balanceador de carga, proxy inverso y herramienta de health check lo soporta perfectamente.
- **Costo de rendimiento**: una sola solicitud-respuesta por conexión (el pipelining es efectivamente un fracaso). A medida que aumenta el número de conexiones, el consumo de memoria y descriptores de archivo explota. **La eficiencia de concurrencia es drásticamente menor** que la multiplexación de HTTP/2.
- **Por qué mantenerlo**:
  - **Clientes heredados**: herramientas CLI antiguas, algunos dispositivos embebidos y rastreadores obsoletos solo entienden HTTP/1.1.
  - **Compatibilidad de infraestructura**: muchos balanceadores de carga y WAF no pueden manejar HTTP/2 de forma nativa a nivel de terminación y recaen a HTTP/1.1.
  - **Health checks y monitoreo**: `curl`, `wget` y sondas basadas en Nagios/Zabbix siguen siendo más estables sobre HTTP/1.1.

## Negociación de prioridad basada en Alt-Svc

```c
// src/handlers/handlers.c
snprintf(altsvc, sizeof(altsvc), 
    "h3=\":%d\"; ma=86400, h2=\":%d\"; ma=86400", 
    g_config.port, g_config.port);
```

- Cuando el cliente establece su primera conexión sobre HTTP/2 (o HTTP/1.1), el servidor anuncia mediante el encabezado de respuesta que HTTP/3 también está disponible en el mismo puerto vía UDP.
- El cliente almacena en caché esta pista (`ma=86400`, 24 horas) y **intenta HTTP/3 en solicitudes posteriores**.
- Si HTTP/3 falla, las bibliotecas del cliente (navegadores, etc.) degradan automáticamente a HTTP/2 → HTTP/1.1.

## Lo que se gana al "sacrificar" rendimiento

| Lo que sacrificas | Lo que ganas |
|-------------------|---------------|
| La latencia teóricamente más baja al ejecutar HTTP/3 exclusivamente | **Accesibilidad global**: el servicio sigue disponible incluso en entornos donde UDP está bloqueado |
| El rendimiento estable de una implementación solo HTTP/2 | **A prueba de futuro**: a medida que HTTP/3 se estandariza, los clientes cambian automáticamente a la ruta óptima |
| La extrema simplicidad y bajo uso de memoria de una implementación solo HTTP/1.1 | **Optimización de transferencia de archivos grandes TASFA**: la multiplexación de HTTP/2 + los streams de HTTP/3 son ventajosos para transferencias por fragmentos |
| Menor complejidad del código del servidor (mantener solo una pila) | **Resiliencia operativa**: si una pila de protocolo tiene un bug, el tráfico se desvía automáticamente a otra |

## Conclusión

Soportar HTTP/1.1, HTTP/2 y HTTP/3 simultáneamente no es una estrategia de "aferrarse a un único protocolo moderno para obtener rendimiento máximo". Por el contrario, es una apuesta pragmática de que **"independientemente del cliente, red o infraestructura, el servicio permanece accesible y, siempre que sea posible, se elige automáticamente el mejor protocolo disponible"**.

Esta elección de diseño se adapta mucho mejor a una **plataforma comunitaria que enfrenta dispositivos terminales y condiciones de red diversas** que a un motor de blog simple. fly.board no está abandonando el alto rendimiento de HTTP/3; está preservando HTTP/2 y HTTP/1.1 como redes de seguridad para los momentos en que HTTP/3 es imposible.
