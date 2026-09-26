import type { HealthState, PositionAlignmentState, ReconciliationState } from '../types/dashboard'

type State = HealthState | ReconciliationState | PositionAlignmentState

const toneByState: Record<State, 'good' | 'warn' | 'bad' | 'info'> = {
  READY: 'good',
  DEGRADED: 'warn',
  PAUSED: 'bad',
  CLEAN: 'good',
  ALIGNED: 'good',
  PENDING: 'warn',
  DRIFT: 'warn',
  BLOCKED: 'bad',
  UNKNOWN: 'info',
}

export function StatePill({ state }: { state: State }) {
  return <span className={`state-pill state-pill--${toneByState[state]}`}><span className="state-pill__dot" />{state}</span>
}
