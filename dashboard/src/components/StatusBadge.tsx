import type { ReactNode } from 'react'

export function StatusBadge({ children, tone = 'good' }: { children: ReactNode; tone?: 'good' | 'warn' | 'bad' | 'info' }) {
  return <span className={`status-badge status-badge--${tone}`}>{children}</span>
}
