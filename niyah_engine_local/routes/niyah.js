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

const C11_EXE = (() => {
  const candidates = [
    process.env.NIYAH_HYBRID_EXE,
    path.join(__dirname, '../../build/niyah_hybrid'),
    path.join(__dirname, '../../build/niyah_hybrid.exe'),
    path.join(__dirname, '../../app/niyah_hybrid.exe'),
    path.join(__dirname, '../../Core_CPP/niyah_hybrid.exe'),
  ].filter(Boolean);
  for (const candidate of candidates) {
    try {
      if (fs.existsSync(candidate)) return path.resolve(candidate);
    } catch (_) {}
  }
  return null;
})();

const DEFAULT_RULES = (() => {
  const candidates = [
    process.env.NIYAH_RULES,
    path.join(__dirname, '../../Data_Training/safety.nrule'),
    path.join(__dirname, '../../../Data_Training/safety.nrule'),
  ].filter(Boolean);
  for (const candidate of candidates) {
    try {
      if (fs.existsSync(candidate)) return path.resolve(candidate);
    } catch (_) {}
  }
  return null;
})();

function c11Audit(prompt, text, rules) {
  return new Promise((resolve) => {
    if (!C11_EXE) return resolve(null);

    const payload = JSON.stringify({
      prompt: prompt.substring(0, 2048),
      text: text.substring(0, 4096),
      rules: rules || DEFAULT_RULES || '',
    });

    const child = execFile(
      C11_EXE,
      ['--audit-stdin'],
      { timeout: 8000, maxBuffer: 65536 },
      (err, stdout) => {
        if (err) {
          console.error('[c11] audit process failed:', err.message);
          return resolve(null);
        }
        try {
          const result = JSON.parse(stdout.trim());
          if (typeof result.audit_passed !== 'boolean' || typeof result.integrity_sha256 !== 'string') {
            throw new Error('unexpected audit response');
          }
          return resolve(result);
        } catch (parseErr) {
          console.error('[c11] invalid audit response:', parseErr.message);
          return resolve(null);
        }
      },
    );

    child.stdin.on('error', () => {});
    child.stdin.end(payload, 'utf8');
  });
}

router.post('/ask', async (req, res) => {
  const { query, forceFresh } = req.body || {};
  if (!query || typeof query !== 'string') {
    return res.status(400).json({ error: 'query required' });
  }
  try {
    const result = await engine.ask(query, { forceFresh: Boolean(forceFresh) });

    let c11 = null;
    if (result.answer && result.answer.length > 10) {
      c11 = await c11Audit(query, result.answer);
    }

    if (c11) {
      result.audit_passed = c11.audit_passed;
      result.integrity_sha256 = c11.integrity_sha256;
      result.structure_energy = c11.structure_energy;
      result.rule_violation = c11.rule_violation || null;
    } else {
      result.audit_passed = null;
      result.integrity_sha256 = null;
      result.structure_energy = null;
      result.rule_violation = null;
    }

    return res.json(result);
  } catch (err) {
    console.error('[niyah/ask] error:', err);
    return res.status(500).json({ error: 'internal failure', details: err.message });
  }
});

router.get('/health', (req, res) => res.json({
  status: 'ok',
  searchBackend: engine.search.backendName(),
  memoryBackend: engine.memory.useSqlite ? 'sqlite' : 'json',
  c11Auditor: C11_EXE ? { available: true, path: C11_EXE } : { available: false },
  defaultRules: DEFAULT_RULES || null,
  uptime: process.uptime(),
  timestamp: new Date().toISOString(),
}));

router.get('/context', (req, res) => {
  const n = Math.max(1, Math.min(Number(req.query.n) || 5, 50));
  return res.json({ context: engine.recentContext(n) });
});

router.get('/stats', (req, res) => {
  try {
    const rows = engine.memory._allRows();
    return res.json({
      total_memories: rows.length,
      avg_confidence: rows.length > 0
        ? Math.round(rows.reduce((sum, row) => sum + row.confidence, 0) / rows.length * 100) / 100
        : 0,
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

module.exports = router;
