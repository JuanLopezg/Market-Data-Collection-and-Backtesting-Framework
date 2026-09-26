# Legacy clock — plan de eliminación

Este archivo existe para que la retirada total no se pierda durante la migración.

## Delete físico esperado al final de la migración

- `lib/src/runtime/clock.h`
- `lib/src/runtime/service_clock.h`
- `lib/src/contracts/clock_state.h`
- `lib/src/contracts/clock_control.h`
- `lib/src/contracts/clock_sync_request.h`
- `live_trading/replay_controller/` completo, si tras la nueva topología no conserva ninguna responsabilidad no temporal
- `lib/src/runtime/runtime_mode.h`, si después de retirar el legacy no tiene un propósito independiente

## Limpiar dentro de archivos que permanecerán

- `lib/src/transport/contract_json_codec.h/.cpp`: eliminar encode/decode del clock legacy.
- `lib/src/transport/transport_subjects.h`: eliminar `simulation.clock.*`; eliminar/simplificar `runtimeSubjects()` para que no inyecte control-plane temporal.
- `live_trading/simulated_exchange_service`: eliminar `ServiceClockContext`, `runtime_mode`, `simulation_id`, guards y clock polling; integrar `TimeHandler` para tiempo económico.
- `live_trading/execution_service`: dejar de usar el subject set que incluye clock legacy.
- `deploy/distributed_replay`: retirar/reemplazar por `deploy/historical_replay`.

## Condición de borrado

No borrar un símbolo/archivo mientras `rg` encuentre un consumidor compilable. El objetivo final sí es `0` referencias al shared logical clock anterior.
