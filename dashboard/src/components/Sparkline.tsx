export function Sparkline({ values }: { values: number[] }) {
  const width = 108, height = 36
  const min = Math.min(...values), max = Math.max(...values)
  const span = max - min || 1
  const points = values.map((v, i) => `${(i/(values.length-1))*width},${height-((v-min)/span)*height}`).join(' ')
  return <svg className="sparkline" viewBox={`0 0 ${width} ${height}`} aria-hidden="true"><polyline points={points} fill="none" vectorEffect="non-scaling-stroke" /></svg>
}
