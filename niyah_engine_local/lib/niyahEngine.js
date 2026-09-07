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
    const elapsed = () => Date.now() - start;

    const tokens = tokenize(query);
    trace.push({
      step: 'TOKENIZE',
      ms: elapsed(),
      detail: `${tokens.length} tokens extracted`,
      data: tokens.slice(0, 8).join(' · '),
    });

    if (!opts.forceFresh) {
      const cached = this.memory.recall(query);
      if (cached) {
        trace.push({
          step: 'MEMORY HIT',
          ms: elapsed(),
          detail: `similarity = ${cached.similarity.toFixed(3)}`,
          data: `matched: "${cached.query.substring(0, 50)}"`,
        });
        return {
          answer: cached.answer,
          citations: cached.sources,
          confidence: cached.confidence,
          fromMemory: true,
          memorySimilarity: cached.similarity,
          tookMs: elapsed(),
          trace,
        };
      }
      trace.push({
        step: 'MEMORY MISS',
        ms: elapsed(),
        detail: 'no sufficiently similar cached entry',
        data: `${this.memory._allRows().length} entries checked`,
      });
    }

    const rawResults = await this.search.search(query, this.searchResultCount);
    const actualBackend = rawResults[0]?.source || this.search.backendName();
    trace.push({
      step: 'SEARCH',
      ms: elapsed(),
      detail: `backend=${actualBackend}`,
      data: `${rawResults.length} results returned`,
    });

    if (rawResults.length === 0) {
      return {
        answer: 'Search returned no results.',
        citations: [],
        confidence: 0,
        fromMemory: false,
        tookMs: elapsed(),
        trace,
      };
    }

    const ranked = rankByRelevance(query, rawResults, (result) => `${result.title} ${result.snippet}`);
    const topCandidates = ranked.slice(0, this.pagesToFetch);
    trace.push({
      step: 'RANK',
      ms: elapsed(),
      detail: 'cosine similarity against query tokens',
      data: `top score = ${(ranked[0]?.relevanceScore || 0).toFixed(3)}; fetching ${topCandidates.length}`,
    });

    const sourcesWithText = [];
    for (const candidate of topCandidates) {
      try {
        const text = await this.search.fetchPageText(candidate.url);
        if (text && text.length > 200) {
          sourcesWithText.push({ title: candidate.title, url: candidate.url, text });
        } else if (candidate.snippet && candidate.snippet.length > 40) {
          sourcesWithText.push({ title: candidate.title, url: candidate.url, text: candidate.snippet });
        }
      } catch (_) {
        if (candidate.snippet && candidate.snippet.length > 40) {
          sourcesWithText.push({ title: candidate.title, url: candidate.url, text: candidate.snippet });
        }
      }
    }
    trace.push({
      step: 'FETCH',
      ms: elapsed(),
      detail: 'public HTTP(S) pages only; HTML stripped to text',
      data: `${sourcesWithText.length}/${topCandidates.length} sources available`,
    });

    if (sourcesWithText.length === 0) {
      return {
        answer: 'Results were found but no usable page text or snippets were available.',
        citations: [],
        confidence: 0,
        fromMemory: false,
        tookMs: elapsed(),
        trace,
      };
    }

    const { answer, citations, confidence } = synthesize(query, sourcesWithText);
    if (answer && confidence > 0) this.memory.store(query, answer, citations, confidence);

    const tookMs = elapsed();
    trace.push({
      step: 'EXTRACT',
      ms: tookMs,
      detail: 'score sentences and attach source references',
      data: `${citations.length} citations; relevance confidence=${confidence}`,
    });

    return { answer, citations, confidence, fromMemory: false, tookMs, trace };
  }

  recentContext(n = 5) { return this.memory.recentContext(n); }
  close() { this.memory.close(); }
}

module.exports = { NiyahEngine };
