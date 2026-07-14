// ═══ Self-Test engine — mapping calibration via test_target TCP feedback ═══
// GAM drives a dense sweep of mapped clicks (reusing the real click path),
// test_target reports each actual landing over TCP, and we compare expected
// vs actual to quantify mapping accuracy.
import { hostCall, onSelfTest } from './bridge'

export const sleep = (ms: number) => new Promise<void>((r) => setTimeout(r, ms))

// Window geometry reported by test_target on connect ("hello").
export interface Geometry {
  client_w: number
  client_h: number
  grid: number
  cell: number
  pad: number
  hit_margin: number
}

// One sampled point: expected (predicted) vs actual (reported).
export interface PointResult {
  rx: number; ry: number          // normalized coord sent
  expPx: number; expPy: number    // expected client px
  expGx: number; expGy: number    // expected grid cell
  expHit: boolean                 // expected inside inner target
  received: boolean               // did test_target report back
  gotGx: number; gotGy: number
  gotHit: boolean
  gotX: number; gotY: number      // actual client px reported
  dx: number | null; dy: number | null   // actual − expected px
  cellMatch: boolean
  hitMatch: boolean
}

export interface SelfTestSummary {
  geo: Geometry
  total: number
  received: number
  cellMatch: number
  hitMatch: number
  meanDx: number; meanDy: number      // systematic offset vector (px)
  meanAbs: number; maxAbs: number     // pixel error magnitude
  cells: number[][]                   // [gy][gx] cellMatch rate 0..1
  cellCounts: number[][]              // [gy][gx] sample count
  points: PointResult[]
  aborted: boolean
}

// Predict cell + hit from a client px position — mirrors test_target's own logic.
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

// Per-cell N×N centered subgrid, ordered column-major (top→bottom within a
// sub-column, then shift right) to match the described sweep pattern.
function genPoints(g: Geometry, perCell: number) {
  const pts: { rx: number; ry: number; px: number; py: number }[] = []
  for (let gx = 0; gx < g.grid; gx++) {
    for (let i = 0; i < perCell; i++) {
      const px = g.pad + gx * g.cell + ((i + 0.5) / perCell) * g.cell
      for (let gy = 0; gy < g.grid; gy++) {
        for (let j = 0; j < perCell; j++) {
          const py = g.pad + gy * g.cell + ((j + 0.5) / perCell) * g.cell
          pts.push({ rx: px / g.client_w, ry: py / g.client_h, px, py })
        }
      }
    }
  }
  return pts
}

type ClickReport = { type: 'click'; seq: number; btn: number; x: number; y: number; gx: number; gy: number; hit: boolean }

// UI phase state shared by App orchestrator + report modal.
// `_dev` marks UI-demo injections — cleared when leaving Dev mode (铁律 5).
export type SelfTestState =
  | { phase: 'idle' }
  | { phase: 'running'; done: number; total: number; _dev?: boolean }
  | { phase: 'done'; summary: SelfTestSummary; _dev?: boolean }
  | { phase: 'error'; error: string; _dev?: boolean }

export interface RunOpts {
  perCell: number
  sendClick: (rx: number, ry: number, button?: string) => Promise<any>
  port?: number
  timeoutMs?: number
  onProgress?: (done: number, total: number) => void
  shouldAbort?: () => boolean
}

// Runs the full sweep. Subscribes BEFORE connecting so the "hello" geometry
// (sent by test_target on connect) is never missed. Always disconnects + unsubs.
export async function runSelfTest(opts: RunOpts): Promise<SelfTestSummary> {
  const timeoutMs = opts.timeoutMs ?? 300
  let geo: Geometry | null = null
  let disconnected = false
  const queue: ClickReport[] = []
  let resolveReport: ((m: ClickReport | null) => void) | null = null

  const unsub = onSelfTest((m) => {
    if (m.type === 'hello') geo = m
    else if (m.type === 'click') {
      if (resolveReport) { resolveReport(m); resolveReport = null }
      else queue.push(m)
    } else if (m.type === 'disconnected') disconnected = true
  })

  try {
    await hostCall('selftest_connect', { port: opts.port ?? 9998 })

    // Await geometry handshake (≤2s)
    for (let i = 0; i < 100 && !geo; i++) await sleep(20)
    if (!geo) throw new Error('no geometry (hello) from test_target')
    const g: Geometry = geo

    const points = genPoints(g, opts.perCell)
    const results: PointResult[] = []
    let aborted = false

    for (let idx = 0; idx < points.length; idx++) {
      if (opts.shouldAbort?.() || disconnected) { aborted = true; break }
      const pt = points[idx]
      const pr = predict(pt.px, pt.py, g)

      // Arm the report waiter BEFORE sending (serial: at most one in flight).
      const waited = new Promise<ClickReport | null>((res) => {
        const q = queue.shift()
        if (q) return res(q)
        resolveReport = res
        setTimeout(() => { if (resolveReport === res) { resolveReport = null; res(null) } }, timeoutMs)
      })
      await opts.sendClick(pt.rx, pt.ry, 'left')
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
      opts.onProgress?.(idx + 1, points.length)
    }

    return summarize(g, results, aborted)
  } finally {
    unsub()
    await hostCall('selftest_disconnect').catch(() => {})
  }
}

function summarize(g: Geometry, pts: PointResult[], aborted: boolean): SelfTestSummary {
  const cells: number[][] = Array.from({ length: g.grid }, () => Array(g.grid).fill(0))
  const cellCounts: number[][] = Array.from({ length: g.grid }, () => Array(g.grid).fill(0))
  const cellHits: number[][] = Array.from({ length: g.grid }, () => Array(g.grid).fill(0))

  let received = 0, cellMatch = 0, hitMatch = 0
  let sumDx = 0, sumDy = 0, sumAbs = 0, maxAbs = 0, nDelta = 0

  for (const p of pts) {
    if (p.received) received++
    if (p.cellMatch) cellMatch++
    if (p.hitMatch) hitMatch++
    // bucket by EXPECTED cell (all samples land inside a cell)
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
    aborted,
  }
}
