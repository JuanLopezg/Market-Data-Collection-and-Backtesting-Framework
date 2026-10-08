"""Build a portable, offline CPU/RAM report from observed container samples."""
import html
import json
import re


def parse_sample(line):
    # Docker Desktop may emit terminal redraw sequences even with piped stdout.
    return json.loads(re.sub(r"\x1b\[[0-?]*[ -/]*[@-~]", "", line))


def memory_bytes(value):
    match = re.fullmatch(r"\s*([0-9.]+)\s*([A-Za-z]+)\s*", value.split(" / ")[0])
    units = {"B": 1, "kB": 1000, "MB": 1000**2, "GB": 1000**3,
             "KiB": 1024, "MiB": 1024**2, "GiB": 1024**3}
    if not match or match[2] not in units:
        raise ValueError("Unknown Docker memory unit: " + value)
    return float(match[1]) * units[match[2]]


def series(metadata):
    names = sorted({name for row in metadata.get("samples", []) for name in row["containers"]})
    data = {"All containers": [], **{name: [] for name in names}}
    for row in metadata.get("samples", []):
        total_cpu = total_ram = 0
        available = 0
        for name in names:
            sample = row["containers"].get(name)
            # Never treat old or absent measurements as zero consumption.
            if sample is None or row["seconds"] - sample["seconds"] > 5:
                data[name].append([row["seconds"], None, None])
                continue
            cpu = float(sample["sample"]["CPUPerc"].rstrip("%"))
            ram = memory_bytes(sample["sample"]["MemUsage"]) / 1024**2
            data[name].append([row["seconds"], cpu, ram])
            total_cpu += cpu
            total_ram += ram
            available += 1
        data["All containers"].append([row["seconds"], total_cpu if available == len(names) else None,
                                       total_ram if available == len(names) else None])
    for name, cpu_key, ram_key in (("Windows host", "cpu_percent", "ram_bytes"),
                                  ("All browser processes", None, "browser_ram_bytes"),
                                  ("WSL / VM host processes", None, "wsl_ram_bytes")):
        values = [[row["seconds"], float(row["sample"][cpu_key]) if cpu_key else None,
                   float(row["sample"].get(ram_key) or 0) / 1024**2] for row in metadata.get("host_samples", [])]
        if values: data[name] = values
    return data


