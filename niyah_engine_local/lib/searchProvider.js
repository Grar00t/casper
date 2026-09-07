'use strict';

const dns = require('dns').promises;
const net = require('net');

const DEFAULT_TIMEOUT_MS = 8000;
const MAX_PAGE_BYTES = 2 * 1024 * 1024;
const MAX_REDIRECTS = 5;

function isBlockedIp(address) {
  if (net.isIPv4(address)) {
    const p = address.split('.').map(Number);
    return p[0] === 0
      || p[0] === 10
      || p[0] === 127
      || (p[0] === 169 && p[1] === 254)
      || (p[0] === 172 && p[1] >= 16 && p[1] <= 31)
      || (p[0] === 192 && p[1] === 168)
      || p[0] >= 224;
  }

  if (net.isIPv6(address)) {
    const a = address.toLowerCase();
    if (a === '::' || a === '::1') return true;
    if (a.startsWith('fc') || a.startsWith('fd')) return true;
    if (/^fe[89ab]/.test(a)) return true;
    if (a.startsWith('::ffff:')) {
      const mapped = a.substring(7);
      return net.isIPv4(mapped) ? isBlockedIp(mapped) : true;
    }
  }
  return false;
}

async function assertPublicHttpUrl(value) {
  let url;
  try {
    url = new URL(value);
  } catch {
    throw new Error('invalid URL');
  }

  if (url.protocol !== 'http:' && url.protocol !== 'https:') {
    throw new Error(`unsupported URL scheme: ${url.protocol}`);
  }
  if (url.username || url.password) throw new Error('URL credentials are not allowed');

  const host = url.hostname.toLowerCase();
  if (host === 'localhost' || host.endsWith('.localhost') || host.endsWith('.local')) {
    throw new Error('local hostnames are not allowed');
  }

  if (net.isIP(host)) {
    if (isBlockedIp(host)) throw new Error('private or special-use address is not allowed');
    return url;
  }

  const addresses = await dns.lookup(host, { all: true, verbatim: true });
  if (!addresses.length) throw new Error('hostname did not resolve');
  if (addresses.some(({ address }) => isBlockedIp(address))) {
    throw new Error('hostname resolves to a private or special-use address');
  }
  return url;
}

class SearchProvider {
  constructor(opts = {}) {
    this.searxngBaseUrl = opts.searxngBaseUrl || process.env.SEARXNG_BASE_URL || '';
    this.braveApiKey = opts.braveApiKey || process.env.BRAVE_API_KEY || '';
    this.timeoutMs = opts.timeoutMs || DEFAULT_TIMEOUT_MS;
  }

  backendName() {
    if (this.searxngBaseUrl) return 'searxng';
    if (this.braveApiKey) return 'brave';
    return 'duckduckgo_html';
  }

  async _fetchWithTimeout(url, options = {}) {
    const controller = new AbortController();
    const timer = setTimeout(() => controller.abort(), this.timeoutMs);
    try {
      return await fetch(url, { ...options, signal: controller.signal });
    } finally {
      clearTimeout(timer);
    }
  }

  async _readTextLimited(res, maxBytes = MAX_PAGE_BYTES) {
    const declared = Number(res.headers.get('content-length'));
    if (Number.isFinite(declared) && declared > maxBytes) {
      throw new Error(`response exceeds ${maxBytes} bytes`);
    }
    if (!res.body) return '';

    const reader = res.body.getReader();
    const decoder = new TextDecoder('utf-8', { fatal: false });
    let bytes = 0;
    let text = '';
    try {
      while (true) {
        const { done, value } = await reader.read();
        if (done) break;
        bytes += value.byteLength;
        if (bytes > maxBytes) throw new Error(`response exceeds ${maxBytes} bytes`);
        text += decoder.decode(value, { stream: true });
      }
      text += decoder.decode();
      return text;
    } finally {
      reader.releaseLock();
    }
  }

  async search(query, maxResults = 8) {
    if (this.searxngBaseUrl) {
      try { return await this._searchSearxng(query, maxResults); }
      catch (err) { console.warn('[niyah/search] SearXNG failed, falling back:', err.message); }
    }
    if (this.braveApiKey) {
      try { return await this._searchBrave(query, maxResults); }
      catch (err) { console.warn('[niyah/search] Brave failed, falling back:', err.message); }
    }
    return this._searchDuckDuckGoHtml(query, maxResults);
  }

  async _searchSearxng(query, maxResults) {
    const url = `${this.searxngBaseUrl.replace(/\/$/, '')}/search?q=${encodeURIComponent(query)}&format=json`;
    const res = await this._fetchWithTimeout(url, { headers: { Accept: 'application/json' } });
    if (!res.ok) throw new Error(`SearXNG HTTP ${res.status}`);
    const data = await res.json();
    const results = (data.results || []).slice(0, maxResults).map((r) => ({
      title: r.title || '', url: r.url || '', snippet: r.content || '', source: 'searxng',
    }));
    if (results.length === 0) throw new Error('SearXNG returned zero results');
    return results;
  }

  async _searchBrave(query, maxResults) {
    const url = `https://api.search.brave.com/res/v1/web/search?q=${encodeURIComponent(query)}&count=${maxResults}`;
    const res = await this._fetchWithTimeout(url, {
      headers: { Accept: 'application/json', 'X-Subscription-Token': this.braveApiKey },
    });
    if (!res.ok) throw new Error(`Brave HTTP ${res.status}`);
    const data = await res.json();
    const web = (data.web && data.web.results) || [];
    return web.slice(0, maxResults).map((r) => ({
      title: r.title || '', url: r.url || '', snippet: r.description || '', source: 'brave',
    }));
  }

