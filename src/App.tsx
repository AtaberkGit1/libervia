/**
 * @license
 * SPDX-License-Identifier: Apache-2.0
 */

import { useState } from 'react';
import { Download, Terminal, Shield, Cpu } from 'lucide-react';

const PROJECT_FILES = [
  'LiberVia-Win64/build.bat',
  'LiberVia-Win64/README.txt',
  'LiberVia-Win64/src/main.c',
  'LiberVia-Win64/src/gen_icon.c',
  'LiberVia-Win64/src/libervia.ico',
  'LiberVia-Win64/src/resource.rc',
  'LiberVia-Win64/src/resource.h',
  'LiberVia-Win64/src/app.manifest',
];

export default function App() {
  const [previewActive, setPreviewActive] = useState(false);

  return (
    <div className="min-h-screen bg-black text-white font-mono selection:bg-[#00E5FF] selection:text-black flex flex-col items-center justify-center p-6">
      <div className="w-full max-w-3xl border border-[#222222] bg-black p-6 md:p-8 space-y-8">
        {/* Header */}
        <div className="flex flex-col sm:flex-row sm:items-center sm:justify-between gap-4 border-b border-[#1A1A1A] pb-6">
          <div className="flex items-center gap-4">
            <img
              src="/assets/aistudio/image_a9e807ff.png"
              alt="LiberVia App Icon"
              className="w-10 h-10 object-cover shrink-0 border border-[#222222]"
            />
            <div>
              <h1 className="text-xl font-bold tracking-wider text-white">LIBERVIA // WIN64</h1>
              <p className="text-xs text-[#666666] mt-0.5">
                PURE C11 + WIN32 API + WINDIVERT 2.2 KERNEL DPI BYPASS
              </p>
            </div>
          </div>

          <a
            href="/LiberVia-Win64.zip"
            download="LiberVia-Win64.zip"
            className="inline-flex items-center justify-center gap-2 border-2 border-[#00E5FF] bg-black px-5 py-2.5 text-xs font-bold tracking-wider text-[#00E5FF] hover:bg-[#001418] transition-none"
          >
            <Download className="w-4 h-4" />
            DOWNLOAD LIBERVIA-WIN64.ZIP
          </a>
        </div>

        {/* Exact 360x250 Win32 Window Preview + Kernel Specs */}
        <div className="grid grid-cols-1 md:grid-cols-2 gap-8 items-start">
          {/* Interactive 360x250 Window Mock */}
          <div>
            <div className="text-[11px] text-[#555555] mb-2 uppercase tracking-wider">
              Win32 Window Preview (360 x 250 px)
            </div>
            <div className="w-full max-w-[360px] border border-[#333333] bg-black">
              {/* Native Dark Title Bar */}
              <div className="h-7 bg-[#0A0A0A] border-b border-[#1A1A1A] px-3 flex items-center justify-between text-[11px] text-[#888888]">
                <span>LiberVia</span>
                <span className="tracking-widest text-[#555555]">_  ×</span>
              </div>
              {/* 360x250 Client Area */}
              <div className="h-[250px] p-6 flex flex-col justify-between bg-black">
                <div className="flex items-center justify-between border-b border-[#1A1A1A] pb-2">
                  <span className="text-lg font-bold tracking-wide text-white">LIBERVIA</span>
                  <span
                    className={`text-sm font-bold tracking-wider ${
                      previewActive ? 'text-[#00E5FF]' : 'text-[#555555]'
                    }`}
                  >
                    {previewActive ? 'ACTIVE' : 'IDLE'}
                  </span>
                </div>

                <button
                  type="button"
                  onClick={() => setPreviewActive((v) => !v)}
                  className={`w-full h-[104px] border-2 text-xl font-bold tracking-widest cursor-pointer ${
                    previewActive
                      ? 'border-[#00E5FF] text-[#00E5FF] bg-black hover:bg-[#001418]'
                      : 'border-white text-white bg-black hover:bg-[#0F0F0F]'
                  }`}
                >
                  {previewActive ? 'DISENGAGE' : 'ENGAGE'}
                </button>

                <div className="flex items-center justify-between text-[10px]">
                  <span className="text-[#444444]">SINGLE-CLICK DPI BYPASS // WIN64</span>
                  <span className="text-[#666666] hover:text-white cursor-default">
                    [MINIMIZE TO TRAY]
                  </span>
                </div>
              </div>
            </div>
          </div>

          {/* Architecture Summary & File Manifest */}
          <div className="space-y-5 text-xs">
            <div className="space-y-2.5">
              <div className="flex items-start gap-2.5 text-[#CCCCCC]">
                <Shield className="w-4 h-4 text-[#00E5FF] shrink-0 mt-0.5" />
                <div>
                  <div className="font-bold text-white">KERNEL PACKET ENGINE</div>
                  <div className="text-[#666666] mt-0.5 leading-relaxed">
                    TLS ClientHello SNI split + disorder + TTL=3 bad-checksum decoy, HTTP byte-2 split + hOsT: casing, QUIC/IPv6 drop, and 1.1.1.1 DNS NAT.
                  </div>
                </div>
              </div>

              <div className="flex items-start gap-2.5 text-[#CCCCCC]">
                <Cpu className="w-4 h-4 text-[#00E5FF] shrink-0 mt-0.5" />
                <div>
                  <div className="font-bold text-white">ZERO-BLOAT FOOTPRINT</div>
                  <div className="text-[#666666] mt-0.5 leading-relaxed">
                    Pure C11 + Win32 GDI/DWM + Shell_NotifyIconW tray. No .NET, no Electron, no runtime bloat.
                  </div>
                </div>
              </div>

              <div className="flex items-start gap-2.5 text-[#CCCCCC]">
                <Terminal className="w-4 h-4 text-[#00E5FF] shrink-0 mt-0.5" />
                <div>
                  <div className="font-bold text-white">SELF-BOOTSTRAPPING BUILD</div>
                  <div className="text-[#666666] mt-0.5 leading-relaxed">
                    Extract <span className="text-white">LiberVia-Win64.zip</span> on Windows 10/11 and run{' '}
                    <span className="text-white">build.bat</span> to auto-fetch WinDivert 2.2.2-A and build{' '}
                    <span className="text-white">dist\LiberVia.exe</span>.
                  </div>
                </div>
              </div>
            </div>

            <div className="border border-[#1A1A1A] p-3 bg-[#050505]">
              <div className="text-[10px] text-[#555555] mb-2 uppercase tracking-wider">
                Packaged Source Tree
              </div>
              <ul className="space-y-1 text-[11px] text-[#888888]">
                {PROJECT_FILES.map((file) => (
                  <li key={file}>{file}</li>
                ))}
              </ul>
            </div>
          </div>
        </div>
      </div>
    </div>
  );
}
