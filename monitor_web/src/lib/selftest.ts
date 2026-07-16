// ═══ Self-Test engine — mapping calibration via test_target TCP feedback ═══
// GAM drives real mapped input (same path as a user), test_target reports
// landings over TCP, and we compare expected vs actual.
import { hostCall, onSelfTest, type SelfTestMsg } from './bridge'

export const sleep = (ms: number) => new Promise<void>((r) => setTimeout(r, ms))

export const SELFTEST_PORT = 19998

export interface Region {
  x: number; y: number; w: number; h: number
}

// Window geometry reported by test_target on connect ("hello").
// client_* = HTML / WebView content space (grid math + JS events).
// win_* + client_off_* = outer frame used by WGC / send_input (GetWindowRect).
export interface Geometry {
  client_w: number
  client_h: number
  win_w?: number
  win_h?: number
  client_off_x?: number
  client_off_y?: number
  grid: number
  cell: number
  pad: number
  hit_margin: number
  version?: number
  port?: number
  regions?: {
    grid?: Region
    buttons?: Region
    scroll?: Region
    drag?: Region
    input?: Region
  }
}

/** Content/client px → window-normalized coords for send_input / overlays. */
function contentToWindowNorm(px: number, py: number, g: Geometry) {
  const ox = g.client_off_x ?? 0
  const oy = g.client_off_y ?? 0
  const ww = g.win_w && g.win_w > 0 ? g.win_w : g.client_w
  const wh = g.win_h && g.win_h > 0 ? g.win_h : g.client_h
  return { rx: (ox + px) / ww, ry: (oy + py) / wh }
}

export interface PointResult {
  rx: number; ry: number
  expPx: number; expPy: number
  expGx: number; expGy: number
  expHit: boolean
  received: boolean
  gotGx: number; gotGy: number
  gotHit: boolean
  gotX: number; gotY: number
  dx: number | null; dy: number | null
  cellMatch: boolean
  hitMatch: boolean
}

export interface ScenarioResult {
  id: string
  label: string
  ok: boolean
  detail: string
}

export interface SelfTestSummary {
  geo: Geometry
  total: number
  received: number
  cellMatch: number
  hitMatch: number
  meanDx: number; meanDy: number
  meanAbs: number; maxAbs: number
  cells: number[][]
  cellCounts: number[][]
  points: PointResult[]
  scenarios: ScenarioResult[]
  aborted: boolean
}

function predict(px: number, py: number, g: Geometry) {
  const rx = px - g.pad, ry = py - g.pad
  const gx = rx >= 0 ? Math.floor(rx / g.cell) : -1
  const gy = ry >= 0 ? Math.floor(ry / g.cell) : -1
  const inGrid = gx >= 0 && gx < g.grid && gy >= 0 && gy < g.grid
  const lx = inGrid ? rx - gx * g.cell : -1
  const ly = inGrid ? ry - gy * g.cell : -1
  const hit =
    inGrid &&
    lx >= g.hit_margin && lx < g.cell - g.hit_margin &&
    ly >= g.hit_margin && ly < g.cell - g.hit_margin
  return { gx: inGrid ? gx : -1, gy: inGrid ? gy : -1, hit }
}

function genPoints(g: Geometry, perCell: number) {
  const pts: { rx: number; ry: number; px: number; py: number }[] = []
  for (let gx = 0; gx < g.grid; gx++) {
    for (let i = 0; i < perCell; i++) {
      const px = g.pad + gx * g.cell + ((i + 0.5) / perCell) * g.cell
      for (let gy = 0; gy < g.grid; gy++) {
        for (let j = 0; j < perCell; j++) {
          const py = g.pad + gy * g.cell + ((j + 0.5) / perCell) * g.cell
          const n = contentToWindowNorm(px, py, g)
          pts.push({ rx: n.rx, ry: n.ry, px, py })
        }
      }
    }
  }
  return pts
}

