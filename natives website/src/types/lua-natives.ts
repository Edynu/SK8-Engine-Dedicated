// Shape of src/data/lua_natives.json - the Lua scripting API sk8-engine
// exposes to resources. Distinct from natives_export.json (retail
// reverse-engineering findings) and commands.json (the sk3o console) -
// nothing in here is a retail native or an engine console command.

export type NativeSide = 'client' | 'server' | 'shared';

export interface LuaNative {
  name: string;
  side: NativeSide;
  signature: string;
  summary: string;
  category: string;
  notes?: string;
  deprecated?: boolean;
}

export interface LuaControls {
  gamepad_buttons: string[];
  gamepad_aliases: Record<string, string>;
  gamepad_axes: string[];
  keyboard: string[];
  notes: string[];
}

export interface LuaManifest {
  fields: string[];
  notes: string[];
}

export interface LuaNativesData {
  $schema_notes?: string;
  generated_at?: string;
  generated_by?: string;
  engine_conventions?: string[];
  counts?: { total: number; client: number; server: number; shared: number };
  natives: LuaNative[];
  controls?: LuaControls;
  sk8manifest?: LuaManifest;
}
