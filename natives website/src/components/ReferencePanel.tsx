import React from 'react';
import type { LuaControls, LuaManifest } from '../types/lua-natives';

interface ReferencePanelProps {
  engineConventions?: string[];
  controls?: LuaControls;
  sk8manifest?: LuaManifest;
}

export const ReferencePanel: React.FC<ReferencePanelProps> = ({
  engineConventions,
  controls,
  sk8manifest,
}) => {
  return (
    <div className="flex-1 min-h-0 bg-[#0f1013] text-[#c0c5ce] p-8 overflow-y-auto h-full">
      <h1 className="text-2xl font-bold text-white mb-6">Reference</h1>

      {engineConventions && engineConventions.length > 0 && (
        <section className="mb-8">
          <h3 className="text-xs font-semibold text-gray-400 uppercase tracking-wider mb-2">
            Engine Conventions
          </h3>
          <ul className="bg-[#14161b] p-4 rounded border border-[#1f232b] text-sm text-gray-300 space-y-2 list-disc list-inside">
            {engineConventions.map((note, i) => (
              <li key={i}>{note}</li>
            ))}
          </ul>
        </section>
      )}

      {controls && (
        <section className="mb-8">
          <h3 className="text-xs font-semibold text-gray-400 uppercase tracking-wider mb-2">
            Control Names
          </h3>
          <div className="bg-[#14161b] p-4 rounded border border-[#1f232b] text-sm text-gray-300 space-y-3">
            <div>
              <span className="text-orange-500 font-mono text-xs uppercase">Gamepad buttons</span>
              <div className="font-mono text-xs text-gray-300 mt-1 flex flex-wrap gap-1.5">
                {controls.gamepad_buttons.map((c) => (
                  <span key={c} className="bg-[#1c1f26] border border-[#2b303c] rounded px-1.5 py-0.5">
                    {c}
                  </span>
                ))}
              </div>
            </div>
            <div>
              <span className="text-orange-500 font-mono text-xs uppercase">D-pad aliases</span>
              <div className="font-mono text-xs text-gray-300 mt-1">
                {Object.entries(controls.gamepad_aliases).map(([alias, target]) => (
                  <div key={alias}>
                    {alias} <span className="text-gray-500">-&gt;</span> {target}
                  </div>
                ))}
              </div>
            </div>
            <div>
              <span className="text-orange-500 font-mono text-xs uppercase">Gamepad axes</span>
              <div className="font-mono text-xs text-gray-300 mt-1 flex flex-wrap gap-1.5">
                {controls.gamepad_axes.map((c) => (
                  <span key={c} className="bg-[#1c1f26] border border-[#2b303c] rounded px-1.5 py-0.5">
                    {c}
                  </span>
                ))}
              </div>
            </div>
            <div>
              <span className="text-orange-500 font-mono text-xs uppercase">Keyboard</span>
              <div className="font-mono text-xs text-gray-300 mt-1 flex flex-wrap gap-1.5">
                {controls.keyboard.map((c) => (
                  <span key={c} className="bg-[#1c1f26] border border-[#2b303c] rounded px-1.5 py-0.5">
                    {c}
                  </span>
                ))}
              </div>
            </div>
            {controls.notes.length > 0 && (
              <ul className="list-disc list-inside space-y-1 text-gray-300 pt-1 border-t border-[#1f232b]">
                {controls.notes.map((note, i) => (
                  <li key={i}>{note}</li>
                ))}
              </ul>
            )}
          </div>
        </section>
      )}

      {sk8manifest && (
        <section className="mb-8">
          <h3 className="text-xs font-semibold text-gray-400 uppercase tracking-wider mb-2">
            sk8manifest.lua
          </h3>
          <div className="bg-[#14161b] p-4 rounded border border-[#1f232b] text-sm text-gray-300 space-y-3">
            <div className="font-mono text-xs flex flex-wrap gap-1.5">
              {sk8manifest.fields.map((f) => (
                <span key={f} className="bg-[#1c1f26] border border-[#2b303c] rounded px-1.5 py-0.5">
                  {f}
                </span>
              ))}
            </div>
            <ul className="list-disc list-inside space-y-1">
              {sk8manifest.notes.map((note, i) => (
                <li key={i}>{note}</li>
              ))}
            </ul>
          </div>
        </section>
      )}
    </div>
  );
};