function regionCenter(r: Region, g: Geometry) {
  const px = r.x + r.w / 2
  const py = r.y + r.h / 2
  const n = contentToWindowNorm(px, py, g)
  return { px, py, rx: n.rx, ry: n.ry }
}

export type SelfTestState =
  | { phase: 'idle' }
  | { phase: 'running'; done: number; total: number; step?: string; _dev?: boolean }
  | { phase: 'done'; summary: SelfTestSummary; _dev?: boolean }
  | { phase: 'error'; error: string; _dev?: boolean }

export interface MonitorInputApi {
  sendClick: (rx: number, ry: number, button?: string) => Promise<any>
  sendWheel: (rx: number, ry: number, delta: number) => Promise<any>
  sendDrag: (path: { x: number; y: number }[], button?: string) => Promise<any>
  sendText: (text: string) => Promise<any>
  sendKey: (type: 'keydown' | 'keyup', key: string, code: string, vk: number) => Promise<any>
}

export interface RunOpts {
  perCell: number
  api: MonitorInputApi
  port?: number
  timeoutMs?: number
  onProgress?: (done: number, total: number, step?: string) => void
  shouldAbort?: () => boolean
}

type AnyReport = SelfTestMsg & Record<string, any>

class ReportBus {
  private queue: AnyReport[] = []
  private waiters: Array<{
    pred: (m: AnyReport) => boolean
    resolve: (m: AnyReport | null) => void
    timer: number
  }> = []
  disconnected = false

  push(m: AnyReport) {
    if (m.type === 'disconnected') {
      this.disconnected = true
      for (const w of this.waiters.splice(0)) {
        clearTimeout(w.timer)
        w.resolve(null)
      }
      return
    }
    const idx = this.waiters.findIndex((w) => w.pred(m))
    if (idx >= 0) {
      const [w] = this.waiters.splice(idx, 1)
      clearTimeout(w.timer)
      w.resolve(m)
      return
    }
    this.queue.push(m)
  }

  wait(pred: (m: AnyReport) => boolean, timeoutMs: number): Promise<AnyReport | null> {
    const cached = this.queue.findIndex(pred)
    if (cached >= 0) {
      const [m] = this.queue.splice(cached, 1)
      return Promise.resolve(m)
    }
    return new Promise((resolve) => {
      const entry = {
        pred,
        resolve,
        timer: window.setTimeout(() => {
          const i = this.waiters.indexOf(entry)
          if (i >= 0) this.waiters.splice(i, 1)
          resolve(null)
        }, timeoutMs),
      }
      this.waiters.push(entry)
    })
  }
}

