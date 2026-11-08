/* Deterministic V2 legacy and V1 bound receipt fixtures. */
#include "proof_generator.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    uint8_t digest[32];
    if (argc != 2 && argc != 4) return 2;
    niyah_proof_generate("hello", "world", NULL, digest);
    if (niyah_proof_save(argv[1], digest, "hello", "world", NULL) != 0)
        return 1;
    if (argc == 4) {
        static const char rule[] = "rule source, exact bytes\n";
        FILE *f = fopen(argv[3], "wb");
        if (!f) return 1;
        if (fwrite(rule, 1u, sizeof(rule) - 1u, f) != sizeof(rule) - 1u) {
            (void)fclose(f);
            return 1;
        }
        if (fclose(f) != 0) return 1;
        if (niyah_proof_generate_v1("hello", "world", argv[3], digest) != 0)
            return 1;
        if (niyah_proof_save_v1(argv[2], digest, "hello", "world", argv[3]) != 0)
            return 1;
        if (!niyah_proof_verify_saved_v1(argv[2], argv[3], NULL, NULL))
            return 1;
    }
    return 0;
}
