'use strict';

const express = require('express');
const niyahRouter = require('./routes/niyah');
const { phiChat, phiHealthCheck, phiModelInfo, phiImproveAnswer } = require('./lib/phiEngine');
const { NiyahEngine } = require('./lib/niyahEngine');

const app = express();
const PORT = Number(process.env.PORT) || 3000;
const HOST = process.env.HOST || '127.0.0.1';
const allowedOrigins = new Set(
  (process.env.CASPER_ALLOWED_ORIGINS || 'null,http://127.0.0.1:3000,http://localhost:3000')
    .split(',')
    .map((value) => value.trim())
    .filter(Boolean),
);

app.disable('x-powered-by');

app.use((req, res, next) => {
  const origin = req.headers.origin;
  if (origin && allowedOrigins.has(origin)) {
    res.setHeader('Access-Control-Allow-Origin', origin);
    res.setHeader('Vary', 'Origin');
    res.setHeader('Access-Control-Allow-Methods', 'GET,POST,DELETE,OPTIONS');
    res.setHeader('Access-Control-Allow-Headers', 'Content-Type');
  }
  if (req.method === 'OPTIONS') {
    return origin && allowedOrigins.has(origin) ? res.sendStatus(204) : res.sendStatus(403);
  }
  return next();
});

app.use(express.json({ limit: '64kb' }));

app.get('/health', async (req, res) => {
  const llmAlive = await phiHealthCheck();
  const llmInfo = llmAlive ? await phiModelInfo() : null;
  res.json({
    ok: true,
    service: 'casper-agent',
    llm: { available: llmAlive, model: llmInfo?.id || null },
    time: new Date().toISOString(),
  });
});

app.get('/summarize', (req, res) => {
  const { text, query, n } = req.query;
  if (!text || !query) return res.status(400).json({ ok: false, error: 'text and query required' });
  const { extractTopSentences } = require('./lib/relevance');
  const count = Math.max(1, Math.min(Number.parseInt(n, 10) || 3, 8));
  const sentences = extractTopSentences(query, text, count);
  const summary = sentences.map((sentence) => sentence.text).join(' ') || text.substring(0, 500);
  return res.json({ ok: true, summary, chars: summary.length });
});

app.use('/api/v1/niyah', niyahRouter);

app.get('/api/v1/phi/health', async (req, res) => {
  const alive = await phiHealthCheck();
  res.json({ available: alive, host: process.env.PHI_HOST || 'http://127.0.0.1:8080' });
});

app.post('/api/v1/phi/chat', async (req, res) => {
  const { message, history, systemPrompt, maxTokens, temperature } = req.body || {};
  if (!message || typeof message !== 'string') return res.status(400).json({ error: 'message required' });
  const result = await phiChat(message, Array.isArray(history) ? history : [], {
    systemPrompt,
    maxTokens,
    temperature,
  });
  if (!result) return res.status(503).json({ error: 'local Phi server not available' });
  return res.json(result);
});

const hybridEngine = new NiyahEngine({
  searxngBaseUrl: process.env.SEARXNG_BASE_URL,
  braveApiKey: process.env.BRAVE_API_KEY,
  memoryDbPath: process.env.NIYAH_MEMORY_DB || undefined,
});

app.post('/api/v1/ask', async (req, res) => {
  const { query, mode, forceFresh } = req.body || {};
  if (!query || typeof query !== 'string') return res.status(400).json({ error: 'query required' });
  if (mode && !['hybrid', 'search-only', 'llm-only'].includes(mode)) {
    return res.status(400).json({ error: 'mode must be hybrid, search-only, or llm-only' });
  }

  const start = Date.now();
  try {
    let niyahResult = null;
    if (mode !== 'llm-only') {
      try {
        niyahResult = await hybridEngine.ask(query, { forceFresh: Boolean(forceFresh) });
      } catch (err) {
        console.error('[hybrid] search failed:', err.message);
      }
    }

    const llmAlive = mode !== 'search-only' && await phiHealthCheck();

    if (llmAlive && niyahResult?.answer && niyahResult.confidence > 0) {
      const improved = await phiImproveAnswer(query, niyahResult.answer, niyahResult.citations || []);
      if (improved) {
        return res.json({
          answer: improved,
          citations: niyahResult.citations || [],
          confidence: niyahResult.confidence,
          fromMemory: niyahResult.fromMemory || false,
          tookMs: Date.now() - start,
          trace: niyahResult.trace || [],
          mode: 'hybrid',
          llm: true,
        });
      }
    }

    if (llmAlive && (!niyahResult || !niyahResult.answer || niyahResult.confidence === 0)) {
      const phiResult = await phiChat(query);
      if (phiResult?.answer) {
        return res.json({
          answer: phiResult.answer,
          citations: [],
          confidence: null,
          fromMemory: false,
          tookMs: Date.now() - start,
          trace: niyahResult?.trace || [],
          mode: 'llm-only',
          llm: true,
          llmTokens: phiResult.tokens,
        });
      }
    }

    if (niyahResult) {
      return res.json({
        ...niyahResult,
        tookMs: Date.now() - start,
        mode: 'search-only',
        llm: false,
      });
    }

    return res.status(503).json({ error: 'search and local LLM unavailable', tookMs: Date.now() - start });
  } catch (err) {
    console.error('[hybrid] error:', err);
    return res.status(500).json({ error: err.message, tookMs: Date.now() - start });
  }
});

app.get('/', (req, res) => res.json({
  service: 'Casper local agent',
  endpoints: {
    health: 'GET /health',
    ask_hybrid: 'POST /api/v1/ask',
    ask_search: 'POST /api/v1/niyah/ask',
    phi_chat: 'POST /api/v1/phi/chat',
    phi_health: 'GET /api/v1/phi/health',
    niyah_health: 'GET /api/v1/niyah/health',
    stats: 'GET /api/v1/niyah/stats',
  },
}));

const server = app.listen(PORT, HOST, () => {
  console.log(`[casper] listening on http://${HOST}:${PORT}`);
});

server.on('error', (err) => {
  console.error('[casper] server error:', err.message);
  process.exitCode = 1;
});

function shutdown() {
  hybridEngine.close();
  server.close(() => process.exit(0));
}

process.once('SIGINT', shutdown);
process.once('SIGTERM', shutdown);

module.exports = { app, server };
