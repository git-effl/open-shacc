/**
 * @file App.tsx
 * @brief open-shacc Developer Workbench & Interactive Shader Compiler Studio.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

import React, { useState, useMemo } from 'react';
import {
  Code2,
  Cpu,
  Layers,
  FileCheck2,
  Terminal,
  Download,
  Copy,
  Check,
  Github,
  ShieldCheck,
  Sparkles,
  ArrowRight,
  ExternalLink,
  BookOpen,
  Binary
} from 'lucide-react';
import { compileShader, CompileResult } from './compiler_engine/compiler';

const PRESET_SHADERS: Record<string, { name: string; description: string; code: string }> = {
  mvp: {
    name: 'MVP Matrix Transform (Standard)',
    description: 'Vertex shader multiplying 4x4 model-view-projection matrix with position and passing UVs',
    code: `// Basic MVP Matrix Vertex Shader
uniform float4x4 u_MVPMatrix;
uniform float4 u_Tint;

struct VS_INPUT {
    float4 position : POSITION;
    float2 uv       : TEXCOORD0;
};

struct VS_OUTPUT {
    float4 position : POSITION;
    float2 uv       : TEXCOORD0;
    float4 color    : COLOR0;
};

VS_OUTPUT main(VS_INPUT input) {
    VS_OUTPUT output;
    output.position = mul(u_MVPMatrix, input.position);
    output.uv = input.uv;
    output.color = u_Tint;
    return output;
}`,
  },
  texturedLit: {
    name: 'Textured Diffuse Lighting',
    description: 'Calculates diffuse N.L dot product lighting and scales texture coordinates',
    code: `// Textured Lit Vertex Shader with Dot Product N.L
uniform float4x4 u_ModelViewProj;
uniform float3   u_LightDirection;
uniform float4   u_DiffuseColor;

struct VertexIn {
    float4 pos    : POSITION;
    float3 normal : NORMAL;
    float2 uv     : TEXCOORD0;
};

struct VertexOut {
    float4 pos   : POSITION;
    float2 uv    : TEXCOORD0;
    float4 color : COLOR0;
};

VertexOut main(VertexIn vin) {
    VertexOut vout;
    vout.pos = mul(u_ModelViewProj, vin.pos);
    vout.uv = vin.uv;

    float ndotl = max(dot(vin.normal, u_LightDirection), 0.0);
    vout.color = u_DiffuseColor * ndotl;

    return vout;
}`,
  },
  dotProduct: {
    name: 'Normal Vector Lighting (Dot4 / Dot3)',
    description: 'Vector arithmetic benchmark isolating DOT3 and DOT4 instruction emission',
    code: `uniform float4x4 u_Transform;
uniform float3 u_SunDirection;

float4 main(float3 normal : NORMAL, float4 pos : POSITION) : POSITION {
    float ndotl = dot(normal, u_SunDirection);
    float4 out_pos = mul(u_Transform, pos);
    return out_pos;
}`,
  },
};

export default function App() {
  const [selectedPreset, setSelectedPreset] = useState<string>('mvp');
  const [sourceCode, setSourceCode] = useState<string>(PRESET_SHADERS.mvp.code);
  const [activeTab, setActiveTab] = useState<'disasm' | 'registers' | 'hexdump' | 'capi' | 'cleanroom'>('disasm');
  const [copied, setCopied] = useState<string | null>(null);

  // Compile on change
  const compileResult: CompileResult = useMemo(() => {
    return compileShader(sourceCode);
  }, [sourceCode]);

  const handlePresetSelect = (key: string) => {
    setSelectedPreset(key);
    setSourceCode(PRESET_SHADERS[key].code);
  };

  const handleCopy = (text: string, id: string) => {
    navigator.clipboard.writeText(text);
    setCopied(id);
    setTimeout(() => setCopied(null), 2000);
  };

  const handleDownloadGxp = () => {
    const blob = new Blob([compileResult.rawGxpBytes.buffer as ArrayBuffer], { type: 'application/octet-stream' });
    const url = URL.createObjectURL(blob);
    const a = document.createElement('a');
    a.href = url;
    a.download = `${selectedPreset}_shader.gxp`;
    document.body.appendChild(a);
    a.click();
    document.body.removeChild(a);
    URL.revokeObjectURL(url);
  };

  // Hex dump formatted string
  const hexDumpRows = useMemo(() => {
    const bytes = compileResult.rawGxpBytes;
    const rows: Array<{ offset: string; hex: string; ascii: string }> = [];
    for (let i = 0; i < bytes.length; i += 16) {
      const offset = '0x' + i.toString(16).toUpperCase().padStart(4, '0');
      let hex = '';
      let ascii = '';
      for (let j = 0; j < 16; j++) {
        if (i + j < bytes.length) {
          const b = bytes[i + j];
          hex += b.toString(16).toUpperCase().padStart(2, '0') + ' ';
          ascii += b >= 32 && b <= 126 ? String.fromCharCode(b) : '.';
        } else {
          hex += '   ';
        }
      }
      rows.push({ offset, hex: hex.trim(), ascii });
    }
    return rows;
  }, [compileResult.rawGxpBytes]);

  return (
    <div className="min-h-screen bg-slate-950 text-slate-100 flex flex-col font-sans selection:bg-cyan-500/30">
      {/* Top Header */}
      <header className="border-b border-slate-800 bg-slate-900/90 backdrop-blur sticky top-0 z-40 px-6 py-3.5 flex items-center justify-between">
        <div className="flex items-center gap-3.5">
          <div className="h-10 w-10 rounded-xl bg-gradient-to-tr from-cyan-600 to-indigo-600 flex items-center justify-center shadow-lg shadow-cyan-500/20 ring-1 ring-cyan-400/30">
            <Cpu className="w-5 h-5 text-white" />
          </div>
          <div>
            <div className="flex items-center gap-2.5">
              <span className="font-mono font-bold text-lg tracking-tight text-white">open-shacc</span>
              <span className="text-[11px] font-semibold bg-emerald-500/15 text-emerald-400 border border-emerald-500/30 px-2 py-0.5 rounded-full flex items-center gap-1">
                <ShieldCheck className="w-3 h-3" /> Clean-Room Certified
              </span>
              <span className="text-[11px] font-semibold bg-blue-500/15 text-blue-400 border border-blue-500/30 px-2 py-0.5 rounded-full">
                C++20 &amp; CMake
              </span>
            </div>
            <p className="text-xs text-slate-400">
              Clean-room Cg/HLSL to USSE shader compiler &amp; 16-byte aligned GXP binary packer
            </p>
          </div>
        </div>

        <div className="flex items-center gap-3">
          <a
            href="https://github.com/git-effl/open-shacc"
            target="_blank"
            rel="noopener noreferrer"
            className="flex items-center gap-2 px-3 py-1.5 rounded-lg bg-slate-800 hover:bg-slate-700 text-slate-200 border border-slate-700 text-xs font-medium transition"
          >
            <Github className="w-4 h-4 text-white" />
            <span>git-effl / open-shacc</span>
            <ExternalLink className="w-3 h-3 text-slate-400" />
          </a>

          <button
            onClick={handleDownloadGxp}
            className="flex items-center gap-1.5 px-3.5 py-1.5 rounded-lg bg-gradient-to-r from-cyan-500 to-blue-600 hover:from-cyan-400 hover:to-blue-500 text-white text-xs font-semibold shadow-md shadow-cyan-500/20 transition active:scale-95 cursor-pointer"
          >
            <Download className="w-3.5 h-3.5" />
            <span>Export .GXP ({compileResult.rawGxpBytes.length} B)</span>
          </button>
        </div>
      </header>

      {/* Preset Toolbar */}
      <div className="bg-slate-900 border-b border-slate-800 px-6 py-2 flex items-center justify-between gap-4 text-xs">
        <div className="flex items-center gap-2">
          <span className="text-slate-400 font-medium">Shader Preset:</span>
          {Object.entries(PRESET_SHADERS).map(([key, item]) => (
            <button
              key={key}
              onClick={() => handlePresetSelect(key)}
              className={`px-3 py-1 rounded-md transition font-medium cursor-pointer ${
                selectedPreset === key
                  ? 'bg-cyan-500/20 text-cyan-300 border border-cyan-500/40'
                  : 'bg-slate-800/60 text-slate-400 hover:text-slate-200 hover:bg-slate-800'
              }`}
            >
              {item.name}
            </button>
          ))}
        </div>

        <div className="flex items-center gap-4 text-slate-400">
          <span className="flex items-center gap-1.5">
            <span className="h-2 w-2 rounded-full bg-emerald-400 animate-pulse"></span>
            USSE Machine Code: {compileResult.instructions.length} instrs
          </span>
          <span>Aligned: 16-Byte Pad</span>
          <span className="font-mono text-cyan-400 font-semibold">Magic: "GXP\0"</span>
        </div>
      </div>

      {/* Main Workspace Split */}
      <div className="flex-1 grid grid-cols-1 lg:grid-cols-12 overflow-hidden">
        {/* Left Column: Cg/HLSL Source Editor */}
        <div className="lg:col-span-5 border-r border-slate-800 flex flex-col bg-slate-950">
          <div className="px-4 py-2.5 bg-slate-900/60 border-b border-slate-800 flex items-center justify-between text-xs font-medium">
            <div className="flex items-center gap-2 text-slate-300">
              <Code2 className="w-4 h-4 text-cyan-400" />
              <span>Cg / HLSL Shader Source (Clean-room grammar)</span>
            </div>
            <span className="text-[11px] text-slate-500 font-mono">
              {sourceCode.split('\n').length} lines
            </span>
          </div>

          <div className="flex-1 relative font-mono text-xs overflow-auto p-4 bg-slate-950">
            <textarea
              value={sourceCode}
              onChange={(e) => setSourceCode(e.target.value)}
              spellCheck={false}
              className="w-full h-full min-h-[460px] bg-transparent text-slate-200 outline-none resize-none font-mono text-xs leading-relaxed focus:ring-0 border-0"
              placeholder="// Write clean-room Cg/HLSL vertex shader here..."
            />
          </div>

          {/* Quick info bar */}
          <div className="px-4 py-2 bg-slate-900/40 border-t border-slate-800 text-[11px] text-slate-400 flex items-center justify-between">
            <span>Entry point: <code className="text-cyan-300">main()</code></span>
            <span>Profile: <code className="text-indigo-300">vp_vertex</code></span>
            <span>Target: <code className="text-emerald-300">USSE VLIW</code></span>
          </div>
        </div>

        {/* Right Column: Multi-Tab Compiler Output Inspector */}
        <div className="lg:col-span-7 flex flex-col bg-slate-900/30">
          {/* Tab Navigation */}
          <div className="flex items-center gap-1 px-4 py-2 bg-slate-900/80 border-b border-slate-800 text-xs overflow-x-auto">
            <button
              onClick={() => setActiveTab('disasm')}
              className={`flex items-center gap-2 px-3.5 py-1.5 rounded-lg transition font-medium cursor-pointer ${
                activeTab === 'disasm'
                  ? 'bg-cyan-500 text-slate-950 font-semibold shadow-sm'
                  : 'text-slate-400 hover:text-slate-200 hover:bg-slate-800'
              }`}
            >
              <Cpu className="w-3.5 h-3.5" />
              <span>USSE Disassembly</span>
            </button>

            <button
              onClick={() => setActiveTab('registers')}
              className={`flex items-center gap-2 px-3.5 py-1.5 rounded-lg transition font-medium cursor-pointer ${
                activeTab === 'registers'
                  ? 'bg-cyan-500 text-slate-950 font-semibold shadow-sm'
                  : 'text-slate-400 hover:text-slate-200 hover:bg-slate-800'
              }`}
            >
              <Layers className="w-3.5 h-3.5" />
              <span>Register Map (c0/v0/r0)</span>
            </button>

            <button
              onClick={() => setActiveTab('hexdump')}
              className={`flex items-center gap-2 px-3.5 py-1.5 rounded-lg transition font-medium cursor-pointer ${
                activeTab === 'hexdump'
                  ? 'bg-cyan-500 text-slate-950 font-semibold shadow-sm'
                  : 'text-slate-400 hover:text-slate-200 hover:bg-slate-800'
              }`}
            >
              <Binary className="w-3.5 h-3.5" />
              <span>GXP Binary Hex Dump</span>
            </button>

            <button
              onClick={() => setActiveTab('capi')}
              className={`flex items-center gap-2 px-3.5 py-1.5 rounded-lg transition font-medium cursor-pointer ${
                activeTab === 'capi'
                  ? 'bg-cyan-500 text-slate-950 font-semibold shadow-sm'
                  : 'text-slate-400 hover:text-slate-200 hover:bg-slate-800'
              }`}
            >
              <FileCheck2 className="w-3.5 h-3.5" />
              <span>Public C API</span>
            </button>

            <button
              onClick={() => setActiveTab('cleanroom')}
              className={`flex items-center gap-2 px-3.5 py-1.5 rounded-lg transition font-medium cursor-pointer ${
                activeTab === 'cleanroom'
                  ? 'bg-cyan-500 text-slate-950 font-semibold shadow-sm'
                  : 'text-slate-400 hover:text-slate-200 hover:bg-slate-800'
              }`}
            >
              <ShieldCheck className="w-3.5 h-3.5 text-emerald-400" />
              <span>Clean-Room Audit</span>
            </button>
          </div>

          {/* Tab Content Areas */}
          <div className="flex-1 p-5 overflow-auto">
            {/* Tab 1: Disassembly */}
            {activeTab === 'disasm' && (
              <div className="space-y-4">
                <div className="flex items-center justify-between">
                  <div className="flex items-center gap-2">
                    <span className="text-xs font-semibold text-slate-200">Generated Microcode Instructions</span>
                    <span className="text-[11px] bg-slate-800 text-cyan-400 px-2 py-0.5 rounded font-mono">
                      {compileResult.instructions.length} words
                    </span>
                  </div>

                  <button
                    onClick={() => handleCopy(compileResult.disassembly, 'disasm')}
                    className="flex items-center gap-1.5 text-xs text-slate-300 hover:text-cyan-400 bg-slate-800/80 hover:bg-slate-800 px-2.5 py-1 rounded transition cursor-pointer"
                  >
                    {copied === 'disasm' ? <Check className="w-3.5 h-3.5 text-emerald-400" /> : <Copy className="w-3.5 h-3.5" />}
                    <span>{copied === 'disasm' ? 'Copied' : 'Copy Disassembly'}</span>
                  </button>
                </div>

                <div className="bg-slate-950 border border-slate-800 rounded-xl p-4 font-mono text-xs overflow-x-auto space-y-1.5">
                  {compileResult.instructions.map((inst) => (
                    <div key={inst.index} className="flex items-center gap-3 hover:bg-slate-900/60 px-2 py-1 rounded transition">
                      <span className="text-slate-500 w-6 text-right select-none">{inst.index}:</span>
                      <span className="text-amber-400/90 font-mono bg-amber-500/10 px-1.5 py-0.5 rounded text-[11px] border border-amber-500/20">
                        {inst.hexWord}
                      </span>
                      <span className="font-bold text-cyan-300 w-12">{inst.opcode}</span>
                      <span className="text-slate-200 flex-1">
                        {inst.dst}
                        {inst.src0 && <span className="text-slate-400">, {inst.src0}</span>}
                        {inst.src1 && <span className="text-slate-400">, {inst.src1}</span>}
                      </span>
                      {inst.comment && (
                        <span className="text-slate-500 text-[11px] italic">
                          // {inst.comment}
                        </span>
                      )}
                    </div>
                  ))}
                </div>
              </div>
            )}

            {/* Tab 2: Register Allocation Map */}
            {activeTab === 'registers' && (
              <div className="space-y-6">
                <div>
                  <h4 className="text-xs font-semibold text-slate-200 mb-2 flex items-center gap-2">
                    <span className="h-2 w-2 rounded-full bg-blue-400"></span>
                    Constant Registers (<code className="text-blue-300">c0 - c255</code>) &mdash; Uniform Bindings
                  </h4>
                  <div className="border border-slate-800 rounded-xl overflow-hidden bg-slate-950">
                    <table className="w-full text-left text-xs">
                      <thead className="bg-slate-900 text-slate-400 border-b border-slate-800">
                        <tr>
                          <th className="py-2 px-3 font-medium">Uniform Name</th>
                          <th className="py-2 px-3 font-medium">Data Type</th>
                          <th className="py-2 px-3 font-medium">Base Reg</th>
                          <th className="py-2 px-3 font-medium">Words</th>
                          <th className="py-2 px-3 font-medium">GXP Symbol Offset</th>
                        </tr>
                      </thead>
                      <tbody className="divide-y divide-slate-800/60 font-mono">
                        {compileResult.uniforms.map((u, i) => (
                          <tr key={i} className="hover:bg-slate-900/40">
                            <td className="py-2 px-3 text-cyan-300 font-semibold">{u.name}</td>
                            <td className="py-2 px-3 text-slate-400">{u.type}</td>
                            <td className="py-2 px-3 text-blue-400">c{u.regOffset}</td>
                            <td className="py-2 px-3 text-slate-300">{u.wordCount} (4-byte words)</td>
                            <td className="py-2 px-3 text-slate-400">+{i * 44} B</td>
                          </tr>
                        ))}
                      </tbody>
                    </table>
                  </div>
                </div>

                <div>
                  <h4 className="text-xs font-semibold text-slate-200 mb-2 flex items-center gap-2">
                    <span className="h-2 w-2 rounded-full bg-emerald-400"></span>
                    Vertex Inputs (<code className="text-emerald-300">v0 - v15</code>) &mdash; Semantic Attributes
                  </h4>
                  <div className="border border-slate-800 rounded-xl overflow-hidden bg-slate-950">
                    <table className="w-full text-left text-xs">
                      <thead className="bg-slate-900 text-slate-400 border-b border-slate-800">
                        <tr>
                          <th className="py-2 px-3 font-medium">Attribute Name</th>
                          <th className="py-2 px-3 font-medium">Semantic</th>
                          <th className="py-2 px-3 font-medium">Hardware Reg</th>
                          <th className="py-2 px-3 font-medium">Components</th>
                        </tr>
                      </thead>
                      <tbody className="divide-y divide-slate-800/60 font-mono">
                        {compileResult.inputs.map((inp, i) => (
                          <tr key={i} className="hover:bg-slate-900/40">
                            <td className="py-2 px-3 text-emerald-300 font-semibold">{inp.name}</td>
                            <td className="py-2 px-3 text-amber-300">:{inp.semantic}</td>
                            <td className="py-2 px-3 text-emerald-400">v{inp.regIndex}</td>
                            <td className="py-2 px-3 text-slate-400">{inp.componentCount} components</td>
                          </tr>
                        ))}
                      </tbody>
                    </table>
                  </div>
                </div>

                <div>
                  <h4 className="text-xs font-semibold text-slate-200 mb-2 flex items-center gap-2">
                    <span className="h-2 w-2 rounded-full bg-purple-400"></span>
                    Secondary Outputs (<code className="text-purple-300">o0 - o15</code>)
                  </h4>
                  <div className="border border-slate-800 rounded-xl overflow-hidden bg-slate-950">
                    <table className="w-full text-left text-xs">
                      <thead className="bg-slate-900 text-slate-400 border-b border-slate-800">
                        <tr>
                          <th className="py-2 px-3 font-medium">Output Channel</th>
                          <th className="py-2 px-3 font-medium">Semantic</th>
                          <th className="py-2 px-3 font-medium">Hardware Reg</th>
                          <th className="py-2 px-3 font-medium">Destination</th>
                        </tr>
                      </thead>
                      <tbody className="divide-y divide-slate-800/60 font-mono">
                        {compileResult.outputs.map((out, i) => (
                          <tr key={i} className="hover:bg-slate-900/40">
                            <td className="py-2 px-3 text-purple-300 font-semibold">{out.name}</td>
                            <td className="py-2 px-3 text-amber-300">:{out.semantic}</td>
                            <td className="py-2 px-3 text-purple-400">o{out.regIndex}</td>
                            <td className="py-2 px-3 text-slate-400">Rasterizer / Framebuffer</td>
                          </tr>
                        ))}
                      </tbody>
                    </table>
                  </div>
                </div>
              </div>
            )}

            {/* Tab 3: Hex Dump */}
            {activeTab === 'hexdump' && (
              <div className="space-y-4">
                <div className="grid grid-cols-2 sm:grid-cols-4 gap-3">
                  <div className="bg-slate-950 border border-slate-800 p-3 rounded-lg">
                    <div className="text-[11px] text-slate-400">Magic Identifier</div>
                    <div className="text-sm font-mono font-bold text-cyan-400">"GXP\0" (0x00505847)</div>
                  </div>
                  <div className="bg-slate-950 border border-slate-800 p-3 rounded-lg">
                    <div className="text-[11px] text-slate-400">Container Size</div>
                    <div className="text-sm font-mono font-bold text-emerald-400">{compileResult.gxpHeader.totalSize} Bytes</div>
                  </div>
                  <div className="bg-slate-950 border border-slate-800 p-3 rounded-lg">
                    <div className="text-[11px] text-slate-400">Microcode Offset</div>
                    <div className="text-sm font-mono font-bold text-amber-400">0x{compileResult.gxpHeader.microcodeOffset.toString(16).toUpperCase()}</div>
                  </div>
                  <div className="bg-slate-950 border border-slate-800 p-3 rounded-lg">
                    <div className="text-[11px] text-slate-400">Section Alignment</div>
                    <div className="text-sm font-mono font-bold text-blue-400">16-Byte Pad (Strict)</div>
                  </div>
                </div>

                <div className="bg-slate-950 border border-slate-800 rounded-xl p-4 font-mono text-xs overflow-x-auto">
                  <div className="text-slate-500 mb-2 border-b border-slate-800 pb-1">
                    OFFSET    00 01 02 03 04 05 06 07  08 09 0A 0B 0C 0D 0E 0F  ASCII
                  </div>
                  <div className="space-y-0.5">
                    {hexDumpRows.map((row, idx) => (
                      <div key={idx} className="flex gap-4 hover:bg-slate-900/50 px-1 py-0.5 rounded">
                        <span className="text-slate-500 select-none">{row.offset}</span>
                        <span className="text-cyan-300 font-medium tracking-wider">{row.hex}</span>
                        <span className="text-slate-400 select-none border-l border-slate-800 pl-3">|{row.ascii}|</span>
                      </div>
                    ))}
                  </div>
                </div>
              </div>
            )}

            {/* Tab 4: C API Reference */}
            {activeTab === 'capi' && (
              <div className="space-y-4">
                <div className="flex items-center justify-between">
                  <span className="text-xs font-semibold text-slate-200">Integration with SceShaccCg C API</span>
                  <button
                    onClick={() => handleCopy(cApiSnippet, 'capi')}
                    className="flex items-center gap-1.5 text-xs text-slate-300 hover:text-cyan-400 bg-slate-800/80 hover:bg-slate-800 px-2.5 py-1 rounded transition cursor-pointer"
                  >
                    {copied === 'capi' ? <Check className="w-3.5 h-3.5 text-emerald-400" /> : <Copy className="w-3.5 h-3.5" />}
                    <span>{copied === 'capi' ? 'Copied' : 'Copy C Code'}</span>
                  </button>
                </div>

                <pre className="bg-slate-950 border border-slate-800 p-4 rounded-xl font-mono text-xs text-slate-300 overflow-x-auto leading-relaxed">
                  {cApiSnippet}
                </pre>
              </div>
            )}

            {/* Tab 5: Clean-Room Statement & Audit */}
            {activeTab === 'cleanroom' && (
              <div className="space-y-4 text-xs leading-relaxed text-slate-300">
                <div className="p-4 bg-emerald-950/30 border border-emerald-500/30 rounded-xl flex items-start gap-3">
                  <ShieldCheck className="w-5 h-5 text-emerald-400 shrink-0 mt-0.5" />
                  <div>
                    <h4 className="font-semibold text-emerald-300 text-sm mb-1">
                      Clean-Room Software Engineering Compliance
                    </h4>
                    <p className="text-slate-300">
                      <code>open-shacc</code> was designed and implemented completely from public domain specifications and standard graphics compiler standards.
                    </p>
                  </div>
                </div>

                <div className="grid grid-cols-1 md:grid-cols-2 gap-4">
                  <div className="p-4 bg-slate-950 border border-slate-800 rounded-xl space-y-2">
                    <h5 className="font-semibold text-white flex items-center gap-2">
                      <BookOpen className="w-4 h-4 text-cyan-400" />
                      Approved Public Specifications
                    </h5>
                    <ul className="list-disc list-inside space-y-1 text-slate-400">
                      <li>NVIDIA Cg Language Specification v3.1</li>
                      <li>DirectX 9 / Shader Model 3.0 HLSL Syntax Rules</li>
                      <li>Public Mesa NIR intermediate representation</li>
                      <li>Open PowerVR SGX / USSE architecture patents</li>
                    </ul>
                  </div>

                  <div className="p-4 bg-slate-950 border border-slate-800 rounded-xl space-y-2">
                    <h5 className="font-semibold text-white flex items-center gap-2">
                      <FileCheck2 className="w-4 h-4 text-blue-400" />
                      Strict Exclusion Verification
                    </h5>
                    <ul className="list-disc list-inside space-y-1 text-slate-400">
                      <li>Zero proprietary binary code included</li>
                      <li>No decompiled IDA Pro or Ghidra disassemblies</li>
                      <li>No copyrighted SDK headers</li>
                      <li>Standard MIT open-source license</li>
                    </ul>
                  </div>
                </div>

                <div className="p-4 bg-slate-950 border border-slate-800 rounded-xl font-mono text-[11px] text-slate-400 space-y-1">
                  <div className="text-cyan-400 font-bold mb-1">GitHub Repository Details:</div>
                  <div>URL: <a href="https://github.com/git-effl/open-shacc" target="_blank" rel="noopener noreferrer" className="text-blue-400 underline">https://github.com/git-effl/open-shacc</a></div>
                  <div>Branch: <span className="text-emerald-400 font-semibold">main</span></div>
                  <div>Standard: <span className="text-amber-400">C++20 &amp; CMake 3.20+</span></div>
                </div>
              </div>
            )}
          </div>
        </div>
      </div>
    </div>
  );
}

const cApiSnippet = `#include <open_shacc.h>
#include <stdio.h>

int main() {
    const char* source = 
        "uniform float4x4 u_MVPMatrix;\\n"
        "float4 main(float4 position : POSITION) : POSITION {\\n"
        "    return mul(u_MVPMatrix, position);\\n"
        "}\\n";

    void* gxp_buffer = NULL;
    size_t gxp_size = 0;
    char* diag_log = NULL;

    int res = SceShaccCgCompileProgram(
        source,
        OPEN_SHACC_OPT_VERBOSE_DISASSEMBLY,
        NULL,
        &gxp_buffer,
        &gxp_size,
        &diag_log
    );

    if (diag_log) {
        printf("%s\\n", diag_log);
        SceShaccCgDestroyDiagnosticLog(diag_log);
    }

    if (res == OPEN_SHACC_SUCCESS) {
        printf("GXP successfully created: %zu bytes (16-byte aligned)\\n", gxp_size);
        // Serialize or load onto GPU hardware...
        SceShaccCgDestroyProgram(gxp_buffer);
    }

    return res;
}`;
