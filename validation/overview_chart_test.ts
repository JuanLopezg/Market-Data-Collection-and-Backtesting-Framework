import assert from 'node:assert/strict'
import { chartDate, chartWindow, chartRanges } from '../dashboard/src/components/LineChart'
const point = (label: string, equity = 100) => ({ label, equity, benchmark: 90 })
const daily = Array.from({length: 400}, (_, i) => point(new Date(Date.UTC(2025, 0, 1) + i * 86400000).toISOString().slice(0, 10)))
for (const [range, count] of [['1D',1],['7D',7],['30D',30],['90D',90],['1Y',365],['ALL',400]] as const) {
  assert.equal(chartWindow(daily, range).length, count)
}
assert.equal(chartRanges.length, 6)
assert.equal(chartDate('20261008'), chartDate('2026-10-08'))
assert.ok(Number.isNaN(chartDate('2026-02-30')))
assert.equal(chartWindow([point('not-a-date'), point('2026-10-08', NaN)], 'ALL').length, 0)
assert.equal(chartWindow([], 'ALL').length, 0)
assert.equal(chartWindow([point('2026-10-08')], '1D').length, 1)
const sparse = [point('2026-01-01'), point('2026-10-01'), point('2026-10-08')]
assert.deepEqual(chartWindow(sparse.reverse(), '7D').map(p => p.label), ['2026-10-08'])
assert.equal(chartWindow([point('Sep 22'), point('Sep 23')], '1D')[0].label, 'Sep 23')
console.log('OVERVIEW-WINDOWS: PASS: all ranges, sparse dates, UTC replay dates, malformed/nonfinite rows, single/empty history')
