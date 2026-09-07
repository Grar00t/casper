#ifndef NIYAH_TRAIN_FULL_H
#define NIYAH_TRAIN_FULL_H

#include "niyah_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Deterministic non-zero initialization for a fresh model.
 * Allocation itself remains zero-initialized so loading a checkpoint keeps
 * its existing semantics. Call this exactly once before training from scratch.
 */
void niyah_init_weights(NiyahModel *m, uint64_t seed);

/*
 * Full-parameter autoregressive training step.
 *
 * Updates token embeddings, every attention/FFN projection, RMSNorm scales,
 * and the LM head with AdamW. The causal KV cache is a deliberate truncated
 * backpropagation boundary: a token receives gradients through its own Q/K/V
 * path, while future losses do not backpropagate into earlier cached K/V
 * states. This keeps the C trainer bounded and deterministic without claiming
 * exact full-sequence BPTT.
 *
 * Returns mean next-token cross-entropy. Returns NAN for invalid token ids,
 * an oversized sequence, or non-finite arithmetic. Returns 0 for n < 2.
 */
float niyah_full_train_step(NiyahModel *m, NiyahAdam *opt,
                            const uint32_t *tokens, uint32_t n);

#ifdef __cplusplus
}
#endif

#endif /* NIYAH_TRAIN_FULL_H */
