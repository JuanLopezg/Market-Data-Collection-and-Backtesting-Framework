# T1 — Audit de tiempo actual

Estado: **AUDITADO sobre los ZIP entregados el 2026-09-20. Sin cambios de producción.**

## Regla de clasificación

- **BUSINESS TIME**: cualquier `now` que cambie qué candle/día es visible, qué fecha económica se procesa, gating diario, timestamps económicos, fills/eventos simulados o decisiones.
- **BUSINESS SLEEP**: una espera cuyo objetivo es alcanzar un instante/intervalo económico. Debe terminar usando `TimeHandler::sleep(...)` / `sleepUntil(...)`, escalada por `speed`.
- **TECHNICAL TIME**: timeout de red/NATS/PostgreSQL, backoff, polling, watchdog, duración de CPU, logging/diagnóstico, identidad técnica. Debe seguir en tiempo real/monotónico y **no** escalarse con `speed`.
- **LEGACY CLOCK**: infraestructura del shared logical clock anterior que se retirará cuando el replay nuevo ya no dependa de ella.

## Hallazgos del runtime LIVE

| Zona | Uso actual | Clasificación | Acción futura |
|---|---|---|---|
| `market_data_service_main.cpp` | `getCurrentUtcDate(system_clock::now())` para previous completed UTC day | BUSINESS | Sustituir el `now` por `TimeHandler::getTime()` |
| `market_data_service_main.cpp` | espera hasta midnight + delay | BUSINESS SLEEP | `TimeHandler::sleepUntil(...)` |
| `market_data_service_main.cpp` | retry tras fallo | TECHNICAL | Separarlo de la espera económica y mantener real/monotónico |
| `binance_market_data_client.cpp` | HTTP retry/backoff `sleep_for` | TECHNICAL | Mantener real |
| `strategy_service_main.cpp` | `newestCompletedUtcDate()` desde host UTC | BUSINESS | Usar `TimeHandler` |
| `strategy_service_main.cpp` | espera hasta próximo UTC midnight | BUSINESS SLEEP | Usar `TimeHandler::sleepUntil(...)` |
| `strategy_service_main.cpp` | loop cadence con `steady_clock` | TECHNICAL | Mantener real |
| `portfolio_risk_service_main.cpp` | `newestCompletedUtcDate()` | BUSINESS | Usar `TimeHandler` |
| `portfolio_risk_service_main.cpp` | espera al próximo día | BUSINESS SLEEP | Usar `TimeHandler::sleepUntil(...)` |
| `portfolio_risk_service_main.cpp` | poll/cadence con `steady_clock` | TECHNICAL | Mantener real |
| `order_planner_service_main.cpp` | `newestCompletedUtcDate()` | BUSINESS | Usar `TimeHandler` |
| `order_planner_service_main.cpp` | espera al próximo día | BUSINESS SLEEP | Usar `TimeHandler::sleepUntil(...)` |
| `order_planner_service_main.cpp` | poll/cadence con `steady_clock` | TECHNICAL | Mantener real |
| `execution_state_service_main.cpp` | cálculo del newest completed day | BUSINESS | Usar `TimeHandler` |
| `execution_state_service_main.cpp` | nonce de snapshot request con `system_clock` | TECHNICAL IDENTITY | No pasar por TimeHandler; preferible monotonic/counter si se cambia |
| `execution_state_service_main.cpp` | event loop con `steady_clock` + `sleep_for` | TECHNICAL | Mantener real |
| `common_types/scheduler.h` | intervalos/timeouts internos | TECHNICAL | Mantener real salvo que un caller demuestre semántica económica |
| `common_types/config_handler.h` | file modification timestamps | TECHNICAL | Mantener host/filesystem time |
| `database_utils.cpp` | timestamp para utilidad/archivo | TECHNICAL | Mantener host time |

## `time_utils`: frontera a limpiar

