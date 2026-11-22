#include "render.h"
#ifdef KALAM_NATIVE_EDITOR
#include "editor.h"
#endif
#ifdef KALAM_EMBEDDED
#define SDL_MAIN_HANDLED
#include "../editor/embedded.h"
#endif
#include <SDL3/SDL_main.h>
#include <limits.h>
#include <string.h>
#ifdef __ANDROID__
#include "../android/surface.h"
#endif

static SDL_Window *window;
static SDL_AtomicInt background, surface_dirty;
static int active, last_w, last_h;
static void *last_surface;
static uint64_t last_generation;
static Uint64 previous;
static unsigned lens_edges;
static bool play_pressed[SDL_SCANCODE_COUNT];
static bool game_handles_escape, game_back_pressed;
#ifndef KALAM_NATIVE_EDITOR
static bool play_paused;
#endif
static SDL_AudioStream *play_audio;
static bool play_audio_attempted;
#ifdef KALAM_NATIVE_EDITOR
static bool native_pick, native_deselect;
static bool native_camera_drag, native_camera_motion;
static float native_pick_x, native_pick_y;

static void native_stop_camera(void) {
    if (native_camera_drag) {
        SDL_SetWindowRelativeMouseMode(window, false);
        native_camera_drag = false;
    }
}
#endif
#ifdef KALAM_EMBEDDED
static void *embedded_hwnd;
static Kalam_Viewport_Pump embedded_pump;
static void *embedded_context;
static bool embedded_input;
static unsigned camera_keys;
static bool pick_requested, pick_ready;
static char picked_entity[65];
void kalam_embed_camera_keys(unsigned keys) { camera_keys = keys; }
void kalam_embed_pick(void) { pick_requested = true; }
const char *kalam_embed_take_pick(void) {
    if (!pick_ready) return NULL;
    pick_ready = false;
    return picked_entity;
}
#endif
const char *capture_prefix;
static bool test_lifecycle;
static int test_step;
static Uint64 test_step_start;
#ifdef __ANDROID__
bgfx_renderer_type_t requested_backend = BGFX_RENDERER_TYPE_VULKAN;
#else
bgfx_renderer_type_t requested_backend = BGFX_RENDERER_TYPE_DIRECT3D11;
#endif
static char capture_staging[2048], capture_name[2048], capture_output[2048];

void *kh_load_asset(const char *name, size_t *size) {
    char path[2048];
#ifdef __ANDROID__
    int count = SDL_snprintf(path, sizeof(path), "%s", name);
#else
    const char *root = SDL_getenv("KALAM_ASSET_ROOT");
#if defined(KALAM_EMBEDDED) || defined(KALAM_NATIVE_EDITOR) || defined(KALAM_GAME_APPLICATION)
    const char *contents = SDL_getenv("KALAM_CONTENT_ROOT");
    if (contents && SDL_strncmp(name, "shaders/", 8) != 0)
        root = contents;
#endif
    int count = root ? SDL_snprintf(path, sizeof(path), "%s/%s", root, name)
                     : SDL_snprintf(path, sizeof(path), "%sassets/%s", SDL_GetBasePath(), name);
#endif
    if (count < 0 || (size_t)count >= sizeof(path))
        return NULL;
    void *data = SDL_LoadFile(path, size);
    if (!data)
        SDL_Log("read %s: %s", path, SDL_GetError());
    return data;
}

int kh_asset(const char *name, void *dst, int capacity) {
    size_t size;
    void *data = kh_load_asset(name, &size);
    if (!data)
        return -1;
    int result = -1;
    if (capacity >= 0 && size <= (size_t)capacity) {
        memcpy(dst, data, size);
        result = (int)size;
    }
    SDL_free(data);
    return result;
}

