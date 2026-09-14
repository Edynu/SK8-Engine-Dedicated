export type Page = 'dashboard' | 'console' | 'players' | 'scripts'

interface NavItem {
  page: Page
  label: string
  icon: React.ReactNode
}

const items: NavItem[] = [
  {
    page: 'dashboard',
    label: 'Dashboard',
    icon: (
      <svg viewBox="0 0 20 20" fill="none" className="h-4.5 w-4.5">
        <rect x="2.5" y="2.5" width="6.5" height="6.5" rx="1" stroke="currentColor" strokeWidth="1.5" />
        <rect x="11" y="2.5" width="6.5" height="4" rx="1" stroke="currentColor" strokeWidth="1.5" />
        <rect x="11" y="8.5" width="6.5" height="9" rx="1" stroke="currentColor" strokeWidth="1.5" />
        <rect x="2.5" y="11" width="6.5" height="6.5" rx="1" stroke="currentColor" strokeWidth="1.5" />
      </svg>
    ),
  },
  {
    page: 'console',
    label: 'Console',
    icon: (
      <svg viewBox="0 0 20 20" fill="none" className="h-4.5 w-4.5">
        <rect x="2" y="3" width="16" height="14" rx="1.5" stroke="currentColor" strokeWidth="1.5" />
        <path d="M5.5 8l2.5 2-2.5 2" stroke="currentColor" strokeWidth="1.5" strokeLinecap="round" strokeLinejoin="round" />
        <path d="M10.5 12.5h4" stroke="currentColor" strokeWidth="1.5" strokeLinecap="round" />
      </svg>
    ),
  },
  {
    page: 'players',
    label: 'Active Players',
    icon: (
      <svg viewBox="0 0 20 20" fill="none" className="h-4.5 w-4.5">
        <circle cx="7" cy="7" r="2.5" stroke="currentColor" strokeWidth="1.5" />
        <path d="M2.5 16c0-2.5 2-4 4.5-4s4.5 1.5 4.5 4" stroke="currentColor" strokeWidth="1.5" strokeLinecap="round" />
        <circle cx="14.5" cy="6.5" r="2" stroke="currentColor" strokeWidth="1.5" />
        <path d="M12.5 10c1.8.2 3 1.3 3.5 3" stroke="currentColor" strokeWidth="1.5" strokeLinecap="round" />
      </svg>
    ),
  },
  {
    page: 'scripts',
    label: 'Scripts',
    icon: (
      <svg viewBox="0 0 20 20" fill="none" className="h-4.5 w-4.5">
        <path d="M6 2.5h5.5L15 6v11.5H6z" stroke="currentColor" strokeWidth="1.5" strokeLinejoin="round" />
        <path d="M11.5 2.5V6H15" stroke="currentColor" strokeWidth="1.5" strokeLinejoin="round" />
        <path d="M8 10.5h5M8 13h5" stroke="currentColor" strokeWidth="1.5" strokeLinecap="round" />
      </svg>
    ),
  },
]

export function Sidebar({
  active,
  onSelect,
  connected,
}: {
  active: Page
  onSelect: (page: Page) => void
  connected: boolean
}) {
  return (
    <aside className="flex w-60 shrink-0 flex-col border-r border-white/[0.06] bg-[#0c0e12]">
      <div className="flex items-center gap-2.5 px-5 py-5">
        <div className="flex h-8 w-8 items-center justify-center rounded-md bg-orange-500/15 text-orange-400">
          <svg viewBox="0 0 20 20" fill="none" className="h-4.5 w-4.5">
            <circle cx="6" cy="15" r="2" stroke="currentColor" strokeWidth="1.5" />
            <circle cx="14" cy="15" r="2" stroke="currentColor" strokeWidth="1.5" />
            <path d="M3 13l4-6h6l4 6" stroke="currentColor" strokeWidth="1.5" strokeLinecap="round" strokeLinejoin="round" />
            <path d="M8.5 7l2-3.5" stroke="currentColor" strokeWidth="1.5" strokeLinecap="round" />
          </svg>
        </div>
        <div>
          <div className="text-[13px] font-semibold leading-tight text-zinc-100">Skate 3</div>
          <div className="text-[11px] leading-tight text-zinc-500">Server Console</div>
        </div>
      </div>

      <nav className="flex flex-1 flex-col gap-0.5 px-3">
        {items.map((item) => {
          const isActive = item.page === active
          return (
            <button
              key={item.page}
              type="button"
              onClick={() => onSelect(item.page)}
              className={
                'flex items-center gap-2.5 rounded-md px-3 py-2 text-left text-[13px] font-medium transition-colors ' +
                (isActive
                  ? 'bg-orange-500/10 text-orange-400'
                  : 'text-zinc-400 hover:bg-white/[0.04] hover:text-zinc-200')
              }
            >
              <span className={isActive ? 'text-orange-400' : 'text-zinc-500'}>{item.icon}</span>
              {item.label}
            </button>
          )
        })}
      </nav>

      <div className="mx-3 mb-4 flex items-center gap-2 rounded-md border border-white/[0.06] bg-white/[0.02] px-3 py-2.5">
        <span
          className={
            'h-1.5 w-1.5 shrink-0 rounded-full ' + (connected ? 'bg-orange-400' : 'bg-rose-500')
          }
        />
        <span className="text-[12px] text-zinc-400">
          {connected ? 'Connected' : 'Unreachable'}
        </span>
      </div>
    </aside>
  )
}
