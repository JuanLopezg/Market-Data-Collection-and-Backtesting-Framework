import { AlertTriangle, RotateCcw } from 'lucide-react'

export function DataLoadError({ title, error, onRetry }: { title: string; error: Error; onRetry: () => void }) {
  return <div className="data-load-error" role="alert">
    <AlertTriangle size={18}/>
    <div><strong>{title}</strong><span>{error.message}</span></div>
    <button type="button" onClick={onRetry}><RotateCcw size={12}/> Retry</button>
  </div>
}