static bool SDLCALL watch_lifecycle(void *context, SDL_Event *event) {
    (void)context;
    if (event->type == SDL_EVENT_WILL_ENTER_BACKGROUND) {
        SDL_SetAtomicInt(&background, 1);
        SDL_SetAtomicInt(&surface_dirty, 1);
    } else if (event->type == SDL_EVENT_DID_ENTER_FOREGROUND)
        SDL_SetAtomicInt(&background, 0);
    return true;
}

static void *surface(uint64_t *generation) {
#ifdef __ANDROID__
    return kh_android_acquire_surface(generation);
#else
    *generation = 0;
    return SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL);
#endif
}
static void release_surface(void *surface_handle) {
#ifdef __ANDROID__
    kh_android_release_surface(surface_handle);
#else
    (void)surface_handle;
#endif
}

#if defined(KALAM_EMBEDDED) || defined(KALAM_NATIVE_EDITOR)
static char selected_entity[65];
#endif
#ifdef KALAM_EMBEDDED
void kalam_embed_clear_pick(void) {
    pick_requested = false;
    pick_ready = false;
    selected_entity[0] = '\0';
}
#endif
#if defined(KALAM_EMBEDDED) || defined(KALAM_NATIVE_EDITOR)
void kalam_select_entity(const char *id) {
    SDL_strlcpy(selected_entity, SDL_strlen(id) < sizeof(selected_entity) ? id : "",
                sizeof(selected_entity));
}
#endif

size_t kh_selected(unsigned char *dst, size_t capacity) {
#if defined(KALAM_EMBEDDED) || defined(KALAM_NATIVE_EDITOR)
    size_t size = SDL_min(SDL_strlen(selected_entity), capacity);
    SDL_memcpy(dst, selected_entity, size);
    return size;
#else
    (void)dst;
    (void)capacity;
    return 0;
#endif
}

uint32_t kh_walk_keys(void) {
#ifdef KALAM_NATIVE_EDITOR
    return 0;
#endif
#ifdef KALAM_EMBEDDED
    if (!embedded_input)
        return 0;
#endif
    if (SDL_GetKeyboardFocus() != window)
        return 0;
    const bool *keys = SDL_GetKeyboardState(NULL);
    return (keys[SDL_SCANCODE_W] ? 1u : 0u) |
           (keys[SDL_SCANCODE_S] ? 2u : 0u) |
           (keys[SDL_SCANCODE_A] ? 4u : 0u) |
           (keys[SDL_SCANCODE_D] ? 8u : 0u);
}

uint32_t kh_editor_camera_input(float *pick_x, float *pick_y) {
#ifdef KALAM_EMBEDDED
    (void)pick_x;
    (void)pick_y;
    const bool pick = pick_requested;
    pick_requested = false;
    if (embedded_input)
        return camera_keys | (1u << 31) | (pick ? (1u << 30) : 0u);
#elif defined(KALAM_NATIVE_EDITOR)
    uint32_t input = 0;
    if (native_pick) {
        *pick_x = native_pick_x;
        *pick_y = native_pick_y;
        input |= (1u << 30) | (1u << 28);
    }
    native_pick = false;
    if (native_deselect)
        input |= 1u << 27;
    native_deselect = false;
    if (kh_ui_take_frame_selection())
        input |= 1u << 26;
    if (SDL_GetKeyboardFocus() == window) {
        if (native_camera_drag || native_camera_motion)
            input |= 1u << 29;
        if (native_camera_drag) {
            const bool *keys = SDL_GetKeyboardState(NULL);
            input |= (keys[SDL_SCANCODE_A] ? 1u << 0 : 0u) |
                     (keys[SDL_SCANCODE_S] ? 1u << 1 : 0u) |
                     (keys[SDL_SCANCODE_W] ? 1u << 2 : 0u) |
                     (keys[SDL_SCANCODE_D] ? 1u << 3 : 0u) |
                     (keys[SDL_SCANCODE_E] ? 1u << 4 : 0u) |
                     (keys[SDL_SCANCODE_Q] ? 1u << 5 : 0u);
        }
    }
    return input;
#else
    (void)pick_x;
    (void)pick_y;
#endif
    return 0;
}

