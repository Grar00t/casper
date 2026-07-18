'use strict';

const express = require('express');
const { execFile } = require('child_process');
const path = require('path');
const fs = require('fs');
const { NiyahEngine } = require('../lib/niyahEngine');

const router = express.Router();

const engine = new NiyahEngine({
  searxngBaseUrl: process.env.SEARXNG_BASE_URL,
  braveApiKey: process.env.BRAVE_API_KEY,
  memoryDbPath: process.env.NIYAH_MEMORY_DB || undefined,
});

/* ── C11 audit bridge ────────────────────────────────────────────────────────
 * Sends { prompt, text, rules } as JSON to niyah_hybrid.exe --audit-stdin
 * via stdin pipe. Returns parsed JSON or null on error/timeout.
 * Uses the compiled binary next to this server: app/niyah_hybrid.exe
 * or the build output at Core_CPP/niyah_hybrid.exe.
 * ──────────────────────────────────────────────────────────────────────────── */
const C11_EXE = (() => {
  const candidates = [
    process.env.NIYAH_HYBRID_EXE,
    path.join(__dirname, '../../app/niyah_hybrid.exe'),
    path.join(__dirname, '../../Core_CPP/niyah_hybrid.exe'),
    path.join(__dirname, '../../../Core_CPP/niyah_hybrid.exe'),
  ].filter(Boolean);
  for (const p of candidates) {
    try { if (fs.existsSync(p)) return p; } catch (_) {}
  }
  return null;
})();

const DEFAULT_RULES = (() => {
  const candidates = [
    process.env.NIYAH_RULES,
    path.join(__dirname, '../../Data_Training/safety.nrule'),
    path.join(__dirname, '../../../Data_Training/safety.nrule'),
  ].filter(Boolean);
  for (const p of candidates) {
    try { if (fs.existsSync(p)) return p; } catch (_) {}
  }
  return null;
})();

/**
 * @param {string} prompt  — original user query
 * @param {string} text    — synthesized answer from NIYAH engine
 * @param {string} [rules] — path to .nrule file
 * @returns {Promise<{verified,chain_hash,confidence,elapsed_ms,khz_energy,rule_violation}|null>}
 */
function c11Audit(prompt, text, rules) {
  return new Promise((resolve) => {
    if (!C11_EXE) {
      console.warn('[c11] niyah_hybrid.exe not found — skipping audit');
      return resolve(null);
    }

    const payload = JSON.stringify({
      prompt: prompt.substring(0, 2048),
      text:   text.substring(0, 4096),
      rules:  rules || DEFAULT_RULES || '',
    });

    const child = execFile(
      C11_EXE,
      ['--audit-stdin'],
      { timeout: 8000, maxBuffer: 65536 },
      (err, stdout, stderr) => {
        if (err) {
          console.error('[c11] audit error:', err.message);
          if (stderr) console.error('[c11] stderr:', stderr.substring(0, 200));
          return resolve(null);
        }
        try {
          const result = JSON.parse(stdout.trim());
          console.log(`[c11] verified=${result.verified} conf=${result.confidence} hash=${(result.chain_hash||'').substring(0,12)}...`);
          resolve(result);
        } catch (parseErr) {
          console.error('[c11] JSON parse error:', parseErr.message, '| raw:', stdout.substring(0, 100));
          resolve(null);
        }
      }
    );

    // Write JSON payload to child stdin then close it
    child.stdin.write(payload, 'utf8', () => child.stdin.end());
  });
}

router.post('/ask', async (req, res) => {
  const { query, forceFresh } = req.body || {};
  if (!query || typeof query !== 'string') {
    return res.status(400).json({ error: 'query مطلوب.' });
  }
  try {
    /* Step 1: Node.js NIYAH pipeline (DDG search → fetch → TF-IDF → cite) */
    const result = await engine.ask(query, { forceFresh: Boolean(forceFresh) });

    /* Step 2: C11 symbolic audit + SHA-256 proof (non-blocking fallback) */
    let c11 = null;
    if (result.answer && result.answer.length > 10) {
      c11 = await c11Audit(query, result.answer);
    }

    /* Merge: if C11 audit ran, use its verified confidence + chain_hash */
    if (c11) {
      result.proof_verified  = c11.verified;
      result.chain_hash      = c11.chain_hash;
      result.khz_energy      = c11.khz_energy;
      result.rule_violation  = c11.rule_violation || null;
      /* Take minimum of both confidences — conservative & honest */
      if (typeof c11.confidence === 'number') {
        result.confidence = Math.min(result.confidence || 0, c11.confidence);
      }
      result.c11_elapsed_ms = c11.elapsed_ms;
    } else {
      result.proof_verified = false;
      result.chain_hash     = null;
    }

    return res.json(result);
  } catch (err) {
    console.error('[niyah/ask] error:', err);
    return res.status(500).json({ error: 'فشل داخلي.', details: err.message });
  }
});

router.get('/health', (req, res) => res.json({
  status: 'ok',
  searchBackend: engine.search.searxngBaseUrl ? 'searxng' : (engine.search.braveApiKey ? 'brave' : 'duckduckgo_html'),
  memoryBackend: engine.memory.useSqlite ? 'sqlite' : 'json',
  c11Auditor: C11_EXE ? { available: true, path: C11_EXE } : { available: false },
  defaultRules: DEFAULT_RULES || null,
  uptime: process.uptime(),
  timestamp: new Date().toISOString(),
}));

router.get('/context', (req, res) => {
  const n = Number(req.query.n) || 5;
  return res.json({ context: engine.recentContext(n) });
});

router.get('/stats', (req, res) => {
  try {
    const rows = engine.memory._allRows();
    return res.json({
      total_memories: rows.length,
      avg_confidence: rows.length > 0 ? Math.round(rows.reduce((s,r) => s+r.confidence,0)/rows.length*100)/100 : 0,
      memory_backend: engine.memory.useSqlite ? 'sqlite' : 'json',
      uptime_seconds: Math.round(process.uptime()),
      node_version: process.version,
    });
  } catch (err) {
    return res.status(500).json({ error: err.message });
  }
});

router.delete('/memory', (req, res) => {
  try {
    if (engine.memory.useSqlite) engine.memory.db.exec('DELETE FROM memories');
    else engine.memory._writeJson([]);
    return res.json({ status: 'ok', message: 'cleared' });
  } catch (err) {
    return res.status(500).json({ error: err.message });
  }
});

process.on('SIGINT', () => { engine.close(); process.exit(0); });

module.exports = router;
