import { useState, useMemo } from 'react';
import { BookOpen, List } from 'lucide-react';
import luaNativesRaw from './data/lua_natives.json';
import type { LuaNativesData, LuaNative, NativeSide } from './types/lua-natives';
import { Header } from './components/Header';
import { NativesSidebar } from './components/NativesSidebar';
import { NativeDetail } from './components/NativeDetail';
import { ReferencePanel } from './components/ReferencePanel';

// This site shows ONLY the Lua scripting API (sk8-engine's FiveM-shaped
// surface for resources). The retail reverse-engineering catalogue
// (natives_export.json) and the sk3o console command reference
// (commands.json) live alongside this file on disk but are deliberately not
// rendered here - they are a different kind of document, not more of this
// one.
const luaNatives = luaNativesRaw as LuaNativesData;
const allEntries: LuaNative[] = luaNatives.natives;

type View = 'natives' | 'reference';

export function App() {
  const [view, setView] = useState<View>('natives');
  const [searchQuery, setSearchQuery] = useState('');
  const [activeCategory, setActiveCategory] = useState('All');
  const [activeSide, setActiveSide] = useState<NativeSide | 'All'>('All');
  const [selectedEntry, setSelectedEntry] = useState<LuaNative | null>(allEntries[0] || null);

  // Category is the functional grouping the JSON already assigns (Events,
  // NUI, Input, ...) - not which side of the connection a native runs on.
  const categories = useMemo(() => {
    const set = new Set<string>();
    allEntries.forEach((e) => set.add(e.category));
    return Array.from(set).sort();
  }, []);

  const filteredEntries = useMemo(() => {
    const q = searchQuery.toLowerCase();
    return allEntries.filter((entry) => {
      const matchesCategory = activeCategory === 'All' || entry.category === activeCategory;
      const matchesSide = activeSide === 'All' || entry.side === activeSide;
      const matchesSearch =
        q === '' ||
        entry.name.toLowerCase().includes(q) ||
        entry.summary.toLowerCase().includes(q) ||
        entry.signature.toLowerCase().includes(q) ||
        (entry.notes ?? '').toLowerCase().includes(q);
      return matchesCategory && matchesSide && matchesSearch;
    });
  }, [searchQuery, activeCategory, activeSide]);

  return (
    <div className="h-screen bg-[#0f1013] flex flex-col font-sans antialiased text-gray-200 overflow-hidden">
      <Header
        searchQuery={searchQuery}
        setSearchQuery={setSearchQuery}
        activeCategory={activeCategory}
        setActiveCategory={setActiveCategory}
        categories={categories}
        activeSide={activeSide}
        setActiveSide={setActiveSide}
        total={allEntries.length}
        shown={filteredEntries.length}
      />

      <div className="flex items-center gap-1 px-6 py-1.5 bg-[#121316] border-b border-[#24272e] text-xs">
        <button
          onClick={() => setView('natives')}
          className={`flex items-center gap-1.5 px-2 py-1 rounded transition ${
            view === 'natives' ? 'bg-[#20242d] text-white' : 'text-gray-400 hover:text-white'
          }`}
        >
          <List className="h-3.5 w-3.5" />
          Natives
        </button>
        <button
          onClick={() => setView('reference')}
          className={`flex items-center gap-1.5 px-2 py-1 rounded transition ${
            view === 'reference' ? 'bg-[#20242d] text-white' : 'text-gray-400 hover:text-white'
          }`}
        >
          <BookOpen className="h-3.5 w-3.5" />
          Reference
        </button>
      </div>

      <div className="flex flex-1 min-h-0 overflow-hidden">
        {view === 'natives' ? (
          <>
            <NativeDetail entry={selectedEntry} />
            <NativesSidebar
              entries={filteredEntries}
              selectedEntry={selectedEntry}
              onSelectEntry={setSelectedEntry}
            />
          </>
        ) : (
          <ReferencePanel
            engineConventions={luaNatives.engine_conventions}
            controls={luaNatives.controls}
            sk8manifest={luaNatives.sk8manifest}
          />
        )}
      </div>
    </div>
  );
}

export default App;
