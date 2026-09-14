import { useEffect, useRef, useState } from 'react'
import { fetchMetrics, type Metrics, type ResourceMetrics } from '../api'

const HISTORY_LENGTH = 40
const POLL_MS = 2000

function formatKb(kb: number): string {
  if (kb >= 1024) return `${(kb / 1024).toFixed(1)} MB`
  return `${kb.toFixed(kb >= 10 ? 0 : 1)} KB`
}

function formatKbPerSec(kb: number): string {
  if (kb >= 1024) return `${(kb / 1024).toFixed(2)} MB/s`
  return `${kb.toFixed(kb >= 10 ? 0 : 1)} KB/s`
}

// Two rolling series (down/up), each capped at HISTORY_LENGTH samples - a
// plain array in a ref rather than a charting library, since this is the
// only place in the dashboard that needs one and a hand-rolled polyline is
// a few lines against a whole dependency.
function Sparkline({
  values,
  color,
}: {
  values: number[]
  color: string
}) {
  const width = 240
  const height = 48
  const max = Math.max(1, ...values)
  const points = values
    .map((value, index) => {
      const x = values.length <= 1 ? width : (index / (values.length - 1)) * width
      const y = height - (value / max) * (height - 4) - 2
      return `${x.toFixed(1)},${y.toFixed(1)}`
    })
    .join(' ')
  const fillPoints = values.length > 0 ? `0,${height} ${points} ${width},${height}` : ''
  return (
    <svg viewBox={`0 0 ${width} ${height}`} className="h-12 w-full" preserveAspectRatio="none">
      {fillPoints && <polygon points={fillPoints} fill={color} opacity={0.12} />}
      {values.length > 1 && (
        <polyline points={points} fill="none" stroke={color} strokeWidth={1.75} />
      )}
    </svg>
  )
}

function StatCard({
  label,
  value,
  sub,
  accent = false,
  warn = false,
}: {
  label: string
  value: string
  sub?: string
  accent?: boolean
  warn?: boolean
}) {
  return (
    <div className="rounded-lg border border-white/[0.06] bg-white/[0.02] px-4 py-3.5">
      <div className="text-[11px] font-medium uppercase tracking-wide text-zinc-500">{label}</div>
      <div
        className={
          'mt-1 text-[22px] font-semibold leading-tight ' +
          (warn ? 'text-rose-400' : accent ? 'text-orange-400' : 'text-zinc-100')
        }
      >
        {value}
      </div>
      {sub && <div className="mt-0.5 text-[12px] text-zinc-500">{sub}</div>}
    </div>
  )
}

function ResourceRow({ resource, maxMs }: { resource: ResourceMetrics; maxMs: number }) {
  const fraction = maxMs > 0 ? Math.min(1, resource.msLastSecond / maxMs) : 0
  return (
    <tr className="border-b border-white/[0.04] last:border-0">
      <td className="px-4 py-2.5 font-medium text-zinc-200">{resource.name}</td>
      <td className="px-4 py-2.5">
        <div className="flex items-center gap-2">
          <div className="h-1.5 w-24 overflow-hidden rounded-full bg-white/[0.06]">
            <div
              className="h-full rounded-full bg-orange-500/80"
              style={{ width: `${fraction * 100}%` }}
            />
          </div>
          <span className="font-mono text-[12px] text-zinc-400">
            {resource.msLastSecond.toFixed(3)} ms/s
          </span>
        </div>
      </td>
      <td className="px-4 py-2.5 text-right font-mono text-zinc-400">
        {resource.callsLastSecond.toLocaleString()}
      </td>
      <td className="px-4 py-2.5 text-right font-mono text-zinc-400">
        {formatKb(resource.memoryKb)}
      </td>
    </tr>
  )
}

