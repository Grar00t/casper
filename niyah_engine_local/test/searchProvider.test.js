'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const { SearchProvider } = require('../lib/searchProvider');

const provider = new SearchProvider();

test('rejects local and private literal page URLs', async () => {
  const blocked = [
    'http://localhost/',
    'http://api.localhost/',
    'http://127.0.0.1/',
    'http://10.0.0.1/',
    'http://169.254.169.254/latest/meta-data/',
    'http://172.16.0.1/',
    'http://192.168.1.1/',
    'http://[::1]/',
    'http://[fc00::1]/',
    'http://[fe80::1]/',
    'http://[::ffff:127.0.0.1]/',
  ];

  for (const url of blocked) {
    await assert.rejects(provider._validatePublicPageUrl(url), /not public|non-public/);
  }
});

test('rejects credentials and non-http protocols', async () => {
  await assert.rejects(provider._validatePublicPageUrl('file:///etc/passwd'), /http or https/);
  await assert.rejects(provider._validatePublicPageUrl('https://user:pass@93.184.216.34/'), /credentials/);
});

test('accepts a public literal address without DNS lookup', async () => {
  const parsed = await provider._validatePublicPageUrl('https://93.184.216.34/path?q=1');
  assert.equal(parsed.protocol, 'https:');
  assert.equal(parsed.hostname, '93.184.216.34');
});

test('bounded reader accepts content within the byte limit', async () => {
  const chunks = [Buffer.from('abc'), Buffer.from('def')];
  let index = 0;
  const response = {
    headers: { get: () => null },
    body: {
      getReader() {
        return {
          async read() {
            if (index >= chunks.length) return { done: true, value: undefined };
            return { done: false, value: chunks[index++] };
          },
          async cancel() {},
        };
      },
    },
  };

  assert.equal(await provider._readTextLimited(response, 6), 'abcdef');
});

test('bounded reader rejects content over the byte limit', async () => {
  const chunks = [Buffer.from('abcd'), Buffer.from('efgh')];
  let index = 0;
  let cancelled = false;
  const response = {
    headers: { get: () => null },
    body: {
      getReader() {
        return {
          async read() {
            if (index >= chunks.length) return { done: true, value: undefined };
            return { done: false, value: chunks[index++] };
          },
          async cancel() { cancelled = true; },
        };
      },
    },
  };

  await assert.rejects(provider._readTextLimited(response, 6), /exceeds 6 bytes/);
  assert.equal(cancelled, true);
});
