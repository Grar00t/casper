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

/* C11 audit bridge.
 * Sends bounded JSON to niyah_hybrid --audit-stdin. The C result is a local
 * gate result plus an unkeyed SHA-256 integrity receipt. It is not a factual
 * truth, authenticity, or compliance attestation.
 */
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

function c11Audit(prompt, text, rules) {
  return new Promise((resolve) => {
    if (!C11_EXE) {
      console.warn('[c11] niyah_hybrid executable not found — skipping audit');
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
          console.log(`[c11] local_gate=${result.verified} hash=${(result.chain_hash || '').substring(0, 12)}...`);
          resolve(result);
        } catch (parseErr) {
          console.error('[c11] JSON parse error:', parseErr.message, '| raw:', stdout.substring(0, 100));
          resolve(null);
        }
      }
    );

    child.stdin.on('error', (err) => {
      console.error('[c11] stdin error:', err.message);
    });
    child.stdin.end(payload, 'utf8');
  });
}

router.post('/ask', async (req, res) => {
  const { query, forceFresh } = req.body || {};
  if (!query || typeof query !== 'string') {
    return res.status(400).json({ error: 'query مطلوب.' });
  }
  try {
    const result = await engine.ask(query, { forceFresh: Boolean(forceFresh) });

    let c11 = null;
    if (result.answer && result.answer.length > 10) {
      c11 = await c11Audit(query, result.answer);
    }

    if (c11) {
      result.local_gate_verified     = Boolean(c11.verified);
      result.verification_scope      = c11.verification_scope || null;
      result.factual_truth_verified  = false;
      result.integrity_receipt = c11.chain_hash ? {
        kind: c11.proof_kind || 'NIYAH-PROOF-V2',
        sha256: c11.chain_hash,
        rules_bound: Boolean(c11.rules_bound),
        rules_sha256: c11.rules_hash || null,
      } : null;

      // Legacy field retained fail-closed: this bridge creates an unkeyed
      // integrity receipt but does not independently verify authenticity.
      result.proof_verified = false;
      result.chain_hash = c11.chain_hash || null;
      result.khz_energy = c11.khz_energy;
      result.khz_penalty = c11.khz_penalty;
      result.rule_violation = c11.rule_violation || null;
      result.c11_error = c11.error || null;
      if (typeof c11.confidence === 'number') {
        result.confidence = Math.min(result.confidence || 0, c11.confidence);
      }
      result.c11_elapsed_ms = c11.elapsed_ms;
    } else {
      result.local_gate_verified = false;
      result.factual_truth_verified = false;
      result.integrity_receipt = null;
      result.proof_verified = false;
      result.chain_hash = null;
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
