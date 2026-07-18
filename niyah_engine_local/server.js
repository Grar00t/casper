'use strict';
/**
 * Casper Agent Server — Azure VM (port 3000, nginx proxy port 80)
 * ---------------------------------------------------------------
 * Endpoints:
 *   GET  /health              — health check
 *   GET  /fetch?url=          — fetch raw page text (legacy)
 *   GET  /summarize?query=&text=&n= — extractive summary (legacy)
 *   POST /api/v1/niyah/ask    — full NIYAH engine: search + extract + cite
 *   GET  /api/v1/niyah/health — engine health
 *   GET  /api/v1/niyah/stats  — memory stats
 *   GET  /api/v1/niyah/context
 *   DELETE /api/v1/niyah/memory
 */

const express = require('express');
const niyahRouter = require('./routes/niyah');
const { phiChat, phiHealthCheck, phiImproveAnswer } = require('./lib/phiEngine');

const app = express();

app.use((req, res, next) => {
  res.setHeader('Access-Control-Allow-Origin', '*');
  res.setHeader('Access-Control-Allow-Methods', 'GET,POST,DELETE,OPTIONS');
  res.setHeader('Access-Control-Allow-Headers', 'Content-Type');
  if (req.method === 'OPTIONS') return res.sendStatus(200);
  next();
});

app.use((req, res, next) => {
  express.json()(req, res, (err) => {
    if (err) return res.status(400).json({ error: 'Invalid JSON', details: err.message });
    next();
  });
});

// ── Legacy endpoints (used by casper_workbench.html) ─────────────────────────

app.get('/health', (req, res) => {
  res.json({ ok: true, service: 'casper-agent', engine: 'niyah-v3', time: new Date().toISOString() });
});

app.get('/fetch', async (req, res) => {
  const targetUrl = req.query.url;
  if (!targetUrl) return res.status(400).json({ ok: false, error: 'url param required' });
  try {
    const controller = new AbortController();
    const t = setTimeout(() => controller.abort(), 10000);
    const resp = await fetch(targetUrl, {
      signal: controller.signal,
      headers: {
        'User-Agent': 'Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 Chrome/120.0.0.0 Safari/537.36',
        'Accept': 'text/html,application/xhtml+xml,*/*;q=0.8',
        'Accept-Language': 'en-US,en;q=0.9,ar;q=0.8',
      },
    });
    clearTimeout(t);
    const html = await resp.text();
    const text = stripHtml(html);
    res.json({
      ok: true,
      final_url: targetUrl,
      status: resp.status,
      html_bytes: html.length,
      text_chars: text.length,
      sample: text.substring(0, 1000),
    });
  } catch (err) {
    res.status(500).json({ ok: false, error: err.message });
  }
});

app.get('/summarize', (req, res) => {
  const { text, query, n } = req.query;
  if (!text || !query) return res.status(400).json({ ok: false, error: 'text and query required' });
  const { extractTopSentences } = require('./lib/relevance');
  const count = Math.min(parseInt(n) || 3, 8);
  const sentences = extractTopSentences(query, text, count);
  const summary = sentences.map(s => s.text).join(' ') || text.substring(0, 500);
  res.json({ ok: true, summary, chars: summary.length });
});

// ── NIYAH Engine (search + extract) ──────────────────────────────────────────
app.use('/api/v1/niyah', niyahRouter);

// ── Phi-3.5 mini (local LLM via llama-server) ─────────────────────────────────
app.get('/api/v1/phi/health', async (req, res) => {
  const alive = await phiHealthCheck();
  res.json({ available: alive, host: process.env.PHI_HOST || 'http://127.0.0.1:8080' });
});

app.post('/api/v1/phi/chat', async (req, res) => {
  const { message, history, systemPrompt, maxTokens, temperature } = req.body || {};
  if (!message) return res.status(400).json({ error: 'message required' });
  const result = await phiChat(message, history || [], { systemPrompt, maxTokens, temperature });
  if (!result) return res.status(503).json({ error: 'Phi server not available. Start llama-server first.' });
  res.json(result);
});