export function DashboardPage() {
  const [metrics, setMetrics] = useState<Metrics | null>(null)
  const [error, setError] = useState<string | null>(null)
  const downHistory = useRef<number[]>([])
  const upHistory = useRef<number[]>([])
  const [, forceRedraw] = useState(0)

  useEffect(() => {
    let cancelled = false
    const poll = async () => {
      try {
        const data = await fetchMetrics()
        if (cancelled) return
        setMetrics(data)
        setError(null)
        const downKb = data.relay
          ? data.relay.bytesInPerSec / 1024
          : (data.network?.downloadKibPerSec ?? 0)
        const upKb = data.relay
          ? data.relay.bytesOutPerSec / 1024
          : (data.network?.uploadKibPerSec ?? 0)
        downHistory.current = [...downHistory.current, downKb].slice(-HISTORY_LENGTH)
        upHistory.current = [...upHistory.current, upKb].slice(-HISTORY_LENGTH)
        forceRedraw((n) => n + 1)
      } catch (err) {
        if (!cancelled) setError(err instanceof Error ? err.message : String(err))
      }
    }
    poll()
    const id = setInterval(poll, POLL_MS)
    return () => {
      cancelled = true
      clearInterval(id)
    }
  }, [])

  const relay = metrics?.relay
  const network = metrics?.network
  const resources = [...(metrics?.resources ?? [])].sort(
    (a, b) => b.msLastSecond - a.msLastSecond,
  )
  const maxMs = resources.reduce((max, r) => Math.max(max, r.msLastSecond), 0)
  const totalResourceMs = resources.reduce((sum, r) => sum + r.msLastSecond, 0)

  const downKb = relay ? relay.bytesInPerSec / 1024 : (network?.downloadKibPerSec ?? 0)
  const upKb = relay ? relay.bytesOutPerSec / 1024 : (network?.uploadKibPerSec ?? 0)

  return (
    <div className="p-6">
      <div className="mb-5 flex items-baseline justify-between">
        <div>
          <h1 className="text-[15px] font-semibold text-zinc-100">Dashboard</h1>
          <p className="mt-0.5 text-[13px] text-zinc-500">
            {relay
              ? 'Relay bandwidth, loop hitches and Lua resmon.'
              : 'This connection’s bandwidth and Lua resmon.'}
          </p>
        </div>
        {relay && (
          <span
            className={
              'rounded-full border px-3 py-1 text-[12px] ' +
              (relay.hitchesPerSec > 0
                ? 'border-rose-900/60 bg-rose-950/40 text-rose-300'
                : 'border-white/[0.08] bg-white/[0.03] text-zinc-400')
            }
          >
            {relay.hitchesPerSec > 0
              ? `${relay.hitchesPerSec} hitch${relay.hitchesPerSec === 1 ? '' : 'es'}/s`
              : 'No hitches'}
          </span>
        )}
      </div>

      {error && (
        <div className="mb-4 rounded-md border border-rose-900/60 bg-rose-950/40 px-3.5 py-2.5 text-[13px] text-rose-300">
          Could not reach the server: {error}
        </div>
      )}

      <div className="mb-4 grid grid-cols-2 gap-3 lg:grid-cols-4">
        <StatCard label="Download" value={formatKbPerSec(downKb)} accent />
        <StatCard label="Upload" value={formatKbPerSec(upKb)} accent />
        {relay ? (
          <>
            <StatCard
              label="Hitches"
              value={relay.hitchesTotal.toLocaleString()}
              sub={`worst ${relay.worstIterationMs.toFixed(1)} ms`}
              warn={relay.hitchesPerSec > 0}
            />
            <StatCard
              label="Total Relayed"
              value={`${((relay.bytesInTotal + relay.bytesOutTotal) / 1024 / 1024).toFixed(1)} MB`}
            />
          </>
        ) : (
          <>
            <StatCard
              label="Peers"
              value={`${network?.visiblePlayers ?? 0} / ${network?.knownPeers ?? 0}`}
              sub="visible / known"
            />
            <StatCard
              label="Session"
              value={network?.enabled ? `Role ${network.role}` : 'Offline'}
            />
          </>
        )}
      </div>

      <div className="mb-4 grid grid-cols-1 gap-3 lg:grid-cols-2">
        <div className="rounded-lg border border-white/[0.06] bg-white/[0.02] px-4 py-3.5">
          <div className="mb-1 text-[11px] font-medium uppercase tracking-wide text-zinc-500">
            Download
          </div>
          <Sparkline values={downHistory.current} color="#fb923c" />
        </div>
        <div className="rounded-lg border border-white/[0.06] bg-white/[0.02] px-4 py-3.5">
          <div className="mb-1 text-[11px] font-medium uppercase tracking-wide text-zinc-500">
            Upload
          </div>
          <Sparkline values={upHistory.current} color="#fdba74" />
        </div>
      </div>

      <div className="mb-2 flex items-baseline justify-between">
        <h2 className="text-[13px] font-semibold text-zinc-200">Resource Monitor</h2>
        <span className="text-[12px] text-zinc-500">
          {resources.length} resource{resources.length === 1 ? '' : 's'}, {totalResourceMs.toFixed(3)} ms/s total
        </span>
      </div>
      <div className="overflow-hidden rounded-lg border border-white/[0.06] bg-white/[0.02]">
        <table className="w-full text-[13px]">
          <thead>
            <tr className="border-b border-white/[0.06] text-left text-zinc-500">
              <th className="px-4 py-2.5 font-medium">Resource</th>
              <th className="px-4 py-2.5 font-medium">CPU</th>
              <th className="px-4 py-2.5 text-right font-medium">Calls/s</th>
              <th className="px-4 py-2.5 text-right font-medium">Memory</th>
            </tr>
          </thead>
          <tbody>
            {resources.length === 0 && (
              <tr>
                <td className="px-4 py-8 text-center text-zinc-600" colSpan={4}>
                  No resources running.
                </td>
              </tr>
            )}
            {resources.map((resource) => (
              <ResourceRow key={resource.name} resource={resource} maxMs={maxMs} />
            ))}
          </tbody>
        </table>
      </div>
    </div>
  )
}
