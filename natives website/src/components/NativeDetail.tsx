import React from 'react';
import type { LuaNative } from '../types/lua-natives';

interface NativeDetailProps {
  entry: LuaNative | null;
}

const sideStyle = (side: string) => {
  switch (side) {
    case 'client':
      return 'bg-sky-950 text-sky-400 border-sky-800';
    case 'server':
      return 'bg-emerald-950 text-emerald-400 border-emerald-800';
    default:
      return 'bg-gray-800 text-gray-300 border-gray-600';
  }
};

// "Fn(args) -> ret" -> { call: "Fn(args)", returns: "ret" }. Signatures with
// no arrow (a bare statement, e.g. "GlobalState.key = value") are left
// whole in `call`.
function splitSignature(signature: string): { call: string; returns: string | null } {
  const arrow = signature.indexOf('->');
  if (arrow === -1) {
    return { call: signature, returns: null };
  }
  return {
    call: signature.slice(0, arrow).trim(),
    returns: signature.slice(arrow + 2).trim(),
  };
}

export const NativeDetail: React.FC<NativeDetailProps> = ({ entry }) => {
  if (!entry) {
    return (
      <div className="flex-1 min-h-0 bg-[#0f1013] text-gray-500 flex items-center justify-center p-8">
        Select a native from the list to view detailed information.
      </div>
    );
  }

  const { call, returns } = splitSignature(entry.signature);

  return (
    <div className="flex-1 min-h-0 bg-[#0f1013] text-[#c0c5ce] p-8 overflow-y-auto h-full">
      {/* Title & Metadata Header */}
      <div className="mb-6 border-b border-[#24272e] pb-4">
        <div className="flex items-center gap-3 flex-wrap">
          <h1 className="text-2xl font-bold font-mono text-white">{entry.name}</h1>
          <span
            className={`text-xs px-2 py-0.5 rounded border font-mono uppercase self-center ${sideStyle(
              entry.side
            )}`}
          >
            {entry.side}
          </span>
          {entry.deprecated && (
            <span className="text-xs px-2 py-0.5 rounded bg-red-950 text-red-400 border border-red-800 font-mono uppercase self-center">
              deprecated
            </span>
          )}
        </div>
        <div className="text-sm font-mono text-orange-500 mt-1">{entry.category}</div>
      </div>

      {/* Call signature */}
      <div className="bg-[#16181d] border border-[#24272e] rounded-md p-4 mb-6 font-mono text-sm overflow-x-auto">
        <div className="text-gray-500 mb-2 text-xs">// Signature</div>
        <div className="text-white whitespace-pre-wrap">{call}</div>
        {returns && (
          <div className="text-gray-400 mt-2 text-xs">
            <span className="text-sky-400">Returns:</span> {returns}
          </div>
        )}
      </div>

      {/* Summary */}
      <div className="mb-8">
        <h3 className="text-xs font-semibold text-gray-400 uppercase tracking-wider mb-2">
          Description
        </h3>
        <p className="text-sm text-gray-300 leading-relaxed bg-[#14161b] p-4 rounded border border-[#1f232b]">
          {entry.summary}
        </p>
      </div>

      {/* Notes */}
      {entry.notes && (
        <div className="mb-8">
          <h3 className="text-xs font-semibold text-gray-400 uppercase tracking-wider mb-2">
            Notes
          </h3>
          <p className="text-sm text-gray-300 leading-relaxed bg-[#14161b] p-4 rounded border border-[#1f232b]">
            {entry.notes}
          </p>
        </div>
      )}
    </div>
  );
};
