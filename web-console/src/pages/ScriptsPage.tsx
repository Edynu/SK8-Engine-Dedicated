import { useCallback, useEffect, useState } from 'react'
import { actOnResource, fetchResources, type ResourceInfo } from '../api'

type Action = 'ensure' | 'restart' | 'stop'

const actionStyle: Record<Action, string> = {
  ensure: 'bg-emerald-600/90 hover:bg-emerald-500',
  restart: 'bg-amber-600/90 hover:bg-amber-500',
  stop: 'bg-rose-700/90 hover:bg-rose-600',
}

export function ScriptsPage({ onConnectivityChange }: { onConnectivityChange: (ok: boolean) => void }) {
  const [resources, setResources] = useState<ResourceInfo[]>([])
  const [pending, setPending] = useState<string | null>(null)
  const [error, setError] = useState<string | null>(null)

  const refresh = useCallback(async () => {
    try {
      const data = await fetchResources()
      setResources(data)
      setError(null)
      onConnectivityChange(true)
    } catch (err) {
      setError(err instanceof Error ? err.message : String(err))
      onConnectivityChange(false)
    }
  }, [onConnectivityChange])

  useEffect(() => {
    refresh()
    const id = setInterval(refresh, 2000)
    return () => clearInterval(id)
  }, [refresh])

  const act = async (name: string, action: Action) => {
    setPending(`${name}:${action}`)
    try {
      await actOnResource(name, action)
      await refresh()
    } finally {
      setPending(null)
    }
  }

  const started = resources.filter((r) => r.status === 'started').length

  return (
    <div className="p-6">
      <div className="mb-5 flex items-baseline justify-between">
        <div>
          <h1 className="text-[15px] font-semibold text-zinc-100">Scripts</h1>
          <p className="mt-0.5 text-[13px] text-zinc-500">
            Lua resources discovered from <code className="rounded bg-white/[0.06] px-1 py-0.5 text-[12px]">sk8manifest.lua</code>.
          </p>
        </div>
        <span className="rounded-full border border-white/[0.08] bg-white/[0.03] px-3 py-1 text-[12px] text-zinc-400">
          {started} / {resources.length} running
        </span>
      </div>

      {error && (
        <div className="mb-4 rounded-md border border-rose-900/60 bg-rose-950/40 px-3.5 py-2.5 text-[13px] text-rose-300">
          Could not reach the game's dev console: {error}
        </div>
      )}

      <div className="overflow-hidden rounded-lg border border-white/[0.06] bg-white/[0.02]">
        <table className="w-full text-[13px]">
          <thead>
            <tr className="border-b border-white/[0.06] text-left text-zinc-500">
              <th className="px-4 py-2.5 font-medium">Resource</th>
              <th className="px-4 py-2.5 font-medium">Status</th>
              <th className="px-4 py-2.5 text-right font-medium">Actions</th>
            </tr>
          </thead>
          <tbody>
            {resources.length === 0 && (
              <tr>
                <td className="px-4 py-8 text-center text-zinc-600" colSpan={3}>
                  No resources found.
                </td>
              </tr>
            )}
            {resources.map((resource) => (
              <tr key={resource.name} className="border-b border-white/[0.04] last:border-0">
                <td className="px-4 py-3 font-medium text-zinc-200">{resource.name}</td>
                <td className="px-4 py-3">
                  <span
                    className={
                      'inline-flex items-center gap-1.5 ' +
                      (resource.status === 'started' ? 'text-emerald-400' : 'text-zinc-500')
                    }
                  >
                    <span
                      className={
                        'h-1.5 w-1.5 rounded-full ' +
                        (resource.status === 'started' ? 'bg-emerald-400' : 'bg-zinc-600')
                      }
                    />
                    {resource.status}
                  </span>
                </td>
                <td className="px-4 py-3">
                  <div className="flex justify-end gap-2">
                    {(['ensure', 'restart', 'stop'] as const).map((action) => (
                      <button
                        key={action}
                        type="button"
                        disabled={pending === `${resource.name}:${action}`}
                        onClick={() => act(resource.name, action)}
                        className={
                          'rounded-md px-2.5 py-1 text-[12px] font-medium capitalize text-white transition-colors disabled:opacity-50 enabled:cursor-pointer ' +
                          actionStyle[action]
                        }
                      >
                        {action}
                      </button>
                    ))}
                  </div>
                </td>
              </tr>
            ))}
          </tbody>
        </table>
      </div>
    </div>
  )
}
