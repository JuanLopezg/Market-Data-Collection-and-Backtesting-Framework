import type { ReactNode } from 'react'

export function Panel({ title, right, children, className = '' }: { title?: string; right?: ReactNode; children: ReactNode; className?: string }) {
  return (
    <section className={`panel ${className}`}>
      {(title || right) && <header className="panel__header"><h2>{title}</h2><div>{right}</div></header>}
      <div className="panel__body">{children}</div>
    </section>
  )
}