  async _searchDuckDuckGoHtml(query, maxResults) {
    const url = `https://html.duckduckgo.com/html/?q=${encodeURIComponent(query)}`;
    const res = await this._fetchWithTimeout(url, {
      headers: {
        'User-Agent': 'Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36',
        Accept: 'text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8',
        'Accept-Language': 'en-US,en;q=0.9,ar;q=0.8',
        'Cache-Control': 'no-cache',
      },
    });
    if (!res.ok) throw new Error(`DuckDuckGo HTML HTTP ${res.status}`);
    const html = await this._readTextLimited(res);
    const results = [];

    const patterns = [
      /<a[^>]+class="result__a"[^>]+href="([^"]+)"[^>]*>([\s\S]*?)<\/a>/g,
      /<a[^>]+href="([^"]+)"[^>]+class="result__a"[^>]*>([\s\S]*?)<\/a>/g,
      /<a[^>]+rel="nofollow"[^>]+class="result__url"[^>]+href="([^"]+)"[^>]*>([\s\S]*?)<\/a>/g,
      /<a[^>]+href="\/l\/\?uddg=([^"]+)"[^>]*>([\s\S]*?)<\/a>/g,
      /<a[^>]+class="[^"]*result-link[^"]*"[^>]+href="([^"]+)"[^>]*>([\s\S]*?)<\/a>/g,
    ];
    const snippetPatterns = [
      /<a[^>]+class="result__snippet"[^>]*>([\s\S]*?)<\/a>/g,
      /<td[^>]+class="result__snippet"[^>]*>([\s\S]*?)<\/td>/g,
      /<div[^>]+class="result__snippet"[^>]*>([\s\S]*?)<\/div>/g,
      /<span[^>]+class="[^"]*result__snippet[^"]*"[^>]*>([\s\S]*?)<\/span>/g,
    ];

    const strip = (s) => s.replace(/<[^>]+>/g, '').replace(/\s+/g, ' ').trim();
    const links = [];
    let m;

    for (const pattern of patterns) {
      while ((m = pattern.exec(html)) !== null) {
        let href = m[1];
        try {
          if (href.includes('/l/?uddg=')) {
            href = decodeURIComponent(href.replace(/^.*\/l\/\?uddg=/, '').split('&')[0]);
          } else if (href.startsWith('/')) {
            href = 'https://duckduckgo.com' + href;
          }
        } catch {
          continue;
        }
        const title = strip(m[2]);
        if (href && title && !href.includes('duckduckgo.com') && /^https?:\/\//.test(href)) {
          if (!links.some((link) => link.url === href)) links.push({ url: href, title });
        }
      }
    }

    const snippets = [];
    for (const pattern of snippetPatterns) {
      while ((m = pattern.exec(html)) !== null) {
        const snippet = strip(m[1]);
        if (snippet && snippet.length > 20) snippets.push(snippet);
      }
    }

    for (let i = 0; i < Math.min(links.length, maxResults); i += 1) {
      results.push({
        title: links[i].title,
        url: links[i].url,
        snippet: snippets[i] || '',
        source: 'duckduckgo_html',
      });
    }
    return results;
  }

  async fetchPageText(pageUrl) {
    let current = await assertPublicHttpUrl(pageUrl);
    for (let redirects = 0; redirects <= MAX_REDIRECTS; redirects += 1) {
      const res = await this._fetchWithTimeout(current.href, {
        headers: {
          'User-Agent': 'Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36',
          Accept: 'text/html,application/xhtml+xml,*/*;q=0.8',
          'Accept-Language': 'en-US,en;q=0.9,ar;q=0.8',
        },
        redirect: 'manual',
      });

      if (res.status >= 300 && res.status < 400) {
        const location = res.headers.get('location');
        if (!location) throw new Error(`redirect ${res.status} without Location`);
        if (redirects === MAX_REDIRECTS) throw new Error('too many redirects');
        current = await assertPublicHttpUrl(new URL(location, current).href);
        continue;
      }

      if (!res.ok) throw new Error(`fetchPageText HTTP ${res.status} for ${current.href}`);
      const contentType = (res.headers.get('content-type') || '').toLowerCase();
      if (!contentType.includes('text/html') && !contentType.includes('text/plain')) {
        throw new Error(`unsupported content-type: ${contentType}`);
      }
      const html = await this._readTextLimited(res);
      return this._stripHtml(html);
    }
    throw new Error('too many redirects');
  }

  _stripHtml(html) {
    return html
      .replace(/<script[\s\S]*?<\/script>/gi, ' ')
      .replace(/<style[\s\S]*?<\/style>/gi, ' ')
      .replace(/<!--[\s\S]*?-->/g, ' ')
      .replace(/<[^>]+>/g, ' ')
      .replace(/&nbsp;/g, ' ')
      .replace(/&amp;/g, '&')
      .replace(/&quot;/g, '"')
      .replace(/&#39;/g, "'")
      .replace(/\s+/g, ' ')
      .trim();
  }
}

module.exports = { SearchProvider, assertPublicHttpUrl, isBlockedIp };
