# CLAUDE.md — Casper Engine Design System & Codebase Rules

## Project Overview

**Casper Engine** is a desktop search+AI application. The UI is a **single HTML file** rendered inside a WPF WebView2 shell. There is no React, no Vue, no build system for the frontend. All styling is inline CSS with CSS custom properties.

---

## 1. Token Definitions

### Location
All design tokens are CSS custom properties defined in **one file**:
- `UI_CSharp/casper_workbench.html` — inside a `<style>` block at the top

### Dark Theme (default)
```css
:root {
  --bg: #0F1117;
  --surface: #1A1D27;
  --surface2: #232635;
  --border: rgba(255,255,255,.08);
  --border-strong: rgba(255,255,255,.15);
  --neon: #7C3AED;       /* primary — purple */
  --neon2: #A78BFA;      /* primary light */
  --accent: #06B6D4;     /* secondary — cyan */
  --accent2: #67E8F9;    /* secondary light */
  --green: #10B981;      /* success */
  --amber: #F59E0B;      /* warning */
  --red: #EF4444;        /* error */
  --text: #F8FAFC;       /* primary text */
  --text2: #94A3B8;      /* secondary text */
  --text3: #475569;      /* muted text */
  --mono: 'JetBrains Mono', monospace;
  --sans: 'Inter', 'Tajawal', sans-serif;
  --radius: 12px;
  --radius-sm: 8px;
  --shadow: 0 4px 24px rgba(0,0,0,.4);
}
```

### Light Theme
```css
[data-theme="light"] {
  --bg: #F1F5F9;
  --surface: #FFFFFF;
  --surface2: #F8FAFC;
  --border: rgba(0,0,0,.08);
  --border-strong: rgba(0,0,0,.18);
  --neon: #7C3AED;
  --neon2: #6D28D9;
  --accent: #0891B2;
  --accent2: #0E7490;
  --green: #059669;
  --amber: #D97706;
  --red: #DC2626;
  --text: #0F172A;
  --text2: #475569;
  --text3: #94A3B8;
  --shadow: 0 4px 24px rgba(0,0,0,.10);
}
```

### Token Format
- **No transformation system** — raw CSS custom properties only
- Theme switching: `document.documentElement.setAttribute('data-theme', 'light')` or remove attribute for dark
- Stored in `localStorage.getItem('theme')` → `'dark'` or `'light'`

---

## 2. Component Library

### Location
- **No component library** — all UI is in `UI_CSharp/casper_workbench.html`
- Components are CSS classes, not JS components

### Key CSS Component Classes

| Class | Purpose |
|-------|---------|
| `.msg-user` | User message bubble (purple, right-aligned) |
| `.msg-niyah` | AI response container |
| `.answer-card` | Bordered card wrapping answers |
| `.conf-row` / `.conf-bar` / `.conf-fill` | Confidence bar (green/amber/red) |
| `.cit` | Citation chip (clickable link) |
| `.trace-block` / `.trace-toggle` / `.trace-body` | Collapsible pipeline trace |
| `.pill` | Welcome screen suggestion pill |
| `.engine-card` | Sidebar NIYAH info card |
| `.stat-card` / `.stat-val` / `.stat-label` | Session stat cards |
| `.kpi` / `.kpi-val` | Ledger KPI numbers |
| `.lentry` | Ledger list entry |
| `.term-panel` | Terminal overlay panel |
| `.composer` | Bottom input area |

### Component Architecture
- **No framework** — vanilla JS + DOM manipulation
- Messages are appended via `appendDiv(className, innerHTML)`
- No virtual DOM, no reactivity system
- RTL/LTR switching via `document.documentElement.dir`

---

## 3. Frameworks & Libraries

