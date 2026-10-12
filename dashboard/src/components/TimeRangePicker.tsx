import { useEffect, useRef, useState } from 'react'
import type { FormEvent } from 'react'
import { ChevronDown, Clock } from 'lucide-react'

// Grafana-style time range control: one compact button ("Last 7 days") that opens a
// dropdown with the presets and an explicit From/To (UTC). A drag-selection on a chart
// produces a custom range, shown on the button and in the inputs.

export type TimeRange =
  | { kind: 'preset'; preset: string }
  | { kind: 'custom'; from: number; to: number }

export interface TimePreset {
  value: string
  label: string       // Short form, e.g. "7D".
  title?: string      // Shown in the button and the list, e.g. "Last 7 days".
}

interface TimeRangePickerProps {
  presets: TimePreset[]
  value: TimeRange
  onChange: (range: TimeRange) => void
  precision: 'date' | 'datetime'   // date: whole UTC days (To inclusive); datetime: minutes.
  presetsOnly?: boolean            // Narrow panels: no From/To inputs.
}

const DAY = 86_400_000

function toInput(time: number, precision: 'date' | 'datetime') {
  const iso = new Date(time).toISOString()
  return precision === 'date' ? iso.slice(0, 10) : iso.slice(0, 16)
}

function fromInput(text: string, precision: 'date' | 'datetime', end: boolean) {
  const time = Date.parse(precision === 'date' ? `${text}T00:00:00Z` : `${text}:00Z`)
  // A To date includes that whole UTC day.
  return precision === 'date' && end ? time + DAY - 1 : time
}

function shownEnd(range: { to: number }, precision: 'date' | 'datetime') {
  return precision === 'date' ? range.to - DAY + 1 : range.to
}

function rangeLabel(value: TimeRange, presets: TimePreset[], precision: 'date' | 'datetime') {
  if (value.kind === 'preset') {
    const preset = presets.find(item => item.value === value.preset)
    return preset?.title ?? preset?.label ?? value.preset
  }
  const text = (time: number) => toInput(time, precision).replace('T', ' ')
  return `${text(value.from)} → ${text(shownEnd(value, precision))}`
}

export function TimeRangePicker({ presets, value, onChange, precision, presetsOnly = false }: TimeRangePickerProps) {
  const [open, setOpen] = useState(false)
  const [fromText, setFromText] = useState('')
  const [toText, setToText] = useState('')
  const [error, setError] = useState('')
  const rootRef = useRef<HTMLDivElement>(null)

  // Show the active custom range (also after a drag-selection) in the inputs.
  useEffect(() => {
    if (value.kind === 'custom') {
      setFromText(toInput(value.from, precision))
      setToText(toInput(shownEnd(value, precision), precision))
      setError('')
    }
  }, [value, precision])

  // Close on a click outside or Escape.
  useEffect(() => {
    if (!open) return
    const click = (event: MouseEvent) => { if (!rootRef.current?.contains(event.target as Node)) setOpen(false) }
    const key = (event: KeyboardEvent) => { if (event.key === 'Escape') setOpen(false) }
    document.addEventListener('mousedown', click)
    document.addEventListener('keydown', key)
    return () => { document.removeEventListener('mousedown', click); document.removeEventListener('keydown', key) }
  }, [open])

  function apply(event: FormEvent) {
    event.preventDefault()
    const from = fromInput(fromText, precision, false), to = fromInput(toText, precision, true)
    if (!Number.isFinite(from) || !Number.isFinite(to)) { setError('Enter both dates'); return }
    if (to <= from) { setError('"To" must be after "From"'); return }
    setError('')
    onChange({ kind: 'custom', from, to })
    setOpen(false)
  }

  const inputType = precision === 'date' ? 'date' : 'datetime-local'
  return <div className="time-range" ref={rootRef}>
    <button type="button" className="time-range__button" aria-haspopup="true" aria-expanded={open} onClick={() => setOpen(!open)}>
      <Clock size={14}/><span>{rangeLabel(value, presets, precision)}</span><ChevronDown size={14}/>
    </button>
    {open && <div className="time-range__menu" role="dialog" aria-label="Time range">
      {!presetsOnly && <form className="time-range__custom" onSubmit={apply}>
        <strong>Absolute time range</strong>
        <label>From<input type={inputType} value={fromText} onChange={event => setFromText(event.target.value)} required/></label>
        <label>To<input type={inputType} value={toText} onChange={event => setToText(event.target.value)} required/></label>
        <button type="submit">Apply time range</button>
        {error && <span className="negative">{error}</span>}
        <small className="muted">UTC</small>
      </form>}
      <ul className="time-range__presets">
        {presets.map(item => {
          const active = value.kind === 'preset' && value.preset === item.value
          return <li key={item.value}><button type="button" className={active ? 'active' : ''} aria-pressed={active}
            onClick={() => { onChange({ kind: 'preset', preset: item.value }); setOpen(false) }}>{item.title ?? item.label}</button></li>
        })}
      </ul>
    </div>}
  </div>
}
