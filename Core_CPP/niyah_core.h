/* niyah_core.h - Casper/NIYAH neural runtime public API. */
#ifndef NIYAH_CORE_H
#define NIYAH_CORE_H

#include <float.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NIYAH_MAGIC UINT32_C(0x4E595148)
#define NIYAH_VER UINT32_C(0x0005)
#define NIYAH_MAX_CTX UINT32_C(8192)
#define NIYAH_MAX_VOCAB UINT32_C(131072)

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t embed_dim;
    uint32_t n_heads;
    uint32_t n_kv_heads;
    uint32_t n_layers;
    uint32_t ffn_mult;
    uint32_t vocab_size;
    uint32_t ctx_len;
    float rope_theta;
    float rms_eps;
    uint32_t flags;
    uint8_t _pad[16];
} NiyahConfig;

typedef struct {
    float *wq;
    float *wk;
    float *wv;
    float *wo;
    float *w_gate;
    float *w_up;
    float *w_down;
    float *rms_att;
    float *rms_ffn;
} NiyahLayer;

typedef struct {
    NiyahConfig cfg;
    NiyahLayer *layers;
    float *token_embed;
    float *rms_final;
    float *lm_head;
    float *kv_k;
    float *kv_v;
    float *scratch;
    float *_logits;
    void *_pool;
    size_t _pool_bytes;
    uint32_t head_dim;
    uint32_t kv_dim;
    uint32_t ffn_dim;
} NiyahModel;

typedef struct {
    float *m;
    float *v;
    uint32_t step;
    float lr;
    float beta1;
    float beta2;
    float eps;
    float wd;
    size_t n_weights;
} NiyahAdam;

typedef struct {
    float temperature;
    float top_p;
    uint64_t seed;
} NiyahSampler;

NiyahModel *niyah_alloc(const NiyahConfig *cfg);
void niyah_free(NiyahModel *m);

/* Deterministic Xavier-style initialization. The same seed/config is bitwise
 * reproducible on the same floating-point implementation. */
void niyah_init_weights(NiyahModel *m, uint64_t seed);

int niyah_save(const NiyahModel *m, const char *path);
int niyah_load(NiyahModel **out, const char *path);

float *niyah_forward(NiyahModel *m, uint32_t token, uint32_t pos);
uint32_t niyah_sample(const float *logits, uint32_t vocab_size, NiyahSampler *s);

/* Compatibility trainer: updates only lm_head. Full-model training is provided
 * by tools/train_casper.py and exports this same .bin weight layout. */
float niyah_train_step(NiyahModel *m, NiyahAdam *opt,
                       const uint32_t *tokens, uint32_t n);
NiyahAdam *niyah_adam_alloc(const NiyahModel *m);
void niyah_adam_free(NiyahAdam *opt);

const char *niyah_simd_name(void);
size_t niyah_param_count(const NiyahModel *m);

typedef struct NiyahRuleKBTag NiyahRuleKBOpaque;

typedef struct {
    void *rules;
    uint32_t max_retries;
    bool generate_proof;
} NiyahHybridOpts;

char *niyah_hybrid_generate(NiyahModel *m, const char *prompt,
                            const NiyahHybridOpts *opts,
                            NiyahSampler *sampler,
                            uint8_t proof_out[32]);

#ifdef __cplusplus
}
#endif
#endif /* NIYAH_CORE_H */
