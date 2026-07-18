'use strict';

const { SearchProvider } = require('./searchProvider');
const { MemoryStore } = require('./memory');
const { rankByRelevance } = require('./relevance');
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
      return { answer: 'سؤال فارغ.', citations: [], confidence: 0, fromMemory: false, tookMs: 0 };
    }

    console.log(`[niyah] query: "${query.substring(0, 60)}"`);

    if (!opts.forceFresh) {
      const cached = this.memory.recall(query);
      if (cached) {
        console.log(`[niyah] cache hit (sim=${cached.similarity.toFixed(2)})`);
        return {
          answer: cached.answer, citations: cached.sources,
          confidence: cached.confidence, fromMemory: true,
          memorySimilarity: cached.similarity, tookMs: Date.now() - start,
        };
      }
    }

    console.log(`[niyah] searching...`);
    const rawResults = await this.search.search(query, this.searchResultCount);
    console.log(`[niyah] raw results: ${rawResults.length}`);

    if (rawResults.length === 0) {
      return {
        answer: 'لم يرجع محرك البحث أي نتائج. تحقق من الاتصال بالإنترنت.',
        citations: [], confidence: 0, fromMemory: false, tookMs: Date.now() - start,
      };
    }

    const ranked = rankByRelevance(query, rawResults, (r) => `${r.title} ${r.snippet}`);
    const topCandidates = ranked.slice(0, this.pagesToFetch);
    const sourcesWithText = [];

    for (const candidate of topCandidates) {
      try {
        const text = await this.search.fetchPageText(candidate.url);
        if (text && text.length > 200) {
          sourcesWithText.push({ title: candidate.title, url: candidate.url, text });
          console.log(`[niyah] fetched ${text.length} chars from ${candidate.url.substring(0, 50)}`);
        }
      } catch (err) {
        console.warn(`[niyah] fetch failed: ${err.message}`);
        if (candidate.snippet && candidate.snippet.length > 40) {
          sourcesWithText.push({ title: candidate.title, url: candidate.url, text: candidate.snippet });
        }
      }
    }

    if (sourcesWithText.length === 0) {
      return {
        answer: 'وُجدت نتائج لكن تعذّر جلب نص منها.',
        citations: [], confidence: 0, fromMemory: false, tookMs: Date.now() - start,
      };
    }

    const { answer, citations, confidence } = synthesize(query, sourcesWithText);
    this.memory.store(query, answer, citations, confidence);
    console.log(`[niyah] done in ${Date.now() - start}ms, confidence=${confidence}`);

    return { answer, citations, confidence, fromMemory: false, tookMs: Date.now() - start };
  }

  recentContext(n = 5) { return this.memory.recentContext(n); }
  close() { this.memory.close(); }
}

module.exports = { NiyahEngine };
