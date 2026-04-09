/*
 * quantize.h — NIYAH Weight Quantization Engine
 *
 * Q8_0 (8-bit) and Q4_0 (4-bit) weight quantization for sovereign local inference.
 * Enables running on $35 hardware (Raspberry Pi, etc.)
 *
 * Zero external dependencies. C11 clean. C++17 compatible.
 */
#ifndef NIYAH_QUANTIZE_H
#define NIYAH_QUANTIZE_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * Q8_0 block: 32 weights quantized to int8 with one float scale
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */
#define NIYAH_Q8_BLOCK_SIZE 32

typedef struct {
    float  scale;                          /* absmax / 127 */
    int8_t quants[NIYAH_Q8_BLOCK_SIZE];    /* quantized values */
} NiyahQ8Block;

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * Q4_0 block: 32 weights packed into 4-bit with one float scale
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */
#define NIYAH_Q4_BLOCK_SIZE 32

typedef struct {
    float   scale;                                  /* absmax / 7 */
    uint8_t quants[NIYAH_Q4_BLOCK_SIZE / 2];        /* 4-bit packed pairs */
} NiyahQ4Block;

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * Quantized matrix (row-major blocks)
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */
typedef struct {
    void     *blocks;       /* NiyahQ8Block* or NiyahQ4Block* */
    uint32_t  rows;
    uint32_t  cols;
    uint32_t  block_size;   /* NIYAH_Q8_BLOCK_SIZE or NIYAH_Q4_BLOCK_SIZE */
    uint8_t   quant_type;   /* 8 for Q8_0, 4 for Q4_0 */
} NiyahQuantMatrix;

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * Public API
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */

/* Quantize float matrix → Q8_0 */
NiyahQuantMatrix *niyah_q8_from_float(const float *data, uint32_t rows, uint32_t cols);

/* Quantize float matrix → Q4_0 */
NiyahQuantMatrix *niyah_q4_from_float(const float *data, uint32_t rows, uint32_t cols);

/* Dequantize back to float (for verification) */
void niyah_quant_to_float(const NiyahQuantMatrix *qm, float *out);

/* Quantized matrix-vector multiply: out[rows] = QM[rows×cols] @ x[cols] */
/* SIMD accelerated: AVX2+FMA on x86_64, NEON on aarch64, scalar fallback */
void niyah_q8_matvec(const NiyahQuantMatrix *qm, const float *x, float *out);
void niyah_q4_matvec(const NiyahQuantMatrix *qm, const float *x, float *out);

/* Free */
void niyah_quant_free(NiyahQuantMatrix *qm);

/* Compute quantization error (RMSE between original and dequantized) */
float niyah_quant_rmse(const float *original, const NiyahQuantMatrix *qm,
                       uint32_t rows, uint32_t cols);

/* Memory savings report */
void niyah_quant_report(const NiyahQuantMatrix *qm);

/* Smoke test — returns failed-assertion count (0 = all pass) */
int niyah_quant_smoke(void);

#ifdef __cplusplus
}
#endif
#endif /* NIYAH_QUANTIZE_H */
