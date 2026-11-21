// SPDX-License-Identifier: MIT
// Recompute the checksum of compiler-produced containers and compare with the stored one.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
void dxbc_checksum(const uint8_t* data, size_t size, uint32_t out[4]);
int main(int argc, char** argv) {
    int bad = 0;
    for (int i = 1; i < argc; i++) {
        FILE* f = fopen(argv[i], "rb");
        if (!f) { printf("cannot open %s\n", argv[i]); return 2; }
        std::vector<uint8_t> d; int c; while ((c = fgetc(f)) != EOF) d.push_back((uint8_t)c); fclose(f);
        uint32_t s[4]; dxbc_checksum(d.data(), d.size(), s);
        bool ok = !memcmp(s, d.data() + 4, 16);
        printf("%s %s\n", ok ? "PASS" : "FAIL", argv[i]); bad += !ok;
    }
    return bad;
}
