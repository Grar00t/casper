---
name: casper-dev
description: >
  Development workflow for Casper Engine (NIYAH search + Phi-3.5 + Foundry).
  USE FOR: editing niyah_engine_local, casper_workbench.html, C11 engine,
  deploying to Azure VM (20.91.208.59), connecting Azure AI Foundry models.
  WHEN: "fix the UI", "deploy to VM", "connect Foundry", "build Casper.exe",
  "add feature to NIYAH", "update server.js".
---

# Casper Dev Skill

## Project Map

| Path | Purpose |
|------|---------|
| `niyah_engine_local/` | Node.js server — NIYAH search + Phi endpoint |
| `niyah_engine_local/lib/niyahEngine.js` | DDG search → TF-IDF → cite |
| `niyah_engine_local/lib/phiEngine.js` | Phi-3.5 via llama-server HTTP |
| `niyah_engine_local/server.js` | Express server — all endpoints |
| `niyah_engine_local/routes/niyah.js` | /api/v1/niyah/ask |
| `UI_CSharp/casper_workbench.html` | Frontend — 2 themes, AR/EN |
| `UI_CSharp/MainWindow.xaml.cs` | WPF host — starts Node.js, PTY |
| `UI_CSharp/CasperBridge.cs` | C# ↔ JS bridge |
| `UI_CSharp/PtyBridge.cs` | Real terminal via ConPTY |
| `Core_CPP/niyah_hybrid_main.c` | C11 engine — smoke 101 tests |
| `Core_CPP/niyah_hybrid.exe` | Built binary |
| `Data_Training/safety.nrule` | Symbolic rules |

## Azure Resources

| Resource | Value |
|----------|-------|
| VM IP | 20.91.208.59 |
| VM name | TestVM |
| Resource Group | GRATECH_NEW_RG |
| Foundry endpoint | https://s-4187-resource.cognitiveservices.azure.com/ |
| Foundry RG | rg-s-4187 |
| Available models | gpt-5-mini, claude-sonnet-4-6, claude-opus-4-6, grok-4-20-reasoning |

## API Endpoints (live on VM port 80)

| Endpoint | Description |
|----------|-------------|
| GET /health | Server health |
| POST /api/v1/niyah/ask | NIYAH search (DDG + TF-IDF) |
| GET /api/v1/niyah/health | Engine health |
| POST /api/v1/phi/chat | Phi-3.5 mini (llama-server port 8080) |
| GET /api/v1/phi/health | Phi availability |
| POST /api/v1/ask | Hybrid: NIYAH + Phi/Foundry |

## Deploy to VM

```powershell
# Pull from GitHub and restart Node server
az vm run-command invoke --resource-group GRATECH_NEW_RG --name TestVM \
  --command-id RunShellScript \
  --scripts "cd /tmp && rm -rf casper && git clone --depth=1 https://github.com/Grar00t/casper.git && cp /tmp/casper/niyah_engine_local/lib/*.js /opt/casper-agent/lib/ && cp /tmp/casper/niyah_engine_local/routes/*.js /opt/casper-agent/routes/ && cp /tmp/casper/niyah_engine_local/server.js /opt/casper-agent/ && pkill -f 'node /opt/casper-agent' 2>/dev/null; sleep 1 && PORT=80 nohup node /opt/casper-agent/server.js > /opt/casper-agent/logs/niyah.log 2>&1 &"
```

## Build Casper.exe

```powershell
# WPF app
dotnet build UI_CSharp/CasperUI.csproj -c Release

# C11 engine
powershell -ExecutionPolicy Bypass -File scripts/build_msvc.ps1
```

## Non-Negotiable Rules (from AGENTS.md)

1. No external dependencies beyond libc + libm in C11 code
2. All C11 changes must pass `--smoke` (101 tests)
3. No fake inference in C11 — avx2_dot_product is math only
4. `.env` contains Foundry key — NEVER commit it
5. `niyah_engine` folder = duplicate of `niyah_engine_local` — don't use it

## Connect Foundry Model

```javascript
// In niyah_engine_local/lib/foundryEngine.js
const ENDPOINT = process.env.FOUNDRY_ENDPOINT;
const KEY = process.env.FOUNDRY_KEY;
const MODEL = process.env.FOUNDRY_MODEL || 'claude-sonnet-4-6';

async function foundryChat(message, history = []) {
  const r = await fetch(`${ENDPOINT}openai/deployments/${MODEL}/chat/completions?api-version=2024-12-01-preview`, {
    method: 'POST',
    headers: { 'api-key': KEY, 'Content-Type': 'application/json' },
    body: JSON.stringify({
      messages: [
        { role: 'system', content: 'You are NIYAH. Answer in the user language. No fake emotions. Tool, not person.' },
        ...history.slice(-6),
        { role: 'user', content: message }
      ],
      max_tokens: 500,
      temperature: 0.7
    }),
    signal: AbortSignal.timeout(30000)
  });
  if (!r.ok) throw new Error(`Foundry HTTP ${r.status}`);
  const d = await r.json();
  return d.choices[0].message.content;
}
```