void kh_editor_picked(const unsigned char *id, size_t length) {
#ifdef KALAM_EMBEDDED
    length = SDL_min(length, sizeof(picked_entity) - 1);
    SDL_memcpy(picked_entity, id, length);
    picked_entity[length] = '\0';
    kalam_select_entity(picked_entity);
    pick_ready = true;
#elif defined(KALAM_NATIVE_EDITOR)
    length = SDL_min(length, sizeof(selected_entity) - 1);
    SDL_memcpy(selected_entity, id, length);
    selected_entity[length] = '\0';
#else
    (void)id;
    (void)length;
#endif
}

uint32_t kh_play_session(void) {
#ifdef KALAM_NATIVE_EDITOR
    return kh_ui_play_session();
#else
    return play_paused ? 2u : 1u;
#endif
}

void kh_game_handles_escape(bool enabled) {
    game_handles_escape = enabled;
#ifndef KALAM_NATIVE_EDITOR
    play_paused = false;
#endif
}

void kh_toggle_play_pause(void) {
#ifdef KALAM_NATIVE_EDITOR
    kh_ui_toggle_play_pause();
#else
    play_paused = !play_paused;
#endif
}

static bool dispatch_game_escape(const SDL_Event *event) {
    if (!game_handles_escape || event->type != SDL_EVENT_KEY_DOWN || event->key.key != SDLK_ESCAPE)
        return false;
#ifdef KALAM_NATIVE_EDITOR
    if ((kh_ui_play_session() & 3u) == 0)
        return false;
    if (kh_ui_uses_escape()) {
        kh_ui_event(event);
        return true;
    }
#endif
    if (!event->key.repeat)
        game_back_pressed = true;
    return true;
}

bool kh_editor_project_document(size_t index, bool open) {
#ifdef KALAM_NATIVE_EDITOR
    return kh_ui_project_document(index, open);
#else
    (void)index;
    (void)open;
    return false;
#endif
}

void kh_editor_gizmo(const char *id, const float *transform, const float *view_projection) {
#ifdef KALAM_NATIVE_EDITOR
    kh_ui_gizmo_target(id, transform, view_projection);
#else
    (void)id;
    (void)transform;
    (void)view_projection;
#endif
}

void kh_editor_planar_gizmo(size_t document, const char *array, size_t index, const float *transform,
                            const float *view_projection) {
#ifdef KALAM_NATIVE_EDITOR
    kh_ui_planar_gizmo(document, array, index, transform, view_projection);
#else
    (void)document;
    (void)array;
    (void)index;
    (void)transform;
    (void)view_projection;
#endif
}

/* Toggle, held and pressed use separate bits so short clicks are retained. */
uint32_t kh_lens_input(float *x, float *y) {
    SDL_MouseButtonFlags buttons = SDL_GetMouseState(x, y);
    float density = SDL_GetWindowPixelDensity(window);
    *x *= density;
    *y *= density;
    return lens_edges | ((buttons & SDL_BUTTON_LMASK) && SDL_GetMouseFocus() == window ? 2u : 0u);
}

void kh_viewport_rect(float rect[4]) {
    int width, height;
    SDL_GetWindowSizeInPixels(window, &width, &height);
    rect[0] = rect[1] = 0;
    rect[2] = (float)width;
    rect[3] = (float)height;
#ifdef KALAM_NATIVE_EDITOR
    if (kh_ui_viewport_rect(rect)) {
        const float density = SDL_GetWindowPixelDensity(window);
        for (unsigned i = 0; i < 4; ++i) rect[i] *= density;
    }
#endif
}

