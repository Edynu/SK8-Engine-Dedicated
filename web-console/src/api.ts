// Thin fetch helpers over the admin HTTP API (skate3_lua_admin_http.cpp).
// No auth, no error recovery beyond surfacing the failure - this dashboard
// only ever talks to a local dev/relay process on the same machine.

export type ResourceStatus = 'started' | 'stopped'

export interface ResourceInfo {
  name: string
  status: ResourceStatus
}

export interface LogLine {
  resource: string
  text: string
  ts: number
}

export interface Player {
  id: number
  name: string
  valid: boolean
  x: number
  y: number
  z: number
  mapHash: string
  bucket: number
  idleMs: number
}

export async function fetchResources(): Promise<ResourceInfo[]> {
  const res = await fetch('/api/resources')
  if (!res.ok) throw new Error(`HTTP ${res.status}`)
  return (await res.json()) as ResourceInfo[]
}

export async function actOnResource(
  name: string,
  action: 'ensure' | 'restart' | 'stop',
): Promise<void> {
  await fetch(`/api/resources/${encodeURIComponent(name)}/${action}`, { method: 'POST' })
}

export async function fetchPlayers(): Promise<Player[]> {
  const res = await fetch('/api/players')
  if (!res.ok) throw new Error(`HTTP ${res.status}`)
  return (await res.json()) as Player[]
}

export async function fetchConsoleLog(
  since: number,
): Promise<{ cursor: number; lines: LogLine[] }> {
  const res = await fetch(`/api/console/log?since=${since}`)
  if (!res.ok) throw new Error(`HTTP ${res.status}`)
  return (await res.json()) as { cursor: number; lines: LogLine[] }
}

export async function execConsoleLine(line: string): Promise<void> {
  await fetch('/api/console/exec', { method: 'POST', body: line })
}

// --- /api/metrics ---------------------------------------------------
//
// Served by both skate3.exe and skate3_dedicated.exe, but not with the
// same shape: only the relay sees its own loop's bandwidth and hitches, so
// `relay` is present on the dedicated server and `network` (this client's
// own upload/download) on the game client instead. `resources` (Lua
// resmon) is always present - LuaScriptHost lives in both processes.
// See docs/metrics.md for the full field-by-field writeup.

export interface RelayMetrics {
  bytesInPerSec: number
  bytesOutPerSec: number
  hitchesPerSec: number
  worstIterationMs: number
  hitchesTotal: number
  bytesInTotal: number
  bytesOutTotal: number
}

export interface NetworkMetrics {
  enabled: boolean
  role: number
  knownPeers: number
  visiblePlayers: number
  uploadKibPerSec: number
  downloadKibPerSec: number
  uploadPacketsPerSec: number
  downloadPacketsPerSec: number
  totalUploadBytes: number
  totalDownloadBytes: number
}

export interface ResourceMetrics {
  name: string
  msLastSecond: number
  callsLastSecond: number
  msLifetime: number
  callsLifetime: number
  memoryKb: number
}

export interface Metrics {
  relay?: RelayMetrics
  network?: NetworkMetrics
  resources: ResourceMetrics[]
}

export async function fetchMetrics(): Promise<Metrics> {
  const res = await fetch('/api/metrics')
  if (!res.ok) throw new Error(`HTTP ${res.status}`)
  const data = (await res.json()) as Partial<Metrics>
  return { resources: [], ...data }
}
