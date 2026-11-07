/* Deterministic receipt fixture for the native Windows CLI integration test. */
#include "proof_generator.h"
#include <stdint.h>
int main(int argc, char **argv) {
    uint8_t proof[32];
    if (argc != 2) return 2;
    niyah_proof_generate("hello", "world", NULL, proof);
    return niyah_proof_save(argv[1], proof, "hello", "world", NULL) == 0 ? 0 : 1;
}
