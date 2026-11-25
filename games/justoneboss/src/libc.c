// The few libc functions the game and the compiler need (no libc is linked)
#include <stddef.h>
#include <stdint.h>

void *memset(void *d, int c, size_t n) { uint8_t *p = d; while(n--) *p++ = (uint8_t)c; return d; }
void *memcpy(void *d, const void *s, size_t n) { uint8_t *p = d; const uint8_t *q = s; while(n--) *p++ = *q++; return d; }
size_t strlen(const char *s) { const char *p = s; while(*p) p++; return p - s; }
int memcmp(const void *a, const void *b, size_t n) {
  const uint8_t *p = a, *q = b;
  for(; n; n--, p++, q++) if(*p != *q) return *p - *q;
  return 0;
}
