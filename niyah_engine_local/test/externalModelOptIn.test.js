'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const { spawnSync } = require('node:child_process');
const path = require('node:path');

test('external model adapter makes zero requests unless explicitly enabled', () => {
  for (const setting of ['', '0', 'true']) {
    const result = spawnSync(process.execPath, ['-e', `
      const assert = require('node:assert/strict');
      let calls = 0;
      global.fetch = async () => { calls++; throw new Error('network forbidden'); };
      const adapter = require('./lib/phiEngine');
      (async () => {
        assert.equal(adapter.EXTERNAL_LLM_ENABLED, false);
        assert.equal(await adapter.phiHealthCheck(), false);
        assert.equal(await adapter.phiModelInfo(), null);
        assert.equal(await adapter.phiChat('test'), null);
        assert.equal(await adapter.phiImproveAnswer('test', 'source'), null);
        assert.equal(calls, 0);
      })().catch(error => { console.error(error); process.exitCode = 1; });
    `], { cwd: path.resolve(__dirname, '..'),
      env: { ...process.env, CASPER_ENABLE_EXTERNAL_LLM: setting }, encoding: 'utf8' });
    assert.equal(result.status, 0, result.stderr || String(result.error));
  }
});