def write_report(directory, metadata):
    data = series(metadata)
    statistics = {}
    for name, rows in data.items():
        cpu = [row[1] for row in rows if row[1] is not None]
        ram = [row[2] for row in rows if row[2] is not None]
        statistics[name] = {"samples": len(ram), "cpu_mean_percent": sum(cpu) / len(cpu) if cpu else None,
                            "cpu_sampled_peak_percent": max(cpu) if cpu else None,
                            "ram_mean_mib": sum(ram) / len(ram) if ram else None,
                            "ram_sampled_peak_mib": max(ram) if ram else None}
    (directory / "resource_summary.json").write_text(json.dumps(statistics, indent=2) + "\n")
    rows = []
    def number(value): return "Unavailable" if value is None else f"{value:,.2f}"
    for name, values in statistics.items():
        prefix = metadata.get("project", "") + "-"
        label = name.removeprefix(prefix).removesuffix("-1") if name.startswith(prefix) else name
        rows.append("<tr><td>" + html.escape(label) + "</td>" + "".join("<td>" + number(values[key]) + "</td>" for key in
            ("cpu_mean_percent", "cpu_sampled_peak_percent", "ram_mean_mib", "ram_sampled_peak_mib")) + "</tr>")
    payload = json.dumps({"series": data, "completed": metadata.get("replay_completed_seconds"),
                          "duration": metadata.get("measured_seconds", metadata["requested_seconds"]),
                          "project": metadata.get("project", "")}).replace("<", "\\u003c")
    page = """<!doctype html><html lang="en"><meta charset="utf-8"><meta name="viewport" content="width=device-width">
<title>Local CPU and RAM profile</title><style>
body{font:17px system-ui,sans-serif;background:#0f172a;color:#e2e8f0;margin:0;padding:30px;max-width:1350px;margin:auto}
h1{font-size:30px}p{line-height:1.55;color:#cbd5e1}select{font:inherit;padding:10px;background:#1e293b;color:white;border:1px solid #64748b;border-radius:6px;max-width:100%}
svg{background:#172033;width:100%;height:auto;border-radius:10px;margin-top:20px}text{fill:#cbd5e1;font:15px system-ui}
table{border-collapse:collapse;width:100%;font-size:15px}td,th{padding:12px;text-align:left;border-bottom:1px solid #334155}.table{overflow-x:auto}
#tooltip{padding:14px;background:#1e293b;border-radius:8px;min-height:26px}.status{font-weight:700;color:#93c5fd}
</style><h1>Local CPU and RAM profile</h1>
<p class="status">__STATUS__ · __DAYS__ historical days · __DURATION__ seconds measured</p>
<p>The validated canonical replay runs in its own container. The historical service sandbox stays up alongside it with its separate synthetic fixture.
The dashboard API and web UI observe canonical simulation state; watchdog and test-file notifier remain running. One authenticated client polls six read resources every five seconds.
Setup/build/startup is excluded. Deliberate display delays add approximately 25% of the window to replay processing; this is not maximum-speed backtest throughput.</p>
<p>Docker CPU: 100% equals one logical core; totals can exceed 100%. RAM is Docker's Linux working-set measurement (MiB).
Windows host CPU uses the whole-machine 0–100% scale and includes unrelated applications. Browser/WSL host memory overlaps host usage and must not be added to container totals.
The browser series covers detected Chrome/Edge/Firefox processes, not a single tab. Missing/stale measurements are gaps; sampled peaks can miss short spikes.</p>
<p>Browser RAM sums process working sets, so shared pages can be counted more than once. Existing dashboard deployment CPU/RAM limits remain in effect.</p>
<label>View <select id="view"></select></label><svg id="cpu" viewBox="0 0 1100 320" aria-label="CPU over time"></svg>
<svg id="ram" viewBox="0 0 1100 320" aria-label="RAM over time"></svg><p id="tooltip">Move over either graph to inspect a sample.</p>
<p>The dashed line marks canonical replay completion. Values after it show the stack remaining up. VPS hardware, real exchange load and full-history capacity remain unverified.</p>
<p>Authenticated dashboard reads: __SUCCESS__ successful, __FAILED__ failed (including state-unavailable warmup).</p>
<p>Windows host sampling: __HOST__. __ERRORS__</p><div class="table"><table><thead><tr><th>Component</th><th>Mean CPU %</th><th>Sampled peak CPU %</th><th>Mean RAM MiB</th><th>Sampled peak RAM MiB</th></tr></thead><tbody>__ROWS__</tbody></table></div>
<script>const report=__DATA__, ns='http://www.w3.org/2000/svg', select=document.getElementById('view');
const prefix=report.project+'-', label=n=>n.startsWith(prefix)?n.slice(prefix.length).replace(/-\d+$/,''):n;
for(const name of ['All containers',...Object.keys(report.series).filter(n=>n!=='All containers')]){const o=document.createElement('option');o.value=name;o.textContent=label(name);select.append(o)}
function element(svg,tag,attrs,text){const e=document.createElementNS(ns,tag);for(const [k,v] of Object.entries(attrs))e.setAttribute(k,v);if(text)e.textContent=text;svg.append(e);return e}
function draw(id,index,title){const svg=document.getElementById(id),values=report.series[select.value]||[];svg.replaceChildren();const usable=values.filter(v=>v[index]!=null),max=Math.max(1,...usable.map(v=>v[index]))*1.12;
const x=t=>90+970*t/Math.max(1,report.duration),y=v=>265-205*v/max;
element(svg,'text',{x:25,y:30},title);for(let i=0;i<=4;i++){const v=max*i/4;element(svg,'line',{x1:90,y1:y(v),x2:1060,y2:y(v),stroke:'#334155'});element(svg,'text',{x:10,y:y(v)+5},v.toFixed(1));element(svg,'text',{x:x(report.duration*i/4)-15,y:300},(report.duration*i/4).toFixed(0)+' s')}
let d='',fresh=true;for(const v of values){if(v[index]==null){fresh=true;continue}d+=(fresh?'M':'L')+x(v[0])+','+y(v[index])+' ';fresh=false}element(svg,'path',{d,fill:'none',stroke:index===1?'#38bdf8':'#a78bfa','stroke-width':2});
if(report.completed!=null)element(svg,'line',{x1:x(report.completed),x2:x(report.completed),y1:50,y2:265,stroke:'#fbbf24','stroke-dasharray':'5 5'});
const cursor=element(svg,'line',{x1:0,x2:0,y1:50,y2:265,stroke:'#e2e8f0',visibility:'hidden'});
svg.onpointermove=e=>{if(!values.length)return;const t=((e.clientX-svg.getBoundingClientRect().left)/svg.getBoundingClientRect().width*1100-90)/970*report.duration;
const v=values.reduce((a,b)=>Math.abs(b[0]-t)<Math.abs(a[0]-t)?b:a);cursor.setAttribute('x1',x(v[0]));cursor.setAttribute('x2',x(v[0]));cursor.setAttribute('visibility','visible');
document.getElementById('tooltip').textContent=label(select.value)+' · '+v[0].toFixed(1)+' s · CPU '+(v[1]==null?'unavailable':v[1].toFixed(2)+'%')+' · RAM '+(v[2]==null?'unavailable':v[2].toFixed(2)+' MiB')};}
function redraw(){draw('cpu',1,'CPU (%)');draw('ram',2,'RAM (MiB)')}select.onchange=redraw;redraw();</script></html>"""
    for key, value in {"__STATUS__": html.escape(metadata["result"]), "__DAYS__": str(metadata["days"]),
                       "__DURATION__": f"{metadata.get('measured_seconds', 0):.1f}",
                       "__HOST__": "available" if metadata.get("host_samples") else "unavailable; container measurements remain separate",
                       "__SUCCESS__": str(metadata.get("dashboard_requests", {}).get("successful", 0)),
                       "__FAILED__": str(metadata.get("dashboard_requests", {}).get("failed", 0)),
                       "__ERRORS__": html.escape("; ".join(metadata.get("errors", []))),
                       "__ROWS__": "".join(rows), "__DATA__": payload}.items():
        page = page.replace(key, value)
    (directory / "resources.html").write_text(page, encoding="utf-8")
