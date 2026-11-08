/* NIYAH-PROOF-V1 golden vector, strict receipt, and tamper regression. */
#include "proof_generator.h"
#include <stdio.h>
#include <string.h>

static int bad;
static void check(int condition, const char *label) {
    printf("PROOF_V1_%s=%s\n", label, condition ? "PASS" : "FAIL");
    if (!condition) ++bad;
}
int main(void) {
    static const char rule[] = "build/release/proof_v1_rules.bin";
    static const char receipt[] = "build/release/proof_v1_bound.proof";
    static const char unbound[] = "build/release/proof_v1_unbound.proof";
    static const char golden[] =
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
    uint8_t hash[32];
    char hex[65];
    FILE *f;
    bool bound = false, verified = false;
    f = fopen(rule, "wb");
    if (!f) return 2;
    if (fwrite("c", 1u, 1u, f) != 1u) return 2;
    if (fclose(f) != 0) return 2;
    check(niyah_proof_generate_v1("a", "b", rule, hash) == 0, "GENERATE");
    niyah_hash_to_hex(hash, hex);
    check(strcmp(hex, golden) == 0, "SHA256_ABC_GOLDEN");
    check(niyah_proof_save_v1(receipt, hash, "a", "b", rule) == 0, "SAVE_BOUND");
    check(niyah_proof_verify_saved_v1(receipt, rule, &bound, &verified)
          && bound && verified, "VERIFY_BOUND");
    check(!niyah_proof_verify_saved_v1(receipt, NULL, &bound, &verified)
          && bound && !verified, "REQUIRE_RULE_FILE");
    check(niyah_proof_verify_saved(receipt, rule, &bound, &verified)
          && bound && verified, "DEFAULT_VERIFY_DISPATCH");
    f = fopen(rule, "wb");
    if (!f) return 2;
    if (fwrite("d", 1u, 1u, f) != 1u) return 2;
    if (fclose(f) != 0) return 2;
    check(!niyah_proof_verify_saved_v1(receipt, rule, &bound, &verified),
          "REJECT_MUTATED_RULE");
    check(niyah_proof_generate_v1("a", "bc", NULL, hash) == 0,
          "GENERATE_NO_RULE");
    niyah_hash_to_hex(hash, hex);
    check(strcmp(hex, golden) == 0, "NO_RULE_GOLDEN");
    check(niyah_proof_save_v1(unbound, hash, "a", "bc", NULL) == 0,
          "SAVE_UNBOUND");
    check(niyah_proof_verify_saved_v1(unbound, NULL, &bound, &verified)
          && !bound && verified, "VERIFY_UNBOUND");
    f = fopen(unbound, "ab");
    if (!f) return 2;
    if (fwrite("unexpected: extra\n", 1u, sizeof("unexpected: extra\n") - 1u, f)
           != sizeof("unexpected: extra\n") - 1u) return 2;
    if (fclose(f) != 0) return 2;
    check(!niyah_proof_verify_saved_v1(unbound, NULL, &bound, &verified),
          "REJECT_EXTRA_FIELD");
    /* Leave a valid rules-bound V1 receipt for both CLI verification gates. */
    f = fopen("build/release/v1_rules_fixture.bin", "wb");
    if (!f) return 2;
    if (fwrite("c", 1u, 1u, f) != 1u) return 2;
    if (fclose(f) != 0) return 2;
    check(niyah_proof_generate_v1("a", "b", "build/release/v1_rules_fixture.bin", hash) == 0 &&
          niyah_proof_save_v1("build/release/v1_fixture.proof", hash, "a", "b",
                              "build/release/v1_rules_fixture.bin") == 0 &&
          niyah_proof_verify_saved_v1("build/release/v1_fixture.proof",
                                      "build/release/v1_rules_fixture.bin",
                                      &bound, &verified) && bound && verified,
          "CROSS_CLI_FIXTURE");
    puts(bad ? "PROOF_V1_REGRESSION=FAIL" : "PROOF_V1_REGRESSION=PASS");
    return bad ? 1 : 0;
}