uint32_t kh_play_input(float cursor[2], float rect[4], const uint32_t bindings[8]) {
    kh_viewport_rect(rect);
    const float density = SDL_GetWindowPixelDensity(window);
    const SDL_MouseButtonFlags buttons = SDL_GetMouseState(&cursor[0], &cursor[1]);
    bool mouse = SDL_GetMouseFocus() == window;
    bool keyboard = SDL_GetKeyboardFocus() == window;
#ifdef KALAM_NATIVE_EDITOR
    mouse = mouse && kh_ui_viewport_hit(cursor[0], cursor[1]) && !native_camera_drag;
    keyboard = keyboard && !kh_ui_captures_keyboard() && !native_camera_drag;
#endif
    cursor[0] *= density;
    cursor[1] *= density;
    mouse = mouse && cursor[0] >= rect[0] && cursor[0] < rect[0] + rect[2] &&
        cursor[1] >= rect[1] && cursor[1] < rect[1] + rect[3];
    unsigned input = mouse ? 32u : 0u;
    if (game_back_pressed && SDL_GetKeyboardFocus() == window) input |= 8192u;
    if (mouse && ((buttons & SDL_BUTTON_LMASK) || (lens_edges & 4u))) input |= 16u;
    if (mouse && (buttons & SDL_BUTTON_RMASK)) input |= 1024u;
    if (keyboard) {
        const bool *keys = SDL_GetKeyboardState(NULL);
        for (unsigned i = 0; i < 8; ++i) {
            SDL_Scancode key = SDL_GetScancodeFromKey(bindings[i], NULL);
            if (key > SDL_SCANCODE_UNKNOWN && key < SDL_SCANCODE_COUNT &&
                (i < 4 ? keys[key] : play_pressed[key]))
                input |= 1u << (i < 4 ? i : i + 2);
        }
        if (play_pressed[SDL_SCANCODE_TAB]) input |= 2048u;
        if (play_pressed[SDL_SCANCODE_F]) input |= 4096u;
    }
    return input;
}

void kh_play_audio(const float *samples, int count) {
    if (!play_audio_attempted) {
        play_audio_attempted = true;
        SDL_AudioSpec spec = { SDL_AUDIO_F32, 1, 24000 };
        if (SDL_InitSubSystem(SDL_INIT_AUDIO))
            play_audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
        if (play_audio) SDL_ResumeAudioStreamDevice(play_audio);
        else SDL_Log("Sample audio unavailable: %s", SDL_GetError());
    }
    if (play_audio && SDL_GetAudioStreamQueued(play_audio) < 24000 * (int)sizeof(float) / 8)
        SDL_PutAudioStreamData(play_audio, samples, count * (int)sizeof(float));
}

