#pragma once
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Returns -1 to quit, 0 while hidden, or flags: 1 visible, 2 focused, 4 reload. */
typedef int (*Kalam_Viewport_Pump)(void *context);
int kalam_embed(void *hwnd, Kalam_Viewport_Pump pump, void *context, const char *scene);
unsigned kalam_embed_frames(void);
unsigned kalam_embed_draw_count(void);
/* Zero clears the override. Called on the editor's render submission thread. */
void kalam_embed_exposure(float exposure);
float kalam_embed_drawn_exposure(void);
void kalam_embed_drawn_view(float view[16]);
float kalam_embed_drawn_light_x(void);
enum {
    KALAM_CAMERA_H = 1u << 0, KALAM_CAMERA_J = 1u << 1,
    KALAM_CAMERA_K = 1u << 2, KALAM_CAMERA_L = 1u << 3,
    KALAM_CAMERA_O = 1u << 4, KALAM_CAMERA_M = 1u << 5,
    KALAM_CAMERA_A = 1u << 6, KALAM_CAMERA_S = 1u << 7,
    KALAM_CAMERA_D = 1u << 8, KALAM_CAMERA_F = 1u << 9
};
void kalam_embed_camera_keys(unsigned keys);
void kalam_embed_pick(void);
void kalam_embed_clear_pick(void);
const char *kalam_embed_take_pick(void);
void kalam_select_entity(const char *id);
size_t kh_selected(unsigned char *dst, size_t capacity);
int kalam_validate_documents(const char *scene, const char *document, size_t length,
                            char *message, size_t capacity);
void kalam_preview_documents(const char *documents, size_t length);

#ifdef __cplusplus
}
#endif
