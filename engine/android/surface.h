#pragma once
#include <stdint.h>

/* The caller owns one reference to the returned ANativeWindow. */
void *kh_android_acquire_surface(uint64_t *generation);
void kh_android_release_surface(void *window);