// ── Hybrid: NIYAH search + Phi synthesis ──────────────────────────────────────
app.post('/api/v1/ask', async (req, res) => {
  const { query, mode } = req.body || {};
  if (!query) return res.status(400).json({ error: 'query required' });

  // Always search with NIYAH first
  let niyahResult = null;
  try {
    const nr = await fetch(`http://localhost:${process.env.PORT || 3000}/api/v1/niyah/ask`, {
      method: 'POST', headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ query }), signal: AbortSignal.timeout(40000),
    });
    if (nr.ok) niyahResult = await nr.json();
  } catch (_) {}

  const phiAlive = await phiHealthCheck();

  // If Phi available and mode=hybrid, improve the answer
  if (phiAlive && niyahResult?.answer && mode !== 'search-only') {
    const improved = await phiImproveAnswer(query, niyahResult.answer);
    if (improved) {
      return res.json({
        answer: improved,
        citations: niyahResult.citations || [],
        confidence: niyahResult.confidence,
        tookMs: niyahResult.tookMs,
        mode: 'hybrid',
        sources: { niyah: true, phi: true },
      });
    }
  }

  // Fallback: Phi direct if NIYAH failed
  if (phiAlive && (!niyahResult || !niyahResult.answer)) {
    const phiResult = await phiChat(query);
    if (phiResult) {
      return res.json({
        answer: phiResult.answer,
        citations: [],
        confidence: 0.6,
        tookMs: phiResult.tookMs,
        mode: 'phi-only',
        sources: { niyah: false, phi: true },
      });
    }
  }

  // Return NIYAH result as-is
  if (niyahResult) return res.json({ ...niyahResult, mode: 'search-only', sources: { niyah: true, phi: false } });
  res.status(503).json({ error: 'Both NIYAH and Phi unavailable' });
});

// ── Root ──────────────────────────────────────────────────────────────────────
app.get('/', (req, res) => res.json({
  service: 'Casper Agent + NIYAH Engine',
  version: '3.0',
  endpoints: {
    health: 'GET /health',
    fetch: 'GET /fetch?url=TARGET',
    summarize: 'GET /summarize?text=TEXT&query=Q&n=3',
    ask: 'POST /api/v1/niyah/ask  { "query": "..." }',
    niyahHealth: 'GET /api/v1/niyah/health',
    stats: 'GET /api/v1/niyah/stats',
  },
}));

// ── Start ─────────────────────────────────────────────────────────────────────
const PORT = process.env.PORT || 3000;
const server = app.listen(PORT, () => {
  console.log(`✅ Casper Agent running on port ${PORT}`);
  console.log(`📊 Health: http://localhost:${PORT}/health`);
  console.log(`🧠 NIYAH:  POST http://localhost:${PORT}/api/v1/niyah/ask`);
});

server.on('error', (err) => {
  if (err.code === 'EADDRINUSE') console.error(`❌ Port ${PORT} in use`);
  else console.error('❌ Server error:', err);
  process.exit(1);
});

['SIGINT', 'SIGTERM'].forEach(sig => process.on(sig, () => {
  server.close(() => process.exit(0));
}));

process.on('uncaughtException', (err) => console.error('❌ Uncaught:', err.message));
process.on('unhandledRejection', (r) => console.error('❌ Unhandled:', r));

// ── Helpers ───────────────────────────────────────────────────────────────────
function stripHtml(html) {
  return html
    .replace(/<script[\s\S]*?<\/script>/gi, ' ')
    .replace(/<style[\s\S]*?<\/style>/gi, ' ')
    .replace(/<!--[\s\S]*?-->/g, ' ')
    .replace(/<[^>]+>/g, ' ')
    .replace(/&nbsp;/g, ' ').replace(/&amp;/g, '&').replace(/&quot;/g, '"')
    .replace(/\s+/g, ' ').trim();
}
