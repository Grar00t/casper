/* Regression coverage for probability-ranked nucleus sampling. */
#include "niyah_core.h"
#include <stdlib.h>
#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "CHECK failed %s:%d: %s\n", __FILE__, __LINE__, #expr); \
    exit(1); \
} } while (0)
#include <math.h>
#include <stdint.h>
#include <stdio.h>

static void test_greedy(void)
{
    const float logits[] = {0.0f, 1.0f, 3.0f, 2.0f};
    NiyahSampler s = {0.0f, 0.01f, UINT64_C(4)};
    CHECK(niyah_sample(logits, 4u, &s) == 2u);
}

static void test_top_p_excludes_low_mass(void)
{
    /* The highest logit lives at ID 3. For top_p 0.55 it is the only
     * candidate; vocabulary-index prefix sampling would fail. */
    const float logits[] = {-10.0f, -9.0f, 0.0f, 4.0f};
    for (uint64_t seed = 0u; seed < 100u; ++seed) {
        NiyahSampler s = {1.0f, 0.55f, seed};
        CHECK(niyah_sample(logits, 4u, &s) == 3u);
    }
}

static void test_ties_and_repeatability(void)
{
    const float logits[] = {2.0f, 2.0f, -4.0f, 2.0f};
    NiyahSampler a = {1.0f, 0.51f, UINT64_C(0xBEEF)};
    NiyahSampler b = a;
    for (int i = 0; i < 80; ++i) {
        uint32_t x = niyah_sample(logits, 4u, &a);
        uint32_t y = niyah_sample(logits, 4u, &b);
        CHECK(x == y);
        CHECK(x == 0u || x == 1u);
    }
    CHECK(a.seed == b.seed);
}

static void test_invalid_and_full_distribution(void)
{
    const float logits[] = {-1.0f, 0.0f, 1.0f};
    const float invalid[] = {0.0f, NAN, 1.0f};
    NiyahSampler s = {1.0f, 1.0f, UINT64_C(12)};
    CHECK(niyah_sample(invalid, 3u, &s) == 0u);
    CHECK(s.seed == UINT64_C(12));
    for (int i = 0; i < 100; ++i) {
        CHECK(niyah_sample(logits, 3u, &s) < 3u);
    }
    s.temperature = -1.0f;
    CHECK(niyah_sample(logits, 3u, &s) == 0u);
}

int main(void)
{
    test_greedy();
    test_top_p_excludes_low_mass();
    test_ties_and_repeatability();
    test_invalid_and_full_distribution();
    puts("NIYAH_SAMPLER_REGRESSION=PASS");
    return 0;
}
