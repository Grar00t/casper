'use strict';

const DEFAULT_TIMEOUT_MS = 8000;

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
      /<a[^>]+rel="nofollow"[^>]+class="result__url"[^>]+href="([^"]+)"[^>]*>([\s\S]*?)<\/a>/g,
      /<a[^>]+href="\/l\/\?uddg=([^"]+)"[^>]*>([\s\S]*?)<\/a>/g,
    ];
    const snippetPatterns = [
      /<a[^>]+class="result__snippet"[^>]*>([\s\S]*?)<\/a>/g,
      /<td[^>]+class="result__snippet"[^>]*>([\s\S]*?)<\/td>/g,
      /<div[^>]+class="result__snippet"[^>]*>([\s\S]*?)<\/div>/g,
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
    const res = await this._fetchWithTimeout(pageUrl, {
      headers: { 'User-Agent': 'Mozilla/5.0 (NIYAH sovereign search client)' },
    });
    if (!res.ok) throw new Error(`fetchPageText HTTP ${res.status} for ${pageUrl}`);
    const contentType = res.headers.get('content-type') || '';
    if (!contentType.includes('text/html') && !contentType.includes('text/plain')) {
      throw new Error(`unsupported content-type: ${contentType}`);
    }
    const html = await res.text();
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