async function runScenarios(
  g: Geometry,
  api: MonitorInputApi,
  bus: ReportBus,
  timeoutMs: number,
  shouldAbort?: () => boolean,
): Promise<ScenarioResult[]> {
  const out: ScenarioResult[] = []
  const regions = g.regions || {}

  const add = (id: string, label: string, ok: boolean, detail: string) => {
    out.push({ id, label, ok, detail })
  }

  // 1) Right-click center of grid
  if (!shouldAbort?.()) {
    const px = g.pad + 2.5 * g.cell
    const py = g.pad + 2.5 * g.cell
    const n = contentToWindowNorm(px, py, g)
    const waited = bus.wait((m) => m.type === 'click' && m.btn === 2, timeoutMs)
    await api.sendClick(n.rx, n.ry, 'right')
    const rep = await waited
    add(
      'right_click',
      'Right click',
      !!rep && rep.gx === 2 && rep.gy === 2,
      rep ? `btn=${rep.btn} grid[${rep.gx},${rep.gy}] hit=${rep.hit}` : 'no feedback',
    )
  }

  // 2) Wheel on scroll panel
  if (!shouldAbort?.() && regions.scroll) {
    const c = regionCenter(regions.scroll, g)
    const waited = bus.wait((m) => m.type === 'wheel' || m.type === 'scroll', timeoutMs + 200)
    await api.sendWheel(c.rx, c.ry, 120)
    const rep = await waited
    add(
      'wheel',
      'Scroll wheel',
      !!rep,
      rep
        ? `type=${rep.type} delta=${rep.delta ?? '?'} scrollTop=${rep.scrollTop ?? '?'}`
        : 'no feedback',
    )
  }

  // 3) Drag across drag pad
  if (!shouldAbort?.() && regions.drag) {
    const r = regions.drag
    const a = contentToWindowNorm(r.x + 60, r.y + 80, g)
    const b = contentToWindowNorm(r.x + r.w - 40, r.y + 80, g)
    const mid = contentToWindowNorm(r.x + r.w / 2, r.y + 80, g)
    const path = [
      { x: a.rx, y: a.ry },
      { x: mid.rx, y: mid.ry },
      { x: b.rx, y: b.ry },
    ]
    const waited = bus.wait((m) => m.type === 'drag', timeoutMs + 400)
    await api.sendDrag(path, 'left')
    const rep = await waited
    add(
      'drag',
      'Drag',
      !!rep,
      rep ? `box=(${rep.boxX},${rep.boxY})` : 'no feedback',
    )
  }

  // 4) Button A
  if (!shouldAbort?.() && regions.buttons) {
    // Button A is the first control — click left portion of the buttons row.
    const r = regions.buttons
    const n = contentToWindowNorm(r.x + 40, r.y + r.h / 2, g)
    const waited = bus.wait((m) => m.type === 'button' && m.id === 'btn-a', timeoutMs)
    await api.sendClick(n.rx, n.ry, 'left')
    const rep = await waited
    add(
      'button',
      'Button A',
      !!rep && rep.id === 'btn-a',
      rep ? `id=${rep.id}` : 'no feedback',
    )
  }

  // 5) Text input: focus input → type text
  if (!shouldAbort?.() && regions.input) {
    const c = regionCenter(regions.input, g)
    await api.sendClick(c.rx, c.ry, 'left')
    await sleep(80)
    const sample = 'GAM'
    const waited = bus.wait(
      (m) => (m.type === 'text' && typeof m.text === 'string' && m.text.includes('G'))
        || (m.type === 'keydown' && m.target === 'input'),
      timeoutMs + 400,
    )
    await api.sendText(sample)
    const rep = await waited
    const ok = !!rep && (
      (rep.type === 'text' && String(rep.text).includes('G'))
      || rep.type === 'keydown'
    )
    add(
      'text',
      'Text input',
      ok,
      rep ? `type=${rep.type} text=${rep.text ?? rep.key ?? ''}` : 'no feedback',
    )
  }

  return out
}