Los overloads que reciben explícitamente un `time_point` son helpers puros y pueden mantenerse. Los overloads sin argumentos que llaman internamente a `system_clock::now()` (`getCurrentUtcDate()`, `computeNextMidnightUTC()`, etc.) son peligrosos para business logic porque ocultan el reloj del host. Durante T6–T12 los servicios económicos deberán recibir el tiempo desde `TimeHandler` y llamar a los helpers parametrizados.

`nowString()` / formatting de logs no debe convertirse en business time salvo que se use explícitamente como timestamp económico.

## Clock antiguo localizado

La infraestructura antigua sigue activa en el árbol entregado:

- `lib/src/runtime/clock.h`: `Clock`, `SystemClock`, `FixedClock`, `SimulatedClock`.
- `lib/src/runtime/service_clock.h`: `ServiceClockContext`, sync/guard/poll y consumo de ClockState.
- `lib/src/runtime/runtime_mode.h`: branching LIVE/TESTNET/REPLAY asociado actualmente al clock legacy.
- `lib/src/contracts/clock_state.h`.
- `lib/src/contracts/clock_control.h`.
- `lib/src/contracts/clock_sync_request.h`.
- codecs de esos contratos en `lib/src/transport/contract_json_codec.{h,cpp}`.
- subjects `simulation.clock.*` y `runtimeSubjects()` en `lib/src/transport/transport_subjects.h`.
- `live_trading/replay_controller`: autoridad temporal antigua.
- `live_trading/simulated_exchange_service`: todavía usa `ServiceClockContext` y `runtime_mode/simulation_id`.
- `deploy/distributed_replay`: todavía arranca la topología legacy con `--runtime-mode`, `--simulation-id` y replay-controller.

También `live_trading/execution_service` llama a `TransportSubjects::runtimeSubjects()`, por lo que actualmente crea/espera un stream que incluye subjects del clock aunque ese servicio no use el clock directamente.

## Archivos que NO deben borrarse todavía

No borrar todavía los ficheros anteriores. `simulated-exchange`, `replay-controller`, codecs y transport subjects todavía compilan contra ellos. Borrarlos en T1 rompería el árbol antes de introducir el camino de replay nuevo.

## Objetivo de borrado final

Cuando el nuevo replay use únicamente `TimeHandler`, hay que retirar por completo (si ya no tienen otros consumidores):

1. `lib/src/runtime/clock.h`
2. `lib/src/runtime/service_clock.h`
3. `lib/src/runtime/runtime_mode.h` (si queda exclusivamente ligado al legacy)
4. `lib/src/contracts/clock_state.h`
5. `lib/src/contracts/clock_control.h`
6. `lib/src/contracts/clock_sync_request.h`
7. codecs `ClockState/ClockControl/ClockSyncRequest`
8. `CLOCK_STATE`, `CLOCK_CONTROL`, `CLOCK_SYNC_REQUEST` y `runtimeSubjects()` legacy
9. `live_trading/replay_controller` como clock authority
10. flags/env legacy `runtime_mode`, `simulation_id`, `--runtime-mode`, `--simulation-id`, clock speed/control-plane legacy
11. tablas PostgreSQL de clock authority legacy, una vez confirmado que no son necesarias para migración/tests
12. compose `deploy/distributed_replay` legacy, reemplazado por `deploy/historical_replay`

## Requisito añadido para T2

`TimeHandler` debe ser pequeño y proporcionar como mínimo:

- `getTime()`
- `sleep(business_duration)` con `real_duration = business_duration / speed`
- `sleepUntil(business_time)` calculando el remanente en business time y convirtiéndolo a duración real

`sleep`/`sleepUntil` son exclusivamente para **BUSINESS SLEEP**. Timeouts, polling y backoff técnicos siguen usando `steady_clock` / tiempo real.

## Decisión T1

No se aplica patch de producción en T1. El siguiente paquete de código debe ser **T2/T3: `TimeHandler` + tests matemáticos/sleep**, sin integrar todavía todos los servicios ni borrar el clock legacy.
