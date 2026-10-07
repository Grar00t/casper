'use strict';

const dns = require('dns').promises;
const net = require('net');

const DEFAULT_TIMEOUT_MS = 8000;
const MAX_PAGE_BYTES = 2 * 1024 * 1024;

class SearchProvider {
  constructor(opts = {}) {
    this.searxngBaseUrl = opts.searxngBaseUrl || process.env.SEARXNG_BASE_URL || '';
    this.braveApiKey = opts.braveApiKey || process.env.BRAVE_API_KEY || '';
    this.timeoutMs = opts.timeoutMs || DEFAULT_TIMEOUT_MS;
  }

  async _fetchWithTimeout(url, options = {}) {
    const controller = new AbortController();
    const t = setTimeout(() => controller.abort(), this.timeoutMs);
    try {
      const res = await fetch(url, { ...options, signal: controller.signal });
      return res;
    } finally {
      clearTimeout(t);
    }
  }

  _isUnsafeAddress(address) {
    const ipVersion = net.isIP(address);
    if (ipVersion === 4) {
      const [a, b, c] = address.split('.').map(Number);
      return a === 0
        || a === 10
        || a === 127
        || (a === 100 && b >= 64 && b <= 127)
        || (a === 169 && b === 254)
        || (a === 172 && b >= 16 && b <= 31)
        || (a === 192 && b === 0)
        || (a === 192 && b === 168)
        || (a === 198 && (b === 18 || b === 19))
        || (a === 198 && b === 51 && c === 100)
        || (a === 203 && b === 0 && c === 113)
        || a >= 224;
    }

    if (ipVersion === 6) {
      const lower = address.toLowerCase();
      const mapped = lower.match(/^::ffff:(\d+\.\d+\.\d+\.\d+)$/);
      if (mapped) return this._isUnsafeAddress(mapped[1]);
      if (lower === '::' || lower === '::1') return true;
      if (lower === '2001:db8::' || lower.startsWith('2001:db8:')) return true;

      const first = Number.parseInt(lower.split(':', 1)[0] || '0', 16);
      if ((first & 0xfe00) === 0xfc00) return true; // unique local fc00::/7
      if ((first & 0xffc0) === 0xfe80) return true; // link local fe80::/10
      if ((first & 0xffc0) === 0xfec0) return true; // deprecated site local fec0::/10
      if ((first & 0xff00) === 0xff00) return true; // multicast ff00::/8
      return false;
    }

    return true;
  }

  async _validatePublicPageUrl(pageUrl) {
    let parsed;
    try {
      parsed = new URL(pageUrl);
    } catch {
      throw new Error('invalid page URL');
    }

    if (!['http:', 'https:'].includes(parsed.protocol)) {
      throw new Error('page URL must use http or https');
    }
    if (parsed.username || parsed.password) {
      throw new Error('page URL credentials are not allowed');
    }

    const hostname = parsed.hostname.replace(/^\[|\]$/g, '').toLowerCase();
    if (!hostname || hostname === 'localhost' || hostname.endsWith('.localhost')) {
      throw new Error('page URL host is not public');
    }

    if (net.isIP(hostname)) {
      if (this._isUnsafeAddress(hostname)) throw new Error('page URL resolves to a non-public address');
      return parsed;
    }

    const addresses = await dns.lookup(hostname, { all: true, verbatim: true });
    if (!addresses.length || addresses.some(({ address }) => this._isUnsafeAddress(address))) {
      throw new Error('page URL resolves to a non-public address');
    }

    return parsed;
  }

  async _readTextLimited(res, maxBytes = MAX_PAGE_BYTES) {
    const declared = Number.parseInt(res.headers?.get?.('content-length') || '', 10);
    if (Number.isFinite(declared) && declared > maxBytes) {
      throw new Error(`page body exceeds ${maxBytes} bytes`);
    }

    if (!res.body || typeof res.body.getReader !== 'function') {
      const text = await res.text();
      if (Buffer.byteLength(text, 'utf8') > maxBytes) {
        throw new Error(`page body exceeds ${maxBytes} bytes`);
      }
      return text;
    }

    const reader = res.body.getReader();
    const chunks = [];
    let total = 0;

    while (true) {
      const { done, value } = await reader.read();
      if (done) break;
      total += value.byteLength;
      if (total > maxBytes) {
        try { await reader.cancel(); } catch (_) {}
        throw new Error(`page body exceeds ${maxBytes} bytes`);
      }
      chunks.push(Buffer.from(value));
    }

    return Buffer.concat(chunks, total).toString('utf8');
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
        'Accept': 'text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8',
        'Accept-Language': 'en-US,en;q=0.9,ar;q=0.8',
        'Cache-Control': 'no-cache',
      },
    });
    if (!res.ok) throw new Error(`DuckDuckGo HTML HTTP ${res.status}`);
    const html = await res.text();
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
        if (href.includes('/l/?uddg=')) {
          href = decodeURIComponent(href.replace(/^.*\/l\/\?uddg=/, '').split('&')[0]);
        } else if (href.startsWith('/')) {
          href = 'https://duckduckgo.com' + href;
        }
        const title = strip(m[2]);
        if (href && title && !href.includes('duckduckgo.com') && href.startsWith('http')) {
          if (!links.some(l => l.url === href)) links.push({ url: href, title });
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

    for (let i = 0; i < Math.min(links.length, maxResults); i++) {
      results.push({ title: links[i].title, url: links[i].url, snippet: snippets[i] || '', source: 'duckduckgo_html' });
    }

    if (results.length === 0) {
      console.warn(`[niyah/search] DDG returned 0. links=${links.length} snippets=${snippets.length}`);
      if (process.env.DEBUG_SEARCH) {
        require('fs').writeFileSync('duckduckgo_debug.html', html);
      }
    }
    return results;
  }

  async fetchPageText(pageUrl) {
    const safeUrl = await this._validatePublicPageUrl(pageUrl);
    const res = await this._fetchWithTimeout(safeUrl.toString(), {
      headers: {
        'User-Agent': 'Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36',
        'Accept': 'text/html,application/xhtml+xml,*/*;q=0.8',
        'Accept-Language': 'en-US,en;q=0.9,ar;q=0.8',
      },
      redirect: 'error',
    });
    if (!res.ok) throw new Error(`fetchPageText HTTP ${res.status} for ${safeUrl}`);
    const contentType = res.headers.get('content-type') || '';
    if (!contentType.includes('text/html') && !contentType.includes('text/plain')) {
      throw new Error(`unsupported content-type: ${contentType}`);
    }
    const html = await this._readTextLimited(res);
    return this._stripHtml(html);
  }

  _stripHtml(html) {
    return html
      .replace(/<script[\s\S]*?<\/script>/gi, ' ')
      .replace(/<style[\s\S]*?<\/style>/gi, ' ')
      .replace(/<!--[\s\S]*?-->/g, ' ')
      .replace(/<[^>]+>/g, ' ')
      .replace(/&nbsp;/g, ' ').replace(/&amp;/g, '&').replace(/&quot;/g, '"').replace(/&#39;/g, "'")
      .replace(/\s+/g, ' ').trim();
  }
}

module.exports = { SearchProvider };
