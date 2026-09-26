export function ComingSoonPage({ title, step }: { title: string; step: string }) {
  return <>
    <div className="page-heading"><div><h1>{title}</h1><p>This route is wired and ready for the next implementation package.</p></div></div>
    <div className="coming-soon"><strong>{title}</strong><span>Planned for {step}</span><p>The persistent shell and navigation are already functional, so future pages drop into the same lightweight layout without reworking the app structure.</p></div>
  </>
}
