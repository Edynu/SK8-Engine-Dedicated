import { useEffect, useState } from 'react'
import { fetchPlayers, type Player } from '../api'

function formatIdle(ms: number): string {
  if (ms < 1000) return 'now'
  const seconds = Math.round(ms / 1000)
  if (seconds < 60) return `${seconds}s`
  return `${Math.round(seconds / 60)}m`
}

export function PlayersPage() {
  const [players, setPlayers] = useState<Player[]>([])
  const [error, setError] = useState<string | null>(null)

  useEffect(() => {
    let cancelled = false
    const refresh = async () => {
      try {
        const data = await fetchPlayers()
        if (!cancelled) {
          setPlayers(data)
          setError(null)
        }
      } catch (err) {
        if (!cancelled) setError(err instanceof Error ? err.message : String(err))
      }
    }
    refresh()
    const id = setInterval(refresh, 2000)
    return () => {
      cancelled = true
      clearInterval(id)
    }
  }, [])

  return (
    <div className="p-6">
      <div className="mb-5 flex items-baseline justify-between">
        <div>
          <h1 className="text-[15px] font-semibold text-zinc-100">Active Players</h1>
          <p className="mt-0.5 text-[13px] text-zinc-500">
            Live from the relay's own presence tracking &mdash; the same registry
            <code className="mx-1 rounded bg-white/[0.06] px-1 py-0.5 text-[12px]">
              GetSkaters()
            </code>
            reads.
          </p>
        </div>
        <span className="rounded-full border border-white/[0.08] bg-white/[0.03] px-3 py-1 text-[12px] text-zinc-400">
          {players.length} online
        </span>
      </div>

      {error && (
        <div className="mb-4 rounded-md border border-rose-900/60 bg-rose-950/40 px-3.5 py-2.5 text-[13px] text-rose-300">
          Could not reach the server: {error}
        </div>
      )}

      <div className="overflow-hidden rounded-lg border border-white/[0.06] bg-white/[0.02]">
        <table className="w-full text-[13px]">
          <thead>
            <tr className="border-b border-white/[0.06] text-left text-zinc-500">
              <th className="px-4 py-2.5 font-medium">ID</th>
              <th className="px-4 py-2.5 font-medium">Name</th>
              <th className="px-4 py-2.5 font-medium">Position</th>
              <th className="px-4 py-2.5 font-medium">Map</th>
              <th className="px-4 py-2.5 font-medium">Bucket</th>
              <th className="px-4 py-2.5 text-right font-medium">Last seen</th>
            </tr>
          </thead>
          <tbody>
            {players.length === 0 && (
              <tr>
                <td className="px-4 py-8 text-center text-zinc-600" colSpan={6}>
                  No players connected.
                </td>
              </tr>
            )}
            {players.map((player) => (
              <tr key={player.id} className="border-b border-white/[0.04] last:border-0">
                <td className="px-4 py-2.5 font-mono text-zinc-200">#{player.id}</td>
                <td className="px-4 py-2.5 font-medium text-zinc-200">
                  {player.name || <span className="font-normal text-zinc-600">unnamed</span>}
                </td>
                <td className="px-4 py-2.5 font-mono text-zinc-400">
                  {player.valid ? (
                    <>
                      {player.x.toFixed(1)}, {player.y.toFixed(1)}, {player.z.toFixed(1)}
                    </>
                  ) : (
                    <span className="text-zinc-600">not spawned</span>
                  )}
                </td>
                <td className="px-4 py-2.5 font-mono text-zinc-500">
                  {player.mapHash.replace(/^0+(?=.)/, '') || '0'}
                </td>
                <td className="px-4 py-2.5 text-zinc-400">{player.bucket}</td>
                <td className="px-4 py-2.5 text-right text-zinc-500">
                  {formatIdle(player.idleMs)}
                </td>
              </tr>
            ))}
          </tbody>
        </table>
      </div>
    </div>
  )
}
