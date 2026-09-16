import React from 'react';
import type { LuaNative } from '../types/lua-natives';

interface NativesSidebarProps {
  entries: LuaNative[];
  selectedEntry: LuaNative | null;
  onSelectEntry: (entry: LuaNative) => void;
}

const sideStyle = (side: string) => {
  switch (side) {
    case 'client':
      return 'border-sky-600 text-sky-400';
    case 'server':
      return 'border-emerald-600 text-emerald-400';
    default:
      return 'border-gray-500 text-gray-400';
  }
};

export const NativesSidebar: React.FC<NativesSidebarProps> = ({
  entries,
  selectedEntry,
  onSelectEntry,
}) => {
  // Grouped by category rather than flattened, so the list itself doubles as
  // a table of contents - the ask this rewrite exists for: category is a
  // functional grouping (Events, NUI, Input, ...), not which side of the
  // connection a native runs on.
  const groups = new Map<string, LuaNative[]>();
  for (const entry of entries) {
    const list = groups.get(entry.category) ?? [];
    list.push(entry);
    groups.set(entry.category, list);
  }

  return (
    <div className="w-[40%] shrink-0 bg-[#16181d] border-l border-[#24272e] flex flex-col h-full min-h-0 overflow-y-auto">
      <div className="py-2">
        {groups.size === 0 && (
          <div className="p-4 text-center text-xs text-gray-500">No matching natives found.</div>
        )}
        {Array.from(groups.entries()).map(([category, list]) => (
          <div key={category} className="mb-1">
            <div className="px-4 pt-3 pb-1 text-[10px] font-semibold uppercase tracking-wider text-gray-500 sticky top-0 bg-[#16181d]">
              {category}
            </div>
            {list.map((entry) => {
              const isSelected = selectedEntry?.name === entry.name && selectedEntry?.side === entry.side;
              return (
                <button
                  key={`${entry.side}:${entry.name}`}
                  onClick={() => onSelectEntry(entry)}
                  className={`w-full text-left px-4 py-2 text-xs flex items-center justify-between font-mono hover:bg-[#20242d] transition ${
                    isSelected
                      ? 'bg-orange-900/40 text-orange-300 font-semibold border-l-4 border-orange-500 pl-3'
                      : 'text-gray-300'
                  }`}
                >
                  <div className="truncate flex-1 pr-2">
                    <span className="block truncate">
                      {entry.name}
                      {entry.deprecated && (
                        <span className="ml-1.5 text-[9px] text-red-400 font-sans normal-case">deprecated</span>
                      )}
                    </span>
                  </div>
                  <span
                    className={`text-[9px] px-1.5 py-0.5 border rounded uppercase shrink-0 ${sideStyle(
                      entry.side
                    )}`}
                  >
                    {entry.side}
                  </span>
                </button>
              );
            })}
          </div>
        ))}
      </div>
    </div>
  );
};
