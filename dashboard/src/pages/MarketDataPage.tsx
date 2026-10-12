import { Search } from 'lucide-react'
import { useMemo, useState } from 'react'
import { Panel } from '../components/Panel'
import { DataLoadError } from '../components/DataLoadError'
import { useDashboardResource } from '../hooks/useDashboardResource'
import type { MarketDataData, MarketDataState } from '../types/dashboard'

// Market Data: is the data complete and current? Integrity counts and the latest candle
// sit with the other cards; the table lists each asset's last candle, worst first.
// Universe, signals and strategy rules live in Pipeline.

const PAGE = 20   // rows shown before "Show more"

function DataState({ state }: { state: MarketDataState }) {
  const tone = state === 'HEALTHY' || state === 'VALID' ? 'good' : state === 'WARN' ? 'warn' : 'bad'
  return <span className={`market-state market-state--${tone}`}>{state}</span>
}

const healthy = (state: MarketDataState) => state === 'HEALTHY' || state === 'VALID'

function MarketView({ data }: { data: MarketDataData }) {
  const [query, setQuery] = useState('')
  const [limit, setLimit] = useState(PAGE)
  const rows = useMemo(() => {
    const q = query.trim().toLowerCase()
    return data.freshness
      .filter(row => !q || row.asset.toLowerCase().includes(q))
      .sort((a, b) => Number(healthy(a.state)) - Number(healthy(b.state)) || a.asset.localeCompare(b.asset))
  }, [data, query])

  const count = (value: number) => ({ value: String(value), tone: value === 0 ? '' : 'negative' })
  const stats = [
    { label: 'Latest Candle', value: data.latestCompletedCandle, tone: '' },
    { label: 'Healthy Assets', value: `${data.healthyAssets} / ${data.totalAssets}`, tone: data.staleAssets === 0 ? 'positive' : 'negative' },
    { label: 'Missing Candles', ...count(data.integrity.missingCandles) },
    { label: 'Duplicate Timestamps', ...count(data.integrity.duplicateTimestamps) },
    { label: 'Gaps', ...count(data.integrity.gaps) },
    { label: 'Invalid Rows', ...count(data.integrity.invalidRows) },
  ]

  return <>
    <div className="page-heading"><h1>Market Data</h1></div>

    <section className="overview-stats">
      {stats.map(stat => <div className="overview-stat" key={stat.label}><span>{stat.label}</span><strong className={stat.tone}>{stat.value}</strong></div>)}
    </section>

    <Panel title="Freshness" right={<label className="search-box"><Search size={14}/><input value={query} onChange={event => setQuery(event.target.value)} placeholder="Filter asset"/></label>}>
      <div className="table-wrap"><table className="wide-table"><thead><tr><th>Asset</th><th>Last Candle</th><th>Age</th><th>Status</th></tr></thead><tbody>
        {rows.length === 0 && <tr><td colSpan={4} className="empty-cell">{data.freshness.length ? 'No assets match the filter' : 'No market data yet'}</td></tr>}
        {rows.slice(0, limit).map(row => <tr key={row.asset}>
          <td><strong>{row.asset}</strong></td>
          <td>{row.lastCandle}</td>
          <td className={row.state === 'STALE' ? 'negative' : ''}>{row.ageLabel}</td>
          <td><DataState state={row.state}/></td>
        </tr>)}
      </tbody></table></div>
      {limit < rows.length && <div className="table-more"><span className="muted">{limit} of {rows.length}</span><button className="ghost-button" onClick={() => setLimit(limit + PAGE)}>Show more</button></div>}
    </Panel>
  </>
}

export function MarketDataPage() {
  const { data, error, retry } = useDashboardResource('getMarketData')

  if (error) return <DataLoadError title="Market data unavailable" error={error} onRetry={retry}/>
  if (!data) return <div className="page-loading">Loading market data…</div>

  return <MarketView data={data}/>
}
