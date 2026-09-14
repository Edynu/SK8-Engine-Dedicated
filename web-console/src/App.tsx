import { useEffect, useState } from 'react'
import { fetchResources } from './api'
import { Sidebar, type Page } from './components/Sidebar'
import { DashboardPage } from './pages/DashboardPage'
import { ConsolePage } from './pages/ConsolePage'
import { PlayersPage } from './pages/PlayersPage'
import { ScriptsPage } from './pages/ScriptsPage'

const titles: Record<Page, string> = {
  dashboard: 'Dashboard',
  console: 'Console',
  players: 'Active Players',
  scripts: 'Scripts',
}

function App() {
  const [page, setPage] = useState<Page>('dashboard')
  const [connected, setConnected] = useState(true)

  // Runs independently of which page is showing, so the sidebar's status
  // dot stays accurate even while ScriptsPage (which also reports
  // connectivity, since it already polls) is unmounted.
  useEffect(() => {
    let cancelled = false
    const check = async () => {
      try {
        await fetchResources()
        if (!cancelled) setConnected(true)
      } catch {
        if (!cancelled) setConnected(false)
      }
    }
    check()
    const id = setInterval(check, 3000)
    return () => {
      cancelled = true
      clearInterval(id)
    }
  }, [])

  return (
    <div className="flex h-screen bg-[#0a0b0e] text-zinc-100 antialiased">
      <Sidebar active={page} onSelect={setPage} connected={connected} />
      <main className="flex min-w-0 flex-1 flex-col">
        {page === 'dashboard' && (
          <div className="h-full overflow-y-auto">
            <DashboardPage />
          </div>
        )}
        {page === 'console' && (
          <div className="flex h-full flex-col">
            <header className="border-b border-white/[0.06] px-5 py-3.5">
              <h1 className="text-[13px] font-semibold text-zinc-200">{titles.console}</h1>
            </header>
            <div className="min-h-0 flex-1">
              <ConsolePage />
            </div>
          </div>
        )}
        {page === 'players' && (
          <div className="h-full overflow-y-auto">
            <PlayersPage />
          </div>
        )}
        {page === 'scripts' && (
          <div className="h-full overflow-y-auto">
            <ScriptsPage onConnectivityChange={setConnected} />
          </div>
        )}
      </main>
    </div>
  )
}

export default App
