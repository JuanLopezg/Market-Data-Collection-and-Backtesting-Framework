import { DataLoadError } from '../components/DataLoadError'
import { Panel } from '../components/Panel'
import { useDashboardResource } from '../hooks/useDashboardResource'
import type { SymbolMapData, SymbolMapRow, SymbolMapState } from '../types/dashboard'

// Symbol Mapping: how each Binance symbol the strategy uses becomes a Kraken contract, and
// whether the unit conversion agrees with real prices. The table is the one the exchange
// gateway converts orders with (config/venues/kraken_futures/symbol_map.json). For a
// correct entry, Binance price / Kraken price is about the conversion factor:
// 1 for BTCUSDT -> PF_XBTUSD, 1000 for 1000PEPEUSDT -> PF_PEPEUSD.

const STATUS_TEXT: Record<SymbolMapData['status'], string> = {
  OK: 'Every strategy asset can be traded on Kraken and every conversion agrees with prices.',
  WARN: 'Some strategy assets cannot be traded on Kraken; their signals would be skipped.',
  BLOCKED: 'A conversion disagrees with prices.',
  UNAVAILABLE: 'The check could not run.',
}

const STATE_LABEL: Record<SymbolMapState, string> = {
  OK: 'OK',
  NOT_ON_VENUE: 'Not on Kraken',
  MISSING_FROM_TABLE: 'Missing from table',
  NOT_LISTED: 'No longer listed',
  SUSPENDED: 'Suspended',
  NO_PRICE: 'No price',
  SCALE_MISMATCH: 'Wrong conversion',
}

const STATE_TONE: Record<SymbolMapState, 'good' | 'warn' | 'bad' | 'info'> = {
  OK: 'good', NOT_ON_VENUE: 'info', MISSING_FROM_TABLE: 'warn', NOT_LISTED: 'warn', SUSPENDED: 'warn', NO_PRICE: 'info', SCALE_MISMATCH: 'bad',
}

const price = (value?: number) => value === undefined || value <= 0 ? '—'
  : value >= 1 ? value.toLocaleString('en-US', { maximumFractionDigits: 4 })
    : value.toLocaleString('en-US', { maximumSignificantDigits: 5 })

const conversion = (row: SymbolMapRow) => !row.venueSymbol || row.scale === undefined ? '—'
  : `1 = ${row.scale.toLocaleString('en-US')} ${row.asset}`

function strategyLabel(row: SymbolMapRow) {
  if (row.activeSignal) return 'Signal active'
  if (row.inStrategy) return 'Universe'
  return 'Top ranking'
}

function MappingRow({ row }: { row: SymbolMapRow }) {
  const tone = row.activeSignal && !['OK', 'NO_PRICE', 'SCALE_MISMATCH'].includes(row.state) ? 'alert' : STATE_TONE[row.state]
  return <tr title={row.detail}>
    <td><strong>{row.source}</strong></td>
    <td className={row.venueSymbol ? 'mono' : 'muted'}>{row.venueSymbol ?? '—'}</td>
    <td className={row.scale !== undefined && row.scale !== 1 ? 'status-warn' : ''}>{conversion(row)}</td>
    <td>{price(row.sourcePrice)}</td>
    <td>{price(row.venuePrice)}</td>
    <td className={row.state === 'SCALE_MISMATCH' ? 'negative' : 'muted'}>{row.priceRatio === undefined ? '—' : row.priceRatio.toLocaleString('en-US', { maximumSignificantDigits: 5 })}</td>
    <td className={row.activeSignal ? 'positive' : 'muted'}>{strategyLabel(row)}</td>
    <td><span className={`state-pill state-pill--${tone}`}><span className="state-pill__dot"/>{STATE_LABEL[row.state]}</span></td>
  </tr>
}

export function SymbolMappingPage() {
  const { data, error, retry } = useDashboardResource('getSymbolMap')
  if (error) return <DataLoadError title="Symbol mapping unavailable" error={error} onRetry={retry}/>
  if (!data) return <div className="page-loading">Loading symbol mapping…</div>

  const statusTone = data.status === 'OK' ? 'positive' : data.status === 'BLOCKED' ? 'negative' : 'status-warn'
  const scaled = data.rows.filter(row => row.scale !== undefined && row.scale !== 1).length
  const stats = [
    { label: 'Status', value: data.status, tone: statusTone, detail: STATUS_TEXT[data.status] },
    { label: 'Strategy Assets on Kraken', value: `${data.strategyMapped} / ${data.strategyAssets}`, tone: '', detail: 'universe assets Kraken can trade' },
    { label: 'Symbol Table', value: `${data.tableEntries} entries`, tone: '', detail: data.tableGeneratedAt ? `built ${data.tableGeneratedAt.slice(0, 10)}` : '' },
    { label: 'Last Checked', value: `${data.checkedAt.slice(11, 16)} UTC`, tone: '',
      detail: `${data.checkedAt.slice(0, 10)}${scaled ? ` · ${scaled} with a unit conversion` : ''}` },
  ]

  return <>
    <div className="page-heading"><h1>Symbol Mapping</h1></div>

    <section className="overview-stats">
      {stats.map(stat => <div className="overview-stat" key={stat.label}>
        <span>{stat.label}</span><strong className={stat.tone}>{stat.value}</strong>
        {stat.detail && <small className="muted">{stat.detail}</small>}
      </div>)}
    </section>

    {data.error && <div className="callout callout--warn"><span>{data.error}</span></div>}

    <section className="positions-layout">
      {data.alarms.length > 0 && <Panel title={`Problems Found (${data.alarms.length})`}>
        <div className="table-wrap"><table><thead><tr><th>Severity</th><th>Asset</th><th>Problem</th></tr></thead><tbody>
          {data.alarms.map(alarm => <tr key={`${alarm.eventType}-${alarm.asset}`}>
            <td><strong className={alarm.severity === 'CRITICAL' ? 'negative' : alarm.severity === 'ALERT' ? 'alert-priority' : alarm.severity === 'WARN' ? 'warning' : 'info'}>{alarm.severity}</strong></td>
            <td><strong>{alarm.asset ?? '—'}</strong></td>
            <td>{alarm.title}</td>
          </tr>)}
        </tbody></table></div>
      </Panel>}

      <Panel title="Binance → Kraken" right={<span className="muted">Ratio = Binance price / Kraken price, should equal the conversion</span>}>
        <div className="table-wrap"><table className="wide-table"><thead><tr>
          <th>Binance</th><th>Kraken</th><th>Conversion</th><th>Binance Price</th><th>Kraken Price</th><th>Ratio</th><th>Used By</th><th>State</th>
        </tr></thead><tbody>
          {data.rows.length === 0
            ? <tr><td colSpan={8} className="empty-cell">No assets to check</td></tr>
            : data.rows.map(row => <MappingRow key={row.source} row={row}/>)}
        </tbody></table></div>
      </Panel>
    </section>
  </>
}
