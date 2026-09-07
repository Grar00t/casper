'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const { SearchProvider, assertPublicHttpUrl, isBlockedIp } = require('../lib/searchProvider');

test('blocks private and special-use IPv4 addresses', () => {
  for (const ip of [
    '0.0.0.0',
    '10.0.0.1',
    '127.0.0.1',
    '169.254.169.254',
    '172.16.0.1',
    '172.31.255.255',
    '192.168.1.1',
    '224.0.0.1',
  ]) {
    assert.equal(isBlockedIp(ip), true, ip);
  }
  assert.equal(isBlockedIp('8.8.8.8'), false);
});

test('blocks local IPv6 addresses', () => {
  assert.equal(isBlockedIp('::1'), true);
  assert.equal(isBlockedIp('fc00::1'), true);
  assert.equal(isBlockedIp('fd00::1'), true);
  assert.equal(isBlockedIp('fe80::1'), true);
});

test('URL policy rejects local targets and non-http schemes without network access', async () => {
  await assert.rejects(() => assertPublicHttpUrl('http://127.0.0.1/'));
  await assert.rejects(() => assertPublicHttpUrl('http://10.0.0.1/'));
  await assert.rejects(() => assertPublicHttpUrl('http://localhost/'));
  await assert.rejects(() => assertPublicHttpUrl('file:///etc/passwd'));
  await assert.rejects(() => assertPublicHttpUrl('http://user:pass@8.8.8.8/'));
  const publicUrl = await assertPublicHttpUrl('https://8.8.8.8/example');
  assert.equal(publicUrl.protocol, 'https:');
});

test('HTML stripping removes scripts, styles, tags, and common entities', () => {
  const provider = new SearchProvider();
  const text = provider._stripHtml('<style>x{}</style><script>alert(1)</script><p>A &amp; B&nbsp;C</p>');
  assert.equal(text, 'A & B C');
});
