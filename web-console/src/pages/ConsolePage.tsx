import { useEffect, useRef, useState } from 'react'
import { execConsoleLine, fetchConsoleLog, type LogLine } from '../api'

export function ConsolePage() {
  const [lines, setLines] = useState<LogLine[]>([])
  const [input, setInput] = useState('')
  const [history, setHistory] = useState<string[]>([])
  const [historyIndex, setHistoryIndex] = useState<number | null>(null)
  const cursorRef = useRef(0)
  const logRef = useRef<HTMLDivElement | null>(null)
  const stickToBottomRef = useRef(true)

  useEffect(() => {
    let cancelled = false
    const poll = async () => {
      try {
        const data = await fetchConsoleLog(cursorRef.current)
        if (cancelled) return
        cursorRef.current = data.cursor
        if (data.lines.length > 0) {
          setLines((prev) => [...prev, ...data.lines].slice(-2000))
        }
      } catch {
        // Dev-only tool; a failed poll just retries next tick.
      }
    }
    poll()
    const id = setInterval(poll, 300)
    return () => {
      cancelled = true
      clearInterval(id)
    }
  }, [])

  useEffect(() => {
    if (stickToBottomRef.current && logRef.current) {
      logRef.current.scrollTop = logRef.current.scrollHeight
    }
  }, [lines])

  const handleScroll = () => {
    const el = logRef.current
    if (!el) return
    stickToBottomRef.current = el.scrollHeight - el.scrollTop - el.clientHeight < 24
  }

  const submit = async () => {
    const line = input.trim()
    if (!line) return
    setHistory((prev) => [...prev, line])
    setHistoryIndex(null)
    setInput('')
    await execConsoleLine(line)
  }

  const onKeyDown = (event: React.KeyboardEvent<HTMLInputElement>) => {
    if (event.key === 'Enter') {
      event.preventDefault()
      submit()
    } else if (event.key === 'ArrowUp') {
      event.preventDefault()
      if (history.length === 0) return
      const next = historyIndex === null ? history.length - 1 : Math.max(0, historyIndex - 1)
      setHistoryIndex(next)
      setInput(history[next])
    } else if (event.key === 'ArrowDown') {
      event.preventDefault()
      if (historyIndex === null) return
      const next = historyIndex + 1
      if (next >= history.length) {
        setHistoryIndex(null)
        setInput('')
      } else {
        setHistoryIndex(next)
        setInput(history[next])
      }
    }
  }

  return (
    <div className="flex h-full flex-col">
      <div
        ref={logRef}
        onScroll={handleScroll}
        className="flex-1 overflow-y-auto px-5 py-4 font-mono text-[13px] leading-relaxed"
      >
        {lines.length === 0 && (
          <p className="text-zinc-600">Waiting for output&hellip;</p>
        )}
        {lines.map((line, i) => (
          <div key={i} className="whitespace-pre-wrap break-words py-0.5">
            {line.resource && (
              <span className="mr-1.5 text-orange-400">[{line.resource}]</span>
            )}
            <span
              className={
                !line.resource && line.text.startsWith('[console]')
                  ? 'text-amber-300'
                  : 'text-zinc-300'
              }
            >
              {line.text}
            </span>
          </div>
        ))}
      </div>
      <div className="flex items-center gap-2 border-t border-white/[0.06] bg-white/[0.02] px-5 py-3">
        <span className="font-mono text-[13px] text-emerald-400">&gt;</span>
        <input
          autoFocus
          value={input}
          onChange={(e) => setInput(e.target.value)}
          onKeyDown={onKeyDown}
          spellCheck={false}
          autoComplete="off"
          placeholder="Type a command or cvar&hellip;"
          className="flex-1 bg-transparent font-mono text-[13px] text-zinc-100 placeholder:text-zinc-600 focus:outline-none"
        />
      </div>
    </div>
  )
}