int kh_next(float *dt, float *dx, float *dy, unsigned *w, unsigned *h) {
#ifdef KALAM_EMBEDDED
    int viewport = embedded_pump(embedded_context);
    if (viewport < 0)
        return 0;
    embedded_input = (viewport & 2) != 0;
    if (!(viewport & 1)) {
        previous = SDL_GetTicksNS();
        lens_edges = 0;
        SDL_Delay(8);
        return 2;
    }
#endif
    SDL_Event e;
    *dx = *dy = 0;
#ifdef KALAM_NATIVE_EDITOR
    native_camera_motion = false;
#endif
    int width = 0, height = 0, reload = 0;
    lens_edges = 0;
    SDL_zeroa(play_pressed);
    game_back_pressed = false;
    if (test_lifecycle) {
        if (test_step == 0) {
            if (!SDL_SetWindowSize(window, 720, 480))
                return -1;
            test_step_start = SDL_GetTicks();
            test_step = 1;
        } else if (test_step == 2) {
            if (!SDL_MinimizeWindow(window))
                return -1;
            test_step_start = SDL_GetTicks();
            test_step = 3;
        } else if (test_step == 4) {
            if (!SDL_RestoreWindow(window))
                return -1;
            test_step_start = SDL_GetTicks();
            test_step = 5;
        }
        if (test_step != 6 && SDL_GetTicks() - test_step_start > 5000) {
            SDL_Log("Lifecycle check timed out at step %d", test_step);
            return -1;
        }
    }
    SDL_GetWindowSizeInPixels(window, &width, &height);
    while (SDL_PollEvent(&e)) {
#ifdef KALAM_NATIVE_EDITOR
        if (e.type == SDL_EVENT_QUIT || e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
            native_stop_camera();
            if (kh_ui_request_close()) return 0;
            continue;
        }
        if (e.type == SDL_EVENT_WINDOW_FOCUS_LOST || e.type == SDL_EVENT_WINDOW_MINIMIZED)
            native_stop_camera();
        if (e.type == SDL_EVENT_KEY_DOWN && (e.key.mod & SDL_KMOD_ALT) &&
            kh_ui_viewport_key(&e.key)) {
            native_stop_camera();
            continue;
        }
        if (native_camera_drag) {
            if (e.type == SDL_EVENT_MOUSE_BUTTON_UP && e.button.button == SDL_BUTTON_RIGHT) {
                native_stop_camera();
                continue;
            }
            if (e.type == SDL_EVENT_MOUSE_MOTION && e.motion.which != SDL_TOUCH_MOUSEID) {
                *dx += e.motion.xrel;
                *dy += e.motion.yrel;
                native_camera_motion = true;
                continue;
            }
            if (e.type == SDL_EVENT_KEY_DOWN || e.type == SDL_EVENT_KEY_UP ||
                e.type == SDL_EVENT_TEXT_INPUT || e.type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
                e.type == SDL_EVENT_MOUSE_BUTTON_UP || e.type == SDL_EVENT_MOUSE_WHEEL)
                continue;
        }
        if (kh_ui_gizmo_event(&e))
            continue;
        if (e.type == SDL_EVENT_KEY_DOWN && kh_ui_viewport_key(&e.key))
            continue;
#endif
        if (dispatch_game_escape(&e))
            continue;
#ifdef KALAM_NATIVE_EDITOR
        if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.which != SDL_TOUCH_MOUSEID &&
            kh_ui_viewport_hit(e.button.x, e.button.y)) {
            if (e.button.button == SDL_BUTTON_RIGHT && (kh_ui_play_session() & 3u) != 1u) {
                native_camera_drag = SDL_SetWindowRelativeMouseMode(window, true);
                if (native_camera_drag) {
                    kh_ui_focus_viewport();
                    continue;
                }
                SDL_Log("Viewport mouse capture: %s", SDL_GetError());
            } else if (e.button.button == SDL_BUTTON_LEFT) {
                kh_ui_focus_viewport();
                if ((kh_ui_play_session() & 3u) != 1u) {
                    const float density = SDL_GetWindowPixelDensity(window);
                    native_pick_x = e.button.x * density;
                    native_pick_y = e.button.y * density;
                    native_pick = true;
                }
            }
        }
        kh_ui_event(&e);
        if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat && !kh_ui_captures_keyboard()) {
            if (e.key.key == SDLK_ESCAPE) {
                native_deselect = true;
                native_pick = false;
                kalam_select_entity("");
            }
        }
        if (e.type == SDL_EVENT_KEY_DOWN && kh_ui_captures_keyboard())
            continue;
#endif
        if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat &&
            e.key.scancode > SDL_SCANCODE_UNKNOWN && e.key.scancode < SDL_SCANCODE_COUNT)
            play_pressed[e.key.scancode] = true;
        if (e.type == SDL_EVENT_QUIT
#if !defined(KALAM_EMBEDDED) && !defined(KALAM_NATIVE_EDITOR)
            || (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_ESCAPE)
#endif
        )
            return 0;
#ifndef KALAM_NATIVE_EDITOR
        if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_R && !e.key.repeat)
            reload = 1;
#endif
        if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_F1 && !e.key.repeat)
            lens_edges ^= 1u;
        if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.button == SDL_BUTTON_LEFT)
            lens_edges |= 4u;
