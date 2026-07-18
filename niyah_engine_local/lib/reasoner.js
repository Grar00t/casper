'use strict';

const { extractTopSentences, rankByRelevance } = require('./relevance');

function synthesize(query, sourcesWithText, opts = {}) {
  const sentencesPerSource = opts.sentencesPerSource || 2;
  const maxSentencesTotal = opts.maxSentencesTotal || 6;

  const candidateClaims = [];

  sourcesWithText.forEach((src, idx) => {
    const top = extractTopSentences(query, src.text, sentencesPerSource);
    for (const t of top) {
      candidateClaims.push({
        text: t.text,
        sourceIndex: idx,
        sourceUrl: src.url,
        sourceTitle: src.title,
        score: t.relevanceScore,
      });
    }
  });

  if (candidateClaims.length === 0) {
    return {
      answer: 'لم يُعثر على معلومة كافية الصلة بالسؤال داخل المصادر المسترجعة. أعد صياغة السؤال أو وسّع نطاق البحث.',
      citations: [],
      confidence: 0,
    };
  }

  candidateClaims.sort((a, b) => b.score - a.score);
  const selected = [];
  const perSourceCount = new Map();
  for (const claim of candidateClaims) {
    if (selected.length >= maxSentencesTotal) break;
    const count = perSourceCount.get(claim.sourceIndex) || 0;
    if (count >= sentencesPerSource) continue;
    selected.push(claim);
    perSourceCount.set(claim.sourceIndex, count + 1);
  }

  const citationMap = new Map();
  const citations = [];
  for (const claim of selected) {
    if (!citationMap.has(claim.sourceIndex)) {
      citationMap.set(claim.sourceIndex, citations.length + 1);
      citations.push({ n: citations.length + 1, title: claim.sourceTitle, url: claim.sourceUrl });
    }
  }

  const answerLines = selected.map((claim) => {
    const n = citationMap.get(claim.sourceIndex);
    return `${claim.text} [${n}]`;
  });

  const avgScore = selected.reduce((s, c) => s + c.score, 0) / selected.length;
  const independentSources = citationMap.size;
  const agreementBoost = Math.min(independentSources / 3, 1);
  const confidence = Math.round(Math.min(avgScore * 0.7 + agreementBoost * 0.3, 1) * 100) / 100;

  return { answer: answerLines.join('\n'), citations, confidence };
}

module.exports = { synthesize };
