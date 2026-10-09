#ifndef ZOS_TEST_M9_SELFTEST_INTERNAL_H
#define ZOS_TEST_M9_SELFTEST_INTERNAL_H

#define M9S_ROOT "/ZOSTEST"
void m9s_puts(const char *text);
void m9s_hex8(uint8_t value);
void m9s_fail(uint8_t code);
int m9s_root_mounted(void);

#endif