#ifndef KALAM_NATIVE_EDITOR
        if (e.type == SDL_EVENT_MOUSE_MOTION && e.motion.which != SDL_TOUCH_MOUSEID &&
            (e.motion.state & SDL_BUTTON_LMASK)) {
            *dx += e.motion.xrel * SDL_GetWindowPixelDensity(window);
            *dy += e.motion.yrel * SDL_GetWindowPixelDensity(window);
        }
        if (e.type == SDL_EVENT_FINGER_MOTION && e.tfinger.touchID != SDL_MOUSE_TOUCHID) {
            *dx += e.tfinger.dx * width;
            *dy += e.tfinger.dy * height;
        }
#endif
    }
    uint64_t generation = 0;
    void *handle = surface(&generation);
    if (test_lifecycle && test_step == 3 && (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED))
        test_step = 4;
    if (active &&
        (SDL_GetAtomicInt(&surface_dirty) || handle != last_surface || generation != last_generation)) {
        renderer_close();
        release_surface(last_surface);
        last_surface = NULL;
        active = 0;
    }
    if (!handle || width <= 0 || height <= 0 || SDL_GetAtomicInt(&background) ||
        (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED)) {
        release_surface(handle);
        previous = SDL_GetTicksNS();
        SDL_Delay(16);
        return 2;
    }
    int recreated = 0;
    if (!active) {
        if (!renderer_open(handle, (uint32_t)width, (uint32_t)height)) {
            release_surface(handle);
            return -1;
        }
        active = 1;
        recreated = 1;
        SDL_SetAtomicInt(&surface_dirty, 0);
        last_surface = handle;
        last_generation = generation;
    } else {
        release_surface(handle);
        if ((width != last_w || height != last_h) && !renderer_resize(width, height))
            return -1;
    }
    last_w = width;
    last_h = height;
    if (test_lifecycle) {
        int logical_w = 0, logical_h = 0;
        SDL_GetWindowSize(window, &logical_w, &logical_h);
        if (test_step == 1 && logical_w == 720 && logical_h == 480)
            test_step = 2;
        else if (test_step == 5 && !(SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED))
            test_step = 6;
        else if (test_step != 6) {
            SDL_Delay(16);
            return 2;
        }
    }
    *w = (unsigned)width;
    *h = (unsigned)height;
    Uint64 now = SDL_GetTicksNS();
    *dt = SDL_min((float)(now - previous) * 1e-9f, 0.1f);
    previous = now;
#ifdef KALAM_EMBEDDED
    if (!embedded_input) {
        *dx = *dy = 0;
        lens_edges = 0;
        reload = 0;
    }
    reload |= (viewport & 4) != 0;
#endif
#ifdef KALAM_NATIVE_EDITOR
    if (!recreated) reload |= kh_ui_reload();
#endif
    return recreated ? 3 : reload ? 4 : 1;
}

static int run_host(int argc, char **argv) {
    int smoke = 0, result = 1;
#ifdef KALAM_GAME_APPLICATION
    const char *scene = kh_register_game();
    if (!scene) return 1;
#else
    const char *scene = "reference";
#endif
    int lens = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--smoke") == 0)
            smoke = 3;
#ifdef __ANDROID__
        else if (strcmp(argv[i], "--gles") == 0)
            requested_backend = BGFX_RENDERER_TYPE_OPENGLES;
#endif
#ifndef __ANDROID__
        else if (strcmp(argv[i], "--backend") == 0 && i + 1 < argc) {
            const char *backend = argv[++i];
            if (strcmp(backend, "vulkan") == 0)
                requested_backend = BGFX_RENDERER_TYPE_VULKAN;
            else if (strcmp(backend, "dx11") != 0)
                return 2;
        } else if (strcmp(argv[i], "--lifecycle-test") == 0) {
            test_lifecycle = true;
            smoke = 3;
        }
#endif
        else if (strcmp(argv[i], "--lens") == 0)
            lens = 1;
        else if (strcmp(argv[i], "--scene") == 0 && i + 1 < argc)
            scene = argv[++i];
        else if (strcmp(argv[i], "--capture") == 0 && i + 1 < argc) {
            const char *prefix = argv[++i];
            if (strlen(prefix) > sizeof(capture_staging) - 64)
                return 2;
            SDL_snprintf(capture_output, sizeof(capture_output), "%s.tga", prefix);
            SDL_snprintf(capture_staging, sizeof(capture_staging), "%s-%llu", prefix,
                         (unsigned long long)SDL_GetPerformanceCounter());
            SDL_snprintf(capture_name, sizeof(capture_name), "%s.tga", capture_staging);
            capture_prefix = capture_staging;
            smoke = 3;
        } else {
            SDL_Log("usage: kalam [--scene name] [--lens] [--smoke] [--capture <path-prefix>] [--backend dx11|vulkan] "
                    "[--lifecycle-test]");
            return 2;
        }
    }
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS))
        return 1;
    SDL_AddEventWatch(watch_lifecycle, NULL);
