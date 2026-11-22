#pragma once
#include <stdint.h>
#include <SDL3/SDL.h>
#include <bgfx/c99/bgfx.h>
#ifdef __cplusplus
extern "C" {
#endif
int kh_run(int frames, const char *scene, int lens);
#ifdef KALAM_GAME_APPLICATION
const char *kh_register_game(void);
#endif
int kh_asset(const char *name, void *destination, int capacity);
void *kh_load_asset(const char *name, size_t *size);
int renderer_open(void *native_window, uint32_t width, uint32_t height);
int renderer_resize(uint32_t width, uint32_t height);
void renderer_close(void);
extern const char *capture_prefix;
extern bgfx_renderer_type_t requested_backend;
#ifdef __cplusplus
}
#endif