| Layer | Technology |
|-------|-----------|
| Desktop shell | WPF (.NET 9) + WebView2 |
| Frontend | **Vanilla HTML/CSS/JS** — single file |
| Fonts | Google Fonts CDN (Inter, Tajawal, JetBrains Mono) |
| Terminal | xterm.js 5.3.0 (loaded on demand from CDN) |
| Backend | Node.js + Express |
| C11 engine | Pure C11, MSVC/GCC |
| Build (C#) | `dotnet build` |
| Build (C11) | `scripts/build_msvc.ps1` or `scripts/build_gcc.sh` |
| No bundler | No Vite, no Webpack, no Rollup |

---

## 4. Asset Management

### Current State
- **No images** — all icons are Unicode/emoji characters
- **No SVG files** — inline only
- **No CDN** except Google Fonts and xterm.js from jsdelivr
- favicon: not set (WPF app, not browser)

### Font Loading
```html
<link href="https://fonts.googleapis.com/css2?family=Inter:wght@300;400;500;600;700&family=Tajawal:wght@300;400;500;700&family=JetBrains+Mono:wght@400;500&display=swap" rel="stylesheet">
```

---

## 5. Icon System

- **No icon library** (no Lucide, no Heroicons, no FontAwesome)
- Icons are Unicode characters in HTML:
  - `🔍` search
  - `⬛` terminal
  - `📎` file attach
  - `↺` refresh/clear
  - `✕` close
  - `▶` / `▸` chevron
  - `⚡` cached/fast
  - `✓` verified
- Engine card uses `◈` for NIYAH brand

### When Adding Icons
Use Unicode or inline SVG. Do NOT add icon libraries — the app runs offline inside WebView2.

---

## 6. Styling Approach

### Methodology
- **Single-file CSS** — all styles in one `<style>` block
- **CSS custom properties** for theming (see tokens above)
- **No CSS Modules, no Tailwind, no Styled Components**
- Class names are flat, BEM-ish but not strict: `.trace-block`, `.trace-toggle`, `.trace-body`

### Global Styles
```css
*{margin:0;padding:0;box-sizing:border-box}
html,body{height:100%;overflow:hidden;font-family:var(--sans);background:var(--bg);color:var(--text)}
```

### Responsive Design
- Grid layout: `grid-template-columns: 220px 1fr 280px`
- No media queries currently (WPF window, not browser)
- RTL/LTR: `document.documentElement.dir = 'rtl'` or `'ltr'`
- Arabic font: `Tajawal` in the `--sans` stack

### Bilingual Support
- UI labels stored in JS object `L.ar` / `L.en`
- Language toggle: `toggleLang()` → `localStorage.setItem('ui-lang', 'ar'|'en')`
- `applyLang()` updates all label elements by ID

---

## 7. Project Structure

```
Casper_Engine/
├── AGENTS.md                    # Agent behavior rules (non-negotiable)
├── CLAUDE.md                    # This file
├── README.md                    # Architecture overview
├── Core_CPP/                    # C11 engine
│   ├── niyah_core.c             # Transformer decoder
│   ├── hybrid_reasoner.c        # Prolog-like symbolic reasoner
│   ├── constraint_solver.c      # Exact rational constraint solver
│   ├── rule_parser.c            # .nrule format parser
│   ├── proof_generator.c        # SHA-256 proof generation
│   ├── khz_q_svd.c              # Math-coherence gate
│   ├── casper_rag.c              # WinHTTP web search (C11)
│   ├── niyah_hybrid_main.c      # CLI entry point (--smoke, --rag, --audit-stdin)
│   └── niyah_hybrid.exe         # Built binary (278KB)
├── UI_CSharp/                   # WPF desktop app
│   ├── casper_workbench.html    # ★ THE ENTIRE FRONTEND UI
│   ├── MainWindow.xaml          # WPF window (borderless, WebView2)
│   ├── MainWindow.xaml.cs       # WebView2 init, Node.js auto-start
│   ├── CasperBridge.cs          # JS ↔ C# bridge (Query, Terminal, Window)
│   ├── PtyBridge.cs             # ConPTY real terminal
│   └── CasperUI.csproj          # .NET 9 WPF project
├── niyah_engine_local/          # Node.js search+AI server
│   ├── server.js                # Express server (all endpoints)
│   ├── package.json             # Dependencies: express only
│   ├── .env                     # Foundry API key (NEVER commit)
│   ├── lib/
│   │   ├── niyahEngine.js       # DDG search → TF-IDF → cite
│   │   ├── searchProvider.js    # DDG HTML scraper
│   │   ├── relevance.js         # TF-IDF, cosine sim, dedup
│   │   ├── reasoner.js          # Extractive synthesis
│   │   ├── memory.js            # SQLite/JSON cache
│   │   ├── phiEngine.js         # Phi-3.5 mini via llama-server
│   │   └── foundryEngine.js     # Azure AI Foundry wrapper
│   └── routes/
│       └── niyah.js             # /api/v1/niyah/* routes + C11 audit
├── Data_Training/
│   └── safety.nrule             # Symbolic verification rules
├── scripts/
│   ├── build_msvc.ps1           # Windows C11 build
│   ├── build_gcc.sh             # Linux C11 build
│   ├── niyah.ps1                # Unified build/smoke/bench
│   └── deploy_vm.ps1            # Azure VM deployment
├── dist/                        # Release distribution
│   ├── Casper.exe               # Self-contained WPF app (56MB)
│   └── app/
│       ├── niyah_hybrid.exe     # C11 engine
│       └── casper_workbench.html # Frontend copy
└── haven_ide_source/            # Downloaded HAVEN IDE assets (reference)
```

---

## Figma-to-Code Integration Notes

### When translating Figma designs to this project:

1. **Output format**: Plain HTML + CSS custom properties. No JSX, no components.
2. **All CSS goes in** `UI_CSharp/casper_workbench.html` inside the existing `<style>` block
3. **All JS goes in** the same file inside the `<script>` block at the bottom
4. **Theme support**: Use `var(--token-name)` for all colors/spacing. Both themes must work.
5. **RTL support**: Test both `dir="rtl"` and `dir="ltr"`. Use `padding-inline-start` not `padding-left`.
6. **Fonts**: Inter (Latin), Tajawal (Arabic), JetBrains Mono (code). Already loaded.
7. **No asset imports**: Use Unicode for icons, inline SVG if needed.
8. **No build step**: Changes to the HTML file are immediately visible after Casper.exe restart.
9. **Copy to dist**: After editing, copy `UI_CSharp/casper_workbench.html` → `dist/app/casper_workbench.html`

### Color Palette Summary (for Figma)

| Token | Dark | Light | Usage |
|-------|------|-------|-------|
| `--neon` | `#7C3AED` | `#7C3AED` | Primary buttons, active states |
| `--accent` | `#06B6D4` | `#0891B2` | Secondary, stat values |
| `--green` | `#10B981` | `#059669` | Success, verified |
| `--amber` | `#F59E0B` | `#D97706` | Warning, cached |
| `--red` | `#EF4444` | `#DC2626` | Error, danger |
| `--bg` | `#0F1117` | `#F1F5F9` | Page background |
| `--surface` | `#1A1D27` | `#FFFFFF` | Cards, panels |
| `--text` | `#F8FAFC` | `#0F172A` | Primary text |
| `--text2` | `#94A3B8` | `#475569` | Secondary text |
| `--text3` | `#475569` | `#94A3B8` | Muted text |

### Azure Resources (for deployment context)
- VM: `20.91.208.59` (TestVM, GRATECH_NEW_RG)
- Foundry: `s-4187-resource.cognitiveservices.azure.com`
- Models: gpt-5-mini, claude-sonnet-4-6, claude-opus-4-6, grok-4-20-reasoning