export async function runSelfTest(opts: RunOpts): Promise<SelfTestSummary> {
  const timeoutMs = opts.timeoutMs ?? 400
  let geo: Geometry | null = null
  const bus = new ReportBus()

  const unsub = onSelfTest((m) => {
    if (m.type === 'hello') geo = m as Geometry
    else bus.push(m as AnyReport)
  })

  try {
    // Connect with retries — test_target TCP may lag a beat behind FindWindow.
    const port = opts.port ?? SELFTEST_PORT
    let connectErr = 'connect failed'
    for (let attempt = 0; attempt < 15 && !geo; attempt++) {
      try {
        const res: any = await hostCall('selftest_connect', { port })
        if (res?.hello && res.hello.type === 'hello') {
          geo = res.hello as Geometry
          break
        }
        if (res?.ok === false) {
          connectErr = res.error || connectErr
        }
      } catch (e: any) {
        connectErr = e?.message || String(e)
      }
      await sleep(150)
    }
    // Backup: hello may still arrive as a push event from the reader thread.
    for (let i = 0; i < 40 && !geo; i++) await sleep(25)
    if (!geo) throw new Error(`no geometry (hello) from test_target: ${connectErr}`)
    const g: Geometry = geo

    const points = genPoints(g, opts.perCell)
    // scenarios ≈ 5 steps; progress total includes them
    const scenarioBudget = 5
    const totalWork = points.length + scenarioBudget
    const results: PointResult[] = []
    let aborted = false
    let done = 0

    opts.onProgress?.(0, totalWork, 'grid')

    for (let idx = 0; idx < points.length; idx++) {
      if (opts.shouldAbort?.() || bus.disconnected) { aborted = true; break }
      const pt = points[idx]
      const pr = predict(pt.px, pt.py, g)

      const waited = bus.wait((m) => m.type === 'click' && (m.btn === 0 || m.btn == null), timeoutMs)
      await opts.api.sendClick(pt.rx, pt.ry, 'left')
      const rep = await waited

      const received = !!rep
      const dx = rep ? rep.x - pt.px : null
      const dy = rep ? rep.y - pt.py : null
      results.push({
        rx: pt.rx, ry: pt.ry,
        expPx: pt.px, expPy: pt.py,
        expGx: pr.gx, expGy: pr.gy, expHit: pr.hit,
        received,
        gotGx: rep ? rep.gx : -1,
        gotGy: rep ? rep.gy : -1,
        gotHit: rep ? !!rep.hit : false,
        gotX: rep ? rep.x : NaN,
        gotY: rep ? rep.y : NaN,
        dx, dy,
        cellMatch: received && rep!.gx === pr.gx && rep!.gy === pr.gy,
        hitMatch: received && !!rep!.hit === pr.hit,
      })
      done++
      opts.onProgress?.(done, totalWork, 'grid')
    }

    let scenarios: ScenarioResult[] = []
    if (!aborted && !opts.shouldAbort?.()) {
      opts.onProgress?.(done, totalWork, 'scenarios')
      scenarios = await runScenarios(g, opts.api, bus, timeoutMs, opts.shouldAbort)
      done = totalWork
      opts.onProgress?.(done, totalWork, 'done')
      if (opts.shouldAbort?.()) aborted = true
    }

    return summarize(g, results, scenarios, aborted)
  } finally {
    unsub()
    await hostCall('selftest_disconnect').catch(() => {})
  }
}

function summarize(
  g: Geometry,
  pts: PointResult[],
  scenarios: ScenarioResult[],
  aborted: boolean,
): SelfTestSummary {
  const cells: number[][] = Array.from({ length: g.grid }, () => Array(g.grid).fill(0))
  const cellCounts: number[][] = Array.from({ length: g.grid }, () => Array(g.grid).fill(0))
  const cellHits: number[][] = Array.from({ length: g.grid }, () => Array(g.grid).fill(0))

  let received = 0, cellMatch = 0, hitMatch = 0
  let sumDx = 0, sumDy = 0, sumAbs = 0, maxAbs = 0, nDelta = 0

  for (const p of pts) {
    if (p.received) received++
    if (p.cellMatch) cellMatch++
    if (p.hitMatch) hitMatch++
    if (p.expGx >= 0 && p.expGy >= 0) {
      cellCounts[p.expGy][p.expGx]++
      if (p.cellMatch) cellHits[p.expGy][p.expGx]++
    }
    if (p.dx != null && p.dy != null) {
      sumDx += p.dx; sumDy += p.dy
      const abs = Math.hypot(p.dx, p.dy)
      sumAbs += abs; if (abs > maxAbs) maxAbs = abs
      nDelta++
    }
  }
  for (let y = 0; y < g.grid; y++)
    for (let x = 0; x < g.grid; x++)
      cells[y][x] = cellCounts[y][x] ? cellHits[y][x] / cellCounts[y][x] : 0

  return {
    geo: g,
    total: pts.length,
    received, cellMatch, hitMatch,
    meanDx: nDelta ? sumDx / nDelta : 0,
    meanDy: nDelta ? sumDy / nDelta : 0,
    meanAbs: nDelta ? sumAbs / nDelta : 0,
    maxAbs,
    cells, cellCounts,
    points: pts,
    scenarios,
    aborted,
  }
}
