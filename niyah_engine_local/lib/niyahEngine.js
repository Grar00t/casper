'use strict';

const { SearchProvider } = require('./searchProvider');
const { MemoryStore } = require('./memory');
const { rankByRelevance, tokenize } = require('./relevance');
const { synthesize } = require('./reasoner');

class NiyahEngine {
  constructor(opts = {}) {
    this.search = new SearchProvider({
      searxngBaseUrl: opts.searxngBaseUrl,
      braveApiKey: opts.braveApiKey,
    });
    this.memory = new MemoryStore(opts.memoryDbPath);
    this.searchResultCount = opts.searchResultCount || 8;
    this.pagesToFetch = opts.pagesToFetch || 4;
  }

  async ask(query, opts = {}) {
    const start = Date.now();
    query = (query || '').trim();
    if (!query) {
      return { answer: 'Empty query.', citations: [], confidence: 0, fromMemory: false, tookMs: 0, trace: [] };
    }

    const trace = [];
    const t = () => Date.now() - start;

    /* Step 1: Tokenize */
    const tokens = tokenize(query);
    trace.push({
      step: 'TOKENIZE',
      ms: t(),
      detail: `${tokens.length} tokens extracted`,
      data: tokens.slice(0, 8).join(' · '),
    });

    /* Step 2: Memory check */
    if (!opts.forceFresh) {
      const cached = this.memory.recall(query);
      if (cached) {
        trace.push({
          step: 'MEMORY HIT',
          ms: t(),
          detail: `cosine similarity = ${cached.similarity.toFixed(3)} ≥ threshold 0.55`,
          data: `matched: "${cached.query.substring(0, 50)}"`,
        });
        return {
          answer: cached.answer, citations: cached.sources,
          confidence: cached.confidence, fromMemory: true,
          memorySimilarity: cached.similarity, tookMs: t(), trace,
        };
      }
      trace.push({
        step: 'MEMORY MISS',
        ms: t(),
        detail: `no cached entry with similarity ≥ 0.55`,
        data: `${this.memory._allRows().length} entries checked`,
      });
    }

    /* Step 3: DDG search */
    const rawResults = await this.search.search(query, this.searchResultCount);
    trace.push({
      step: 'DDG SEARCH',
      ms: t(),
      detail: `GET html.duckduckgo.com/html/?q=${encodeURIComponent(query.substring(0,30))}`,
      data: `${rawResults.length} results returned`,
    });

    if (rawResults.length === 0) {
      return {
        answer: 'DDG returned 0 results. Check internet or try a different query.',
        citations: [], confidence: 0, fromMemory: false, tookMs: t(), trace,
      };
    }

    /* Step 4: TF-IDF rank */
    const ranked = rankByRelevance(query, rawResults, (r) => `${r.title} ${r.snippet}`);
    const topCandidates = ranked.slice(0, this.pagesToFetch);
    trace.push({
      step: 'TF-IDF RANK',
      ms: t(),
      detail: `cosine similarity against query tokens`,
      data: `top score = ${(ranked[0]?.relevanceScore||0).toFixed(3)} · fetching top ${topCandidates.length}`,
    });

    /* Step 5: Fetch pages */
    const sourcesWithText = [];
    for (const candidate of topCandidates) {
      try {
        const text = await this.search.fetchPageText(candidate.url);
        if (text && text.length > 200) {
          sourcesWithText.push({ title: candidate.title, url: candidate.url, text });
        }
      } catch (err) {
        if (candidate.snippet && candidate.snippet.length > 40) {
          sourcesWithText.push({ title: candidate.title, url: candidate.url, text: candidate.snippet });
        }
      }
    }
    trace.push({
      step: 'FETCH PAGES',
      ms: t(),
      detail: `HTTP GET each URL · strip HTML · extract text`,
      data: `${sourcesWithText.length}/${topCandidates.length} pages fetched successfully`,
    });

    if (sourcesWithText.length === 0) {
      return {
        answer: 'Pages found but text extraction failed.',
        citations: [], confidence: 0, fromMemory: false, tookMs: t(), trace,
      };
    }

    /* Step 6: Extractive synthesis (TF-IDF sentence scoring) */
    const { answer, citations, confidence } = synthesize(query, sourcesWithText);
    this.memory.store(query, answer, citations, confidence);

    const tookMs = t();
    trace.push({
      step: 'EXTRACT & CITE',
      ms: tookMs,
      detail: `score sentences · pick top N · attach [n] citation refs`,
      data: `${citations.length} citations · confidence = ${confidence}`,
    });

    console.log(`[niyah] done ${tookMs}ms conf=${confidence}`);
    return { answer, citations, confidence, fromMemory: false, tookMs, trace };
  }

  recentContext(n = 5) { return this.memory.recentContext(n); }
  close() { this.memory.close(); }
}

module.exports = { NiyahEngine };
