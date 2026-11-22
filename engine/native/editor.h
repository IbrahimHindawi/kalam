#pragma once
#include "render.h"

#ifdef __cplusplus
extern "C" {
#endif
bool kh_ui_open(SDL_Window *window, const char *scene, bool smoke);
void kh_ui_close(void);
bool kh_ui_device_open(bgfx_program_handle_t program, bgfx_uniform_handle_t sampler);
void kh_ui_device_close(void);
void kh_ui_event(const SDL_Event *event);
bool kh_ui_captures_keyboard(void);
bool kh_ui_uses_escape(void);
bool kh_ui_viewport_hit(float x, float y);
bool kh_ui_viewport_rect(float rect[4]);
bool kh_ui_take_frame_selection(void);
void kh_ui_focus_viewport(void);
bool kh_ui_project_document(size_t index, bool open);
bool kh_ui_request_close(void);
bool kh_ui_reload(void);
uint32_t kh_ui_play_session(void);
void kh_ui_toggle_play_pause(void);
bool kh_ui_viewport_key(const SDL_KeyboardEvent *event);
bool kh_ui_gizmo_event(const SDL_Event *event);
void kh_ui_gizmo_target(const char *id, const float *transform, const float *view_projection);
void kh_ui_planar_gizmo(size_t document, const char *array, size_t index, const float *transform,
                        const float *view_projection);
int kh_ui_draw(float exposure);
void kalam_select_entity(const char *id);
size_t kh_selected(unsigned char *dst, size_t capacity);
int kalam_validate_documents(const char *scene, const char *documents, size_t length,
                             char *message, size_t capacity);
void kalam_preview_documents(const char *documents, size_t length);
size_t kalam_project_document(const char *scene, size_t index, char *path, size_t capacity);
size_t kalam_project_smoke_document(const char *scene, char *path, size_t path_capacity,
                                    char *text, size_t text_capacity);
#ifdef __cplusplus
}
#endif