#ifdef KALAM_EMBEDDED
    SDL_PropertiesID properties = SDL_CreateProperties();
    SDL_SetPointerProperty(properties, SDL_PROP_WINDOW_CREATE_WIN32_HWND_POINTER, embedded_hwnd);
    window = SDL_CreateWindowWithProperties(properties);
    SDL_DestroyProperties(properties);
#else
#ifdef KALAM_NATIVE_EDITOR
    const SDL_WindowFlags editor_flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY |
        (smoke ? SDL_WINDOW_HIDDEN : 0);
    window = SDL_CreateWindow("Kalam Editor", 1280, 800, editor_flags);
    if (window && !kh_ui_open(window, scene, smoke != 0))
        goto finish;
#else
    window = SDL_CreateWindow("Kalam", 960, 600, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
#endif
#endif
    if (!window)
        goto finish;
    while (!last_surface || last_w <= 0 || last_h <= 0) {
        SDL_Event event;
        while (SDL_PollEvent(&event))
            if (event.type == SDL_EVENT_QUIT)
                goto finish;
        release_surface(last_surface);
        last_surface = surface(&last_generation);
        if (!SDL_GetWindowSizeInPixels(window, &last_w, &last_h))
            goto finish;
#ifdef __ANDROID__
        if (!last_surface || last_w <= 0 || last_h <= 0)
            SDL_Delay(16);
#else
        if (!last_surface || last_w <= 0 || last_h <= 0)
            goto finish;
#endif
    }
    if (!renderer_open(last_surface, last_w, last_h))
        goto finish;
    active = 1;
    previous = SDL_GetTicksNS();
    result = kh_run(smoke, scene, lens);
finish:
    if (play_audio) SDL_DestroyAudioStream(play_audio);
    play_audio = NULL;
    play_audio_attempted = false;
    if (active)
        renderer_close();
#ifdef KALAM_NATIVE_EDITOR
    native_stop_camera();
    kh_ui_close();
#endif
    release_surface(last_surface);
    last_surface = NULL;
    if (capture_prefix && result == 0) {
        SDL_PathInfo info;
        if (!SDL_GetPathInfo(capture_name, &info) || info.size < 18 ||
            !SDL_RenamePath(capture_name, capture_output)) {
            SDL_Log("Capture failed: %s", capture_output);
            result = 1;
        }
    }
    SDL_RemoveEventWatch(watch_lifecycle, NULL);
    if (window)
        SDL_DestroyWindow(window);
    SDL_Quit();
    if (result)
        SDL_Log("Kalam exited with status %d (%s)", result, SDL_GetError());
    return result;
}

#ifdef KALAM_EMBEDDED
int kalam_embed(void *hwnd, Kalam_Viewport_Pump pump, void *context, const char *scene) {
    embedded_hwnd = hwnd;
    embedded_pump = pump;
    embedded_context = context;
    SDL_SetMainReady();
    char *args[] = {"kalam-editor", "--scene", (char *)scene};
    return run_host(3, args);
}
#else
int main(int argc, char **argv) {
    return run_host(argc, argv);
}
#endif
