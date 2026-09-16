import React from 'react';
import { Search } from 'lucide-react';
import type { NativeSide } from '../types/lua-natives';

interface HeaderProps {
  searchQuery: string;
  setSearchQuery: (q: string) => void;
  activeCategory: string;
  setActiveCategory: (cat: string) => void;
  categories: string[];
  activeSide: NativeSide | 'All';
  setActiveSide: (side: NativeSide | 'All') => void;
  total: number;
  shown: number;
}

const SIDES: (NativeSide | 'All')[] = ['All', 'client', 'server', 'shared'];

export const Header: React.FC<HeaderProps> = ({
  searchQuery,
  setSearchQuery,
  activeCategory,
  setActiveCategory,
  categories,
  activeSide,
  setActiveSide,
  total,
  shown,
}) => {
  return (
    <header className="bg-[#121316] text-[#c0c5ce] border-b border-[#24272e] sticky top-0 z-50">
      <div className="flex items-center justify-between px-6 h-14">
        <div className="flex items-center space-x-2">
          <span className="text-orange-500 font-bold text-xl">S3</span>
          <span className="font-semibold text-lg text-white">sk8-engine Lua Natives</span>
          <span className="text-xs text-gray-500 font-mono ml-2">
            {shown}/{total}
          </span>
        </div>

        <div className="relative w-80">
          <Search className="absolute left-3 top-2.5 h-4 w-4 text-gray-500" />
          <input
            type="text"
            placeholder="Search natives"
            value={searchQuery}
            onChange={(e) => setSearchQuery(e.target.value)}
            className="w-full bg-[#1c1f26] border border-[#2b303c] rounded pl-9 pr-4 py-1.5 text-xs text-white placeholder-gray-500 focus:outline-none focus:border-orange-500"
          />
        </div>
      </div>

      {/* Category is a FUNCTIONAL grouping (Events, NUI, Input, ...), not
          which side of the connection a native runs on - that lives in the
          separate side filter to the right instead of being conflated with
          category the way client/server used to be. */}
      <div className="flex items-center justify-between px-6 py-2 bg-[#181a20] border-t border-[#21252e] text-xs gap-4">
        <div className="flex items-center space-x-4 flex-wrap gap-y-1 min-w-0">
          <span className="text-gray-400 font-semibold shrink-0">Category:</span>
          <button
            onClick={() => setActiveCategory('All')}
            className={`px-2 py-0.5 rounded ${
              activeCategory === 'All' ? 'bg-orange-600 text-white' : 'text-gray-400 hover:text-white'
            }`}
          >
            ALL
          </button>
          {categories.map((cat) => (
            <button
              key={cat}
              onClick={() => setActiveCategory(cat)}
              className={`px-2 py-0.5 rounded uppercase whitespace-nowrap ${
                activeCategory === cat ? 'bg-orange-600 text-white' : 'text-gray-400 hover:text-white'
              }`}
            >
              {cat}
            </button>
          ))}
        </div>

        <div className="flex items-center space-x-2 shrink-0">
          <span className="text-gray-400 font-semibold">Side:</span>
          {SIDES.map((side) => (
            <button
              key={side}
              onClick={() => setActiveSide(side)}
              className={`px-2 py-0.5 rounded uppercase ${
                activeSide === side ? 'bg-orange-600 text-white' : 'text-gray-400 hover:text-white'
              }`}
            >
              {side}
            </button>
          ))}
        </div>
      </div>
    </header>
  );
};
