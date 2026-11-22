#include "render.h"
#ifdef KALAM_NATIVE_EDITOR
#include "editor.h"
#endif
#include <limits.h>
#include <string.h>

#define INVALID UINT16_MAX
#define VALID(handle) ((handle).idx != INVALID)
enum { SHADOW, SCENE, TONE, SPRITE, SKIN_SHADOW, SKIN_SCENE, FX_QUADS, FX_MESH, MSDF_TEXT,
#ifdef KALAM_NATIVE_EDITOR
       EDITOR_UI,
#endif
       PASS_COUNT };
enum { BASE, MATERIAL, LIGHT_DIR, LIGHT_COLOR, EYE, LIGHT_VP, FLAGS, EXPOSURE, OUTLINE_COLOR, TINT, FX_UV, FX_SCROLL, FX_OPACITY, FX_MAPS, FX_DISSOLVE, FX_WAVE, FX_WAVE_SHAPE, MSDF_RANGE, UNIFORM_COUNT };
enum { BASE_TEX, MATERIAL_TEX, SHADOW_TEX, IRRADIANCE, PREFILTER, DFG, SCREEN, SKIN_PALETTE, NORMAL_TEX, FX_OPACITY_TEX, FX_RAMP_TEX, FX_DISSOLVE_TEX, SAMPLER_COUNT };
static const char *const pass_names[] = {"depth", "pbr", "tone", "sprite", "depth_skin", "pbr_skin", "world_fx", "mesh_fx", "msdf"
#ifdef KALAM_NATIVE_EDITOR
                                        , "editor_ui"
#endif
};
static const char *const uniform_names[] = {"u_baseColor",   "u_material", "u_lightDir",
                                            "u_lightColor",  "u_eye",      "u_lightViewProj",
                                            "u_cameraFlags", "u_exposure", "u_outlineColor", "u_tint",
                                            "u_fxUvRect", "u_fxScroll", "u_fxOpacity", "u_fxMaps",
                                            "u_fxDissolve", "u_fxWave", "u_fxWaveShape", "u_msdfRange"};
static const char *const sampler_names[] = {"s_baseColor", "s_material", "s_shadow", "s_irradiance",
                                            "s_prefilter", "s_dfg",      "s_color", "s_skinPalette", "s_normal",
                                            "s_fxOpacity", "s_fxRamp", "s_fxDissolve"};
static bgfx_program_handle_t programs[PASS_COUNT];
static bgfx_uniform_handle_t uniforms[UNIFORM_COUNT], samplers[SAMPLER_COUNT];
static bgfx_texture_handle_t env[3];
static bgfx_frame_buffer_handle_t shadow, hdr;
static bgfx_vertex_layout_t vertex_layout, skin_layout, fx_layout, text_layout;
static bgfx_swap_chain_t swap;
static unsigned frames;
#ifdef KALAM_NATIVE_EDITOR
static float ui_exposure;
#endif
#ifdef KALAM_EMBEDDED
static float editor_exposure, drawn_exposure;
static float drawn_view[16];
static float drawn_light_x;
float kalam_embed_drawn_light_x(void) {
    return drawn_light_x;
}
void kalam_embed_exposure(float exposure) {
    editor_exposure = exposure;
}
float kalam_embed_drawn_exposure(void) {
    return drawn_exposure;
}
void kalam_embed_drawn_view(float view[16]) {
    memcpy(view, drawn_view, sizeof(drawn_view));
}
#endif
static bool open_device;
enum { SLOTS = 512, POSE_COLUMNS = 6, POSE_ROWS = 256, POSE_STAGE = 6 };
typedef struct {
    bgfx_vertex_buffer_handle_t vb;
    bgfx_dynamic_vertex_buffer_handle_t dynamic_vb;
    bgfx_index_buffer_handle_t ib;
    uint32_t vertices;
    uint32_t indices;
    uint16_t joints;
} mesh_t;
static mesh_t meshes[SLOTS];
static bgfx_texture_handle_t textures[SLOTS];
static uint16_t pose_counts[SLOTS];
static bool pose_uploaded[SLOTS];
static uint32_t mesh_gen[SLOTS], tex_gen[SLOTS];
static uint32_t quad, overlay, icon;
static const uint16_t quad_indices[] = {0, 1, 2, 2, 3, 0};

int kh_live_resources(void) {
    int count = 0;
    if (open_device)
        for (int i = 0; i < SLOTS; i++)
            count += (meshes[i].indices != 0) + VALID(textures[i]);
    return count;
}

static bool matches(uint32_t handle, const uint32_t *generations) {
    return handle && generations[handle % SLOTS] == handle / SLOTS;
}
bool kh_gpu_skinning(void) {
    return open_device && VALID(programs[SKIN_SHADOW]) && VALID(programs[SKIN_SCENE]);
}

static uint32_t create_mesh(const float *vertices, uint32_t count, const uint16_t *indices, uint32_t ni,
                            bool dynamic, uint16_t joints) {
    if (!vertices || !indices || !count || !ni || count > 65536 || ni > UINT32_MAX / 2)
        return 0;
    if (joints && (!kh_gpu_skinning() || joints > POSE_ROWS || dynamic))
        return 0;
    const bgfx_vertex_layout_t *layout = joints ? &skin_layout : &vertex_layout;
    for (uint32_t i = 0; i < SLOTS; i++) {
        if (meshes[i].indices || mesh_gen[i] == UINT32_MAX / SLOTS)
            continue;
        bgfx_vertex_buffer_handle_t vb = {INVALID};
        bgfx_dynamic_vertex_buffer_handle_t dynamic_vb = {INVALID};
        if (dynamic)
            dynamic_vb = bgfx_create_dynamic_vertex_buffer_mem(bgfx_copy(vertices, layout->stride * count), layout, 0);
        else
            vb = bgfx_create_vertex_buffer(bgfx_copy(vertices, layout->stride * count), layout, 0);
        if (!VALID(vb) && !VALID(dynamic_vb))
            return 0;
        bgfx_index_buffer_handle_t ib = bgfx_create_index_buffer(bgfx_copy(indices, 2 * ni), 0);
        if (!VALID(ib)) {
            if (dynamic) bgfx_destroy_dynamic_vertex_buffer(dynamic_vb);
            else bgfx_destroy_vertex_buffer(vb);
            return 0;
        }
        meshes[i] = (mesh_t){vb, dynamic_vb, ib, count, ni, joints};
        return ++mesh_gen[i] * SLOTS + i;
    }
    return 0;
}
uint32_t kh_mesh(const float *vertices, uint32_t count, const uint16_t *indices, uint32_t ni) {
    return create_mesh(vertices, count, indices, ni, false, 0);
}
uint32_t kh_dynamic_mesh(const float *vertices, uint32_t count, const uint16_t *indices, uint32_t ni) {
    return create_mesh(vertices, count, indices, ni, true, 0);
}
uint32_t kh_skin_mesh(const float *vertices, uint32_t count, const uint16_t *indices, uint32_t ni, uint16_t joints) {
    return joints ? create_mesh(vertices, count, indices, ni, false, joints) : 0;
}
int kh_update_mesh(uint32_t id, const float *vertices, uint32_t count) {
    const mesh_t *m = &meshes[id % SLOTS];
    if (!vertices || !m->indices || count != m->vertices || !matches(id, mesh_gen) || !VALID(m->dynamic_vb))
        return 0;
    bgfx_update_dynamic_vertex_buffer(m->dynamic_vb, 0, bgfx_copy(vertices, 32 * count));
    return 1;
}
void kh_drop_mesh(uint32_t id) {
    mesh_t *m = &meshes[id % SLOTS];
    if (!m->indices || !matches(id, mesh_gen))
        return;
    if (VALID(m->dynamic_vb)) bgfx_destroy_dynamic_vertex_buffer(m->dynamic_vb);
    else bgfx_destroy_vertex_buffer(m->vb);
    bgfx_destroy_index_buffer(m->ib);
    m->indices = 0;
}
uint32_t kh_texture(const unsigned char *rgba, uint32_t w, uint32_t h, int srgb) {
    if (!rgba || !w || !h || w > UINT16_MAX || h > UINT16_MAX || w > (UINT32_MAX / 4) / h)
        return 0;
    for (uint32_t i = 0; i < SLOTS; i++) {
        if (VALID(textures[i]) || tex_gen[i] == UINT32_MAX / SLOTS)
            continue;
        textures[i] = bgfx_create_texture_2d(w, h, false, 1, BGFX_TEXTURE_FORMAT_RGBA8,
                                             srgb ? BGFX_TEXTURE_SRGB : 0, bgfx_copy(rgba, w * h * 4), 0);
        if (!VALID(textures[i]))
            return 0;
        return ++tex_gen[i] * SLOTS + i;
    }
    return 0;
}
uint32_t kh_skin_palette(uint16_t joints) {
    if (!kh_gpu_skinning() || !joints || joints > POSE_ROWS)
        return 0;
    for (uint32_t i = 0; i < SLOTS; ++i) {
        if (VALID(textures[i]) || tex_gen[i] == UINT32_MAX / SLOTS)
            continue;
        textures[i] = bgfx_create_texture_2d(POSE_COLUMNS, POSE_ROWS, false, 1,
            BGFX_TEXTURE_FORMAT_RGBA32F, 0, NULL, 0);
        if (!VALID(textures[i])) return 0;
        pose_counts[i] = joints;
        pose_uploaded[i] = false;
        return ++tex_gen[i] * SLOTS + i;
    }
    return 0;
}
int kh_upload_palette(uint32_t id, const float *values, uint16_t joints) {
    const uint32_t slot = id % SLOTS;
    if (!values || !joints || !matches(id, tex_gen) ||
        !VALID(textures[slot]) || pose_counts[slot] != joints)
        return 0;
    bgfx_update_texture_2d(textures[slot], 0, 0, 0, 0, POSE_COLUMNS, joints,
        bgfx_copy(values, POSE_COLUMNS * joints * 4 * sizeof(float)), UINT16_MAX);
    pose_uploaded[slot] = true;
    return 1;
}
void kh_drop_texture(uint32_t id) {
    bgfx_texture_handle_t *t = &textures[id % SLOTS];
    if (!matches(id, tex_gen) || !VALID(*t))
        return;
    bgfx_destroy_texture(*t);
    t->idx = INVALID;
    pose_counts[id % SLOTS] = 0;
    pose_uploaded[id % SLOTS] = false;
}

static bgfx_shader_handle_t shader(const char *pass, const char *stage) {
    char path[128];
    bgfx_renderer_type_t renderer = bgfx_get_renderer_type();
    const char *backend = renderer == BGFX_RENDERER_TYPE_OPENGLES ? "essl"
                          : renderer == BGFX_RENDERER_TYPE_VULKAN ? "spirv" : "dx11";
    SDL_snprintf(path, sizeof(path), "shaders/%s/%s.%s.bin", backend, pass, stage);
    size_t size = 0;
    void *data = kh_load_asset(path, &size);
    bgfx_shader_handle_t s = {INVALID};
    if (data && size && size <= UINT32_MAX)
        s = bgfx_create_shader(bgfx_copy(data, (uint32_t)size));
    SDL_free(data);
    if (!VALID(s))
        SDL_Log("Cannot create shader %s", path);
    return s;
}
static bool load_program(int pass) {
    bgfx_shader_handle_t vs = shader(pass_names[pass], "vs"), fs = shader(pass_names[pass], "fs");
    if (VALID(vs) && VALID(fs))
        programs[pass] = bgfx_create_program(vs, fs, false);
    if (VALID(vs))
        bgfx_destroy_shader(vs);
    if (VALID(fs))
        bgfx_destroy_shader(fs);
    return VALID(programs[pass]);
}
static bool load_environment(int index, const char *name, int width, int levels, bool cube) {
    size_t size = 0, expected = 0;
    void *data = kh_load_asset(name, &size);
    for (int mip = 0; mip < levels; mip++)
        expected += (width >> mip) * (width >> mip) * 4 * (cube ? 6 : 1);
    if (data && size == expected) {
        const bgfx_memory_t *mem = bgfx_copy(data, (uint32_t)size);
        env[index] = cube ? bgfx_create_texture_cube(width, levels > 1, 1, BGFX_TEXTURE_FORMAT_RGBA8,
                                                     BGFX_SAMPLER_UVW_CLAMP, mem, 0)
                          : bgfx_create_texture_2d(width, width, false, 1, BGFX_TEXTURE_FORMAT_RGBA8,
                                                   BGFX_SAMPLER_UVW_CLAMP, mem, 0);
    }
    SDL_free(data);
    if (!VALID(env[index]))
        SDL_Log("Invalid environment fixture %s", name);
    return VALID(env[index]);
}
static bgfx_frame_buffer_handle_t target(uint16_t w, uint16_t h, bgfx_texture_format_t fmt) {
    bgfx_frame_buffer_handle_t fb = {INVALID};
    const uint64_t flags = BGFX_TEXTURE_RT | BGFX_SAMPLER_UVW_CLAMP;
    bgfx_texture_handle_t attachments[2] = {
        bgfx_create_texture_2d(w, h, false, 1, fmt, flags, NULL, 0),
        bgfx_create_texture_2d(w, h, false, 1, BGFX_TEXTURE_FORMAT_D16, BGFX_TEXTURE_RT_WRITE_ONLY, NULL, 0)};
    if (VALID(attachments[0]) && VALID(attachments[1]))
        fb = bgfx_create_frame_buffer_from_handles(2, attachments, true);
    if (!VALID(fb))
        for (int i = 0; i < 2; i++)
            if (VALID(attachments[i]))
                bgfx_destroy_texture(attachments[i]);
    return fb;
}
static uint32_t rectangle(float left, float top, float right, float bottom) {
    const float v[] = {left,  top,    0, 0, 0, 1, 0, 0, right, top,    0, 0, 0, 1, 1, 0,
                       right, bottom, 0, 0, 0, 1, 1, 1, left,  bottom, 0, 0, 0, 1, 0, 1};
    return kh_mesh(v, 4, quad_indices, 6);
}
int renderer_open(void *native, uint32_t w, uint32_t h) {
    if (!w || !h || w > UINT16_MAX || h > UINT16_MAX)
        return 0;
    bgfx_init_t init;
    bgfx_init_ctor(&init);
    init.type = requested_backend;
    init.fallback = false;
    init.swapChain.nwh = native;
    init.swapChain.width = w;
    init.swapChain.height = h;
    init.swapChain.formatDepthStencil = BGFX_TEXTURE_FORMAT_COUNT;
    init.reset = BGFX_RESET_VSYNC;
    swap = init.swapChain;
    if (!bgfx_init(&init)) {
        SDL_Log("bgfx initialization failed for %s", bgfx_get_renderer_name(init.type));
        return 0;
    }
    open_device = true;
    frames = 0;
    SDL_Log("backend=%s vendor=%04x device=%04x pixels=%ux%u", bgfx_get_renderer_name(init.type),
            bgfx_get_caps()->vendorId, bgfx_get_caps()->deviceId, w, h);
    memset(textures, 0xff, sizeof(textures));
    memset(env, 0xff, sizeof(env));
    memset(uniforms, 0xff, sizeof(uniforms));
    memset(samplers, 0xff, sizeof(samplers));
    memset(programs, 0xff, sizeof(programs));
    hdr.idx = shadow.idx = INVALID;
    if (!bgfx_is_texture_valid(0, false, 1, BGFX_TEXTURE_FORMAT_RGBA16F, BGFX_TEXTURE_RT) ||
        !bgfx_is_texture_valid(0, false, 1, BGFX_TEXTURE_FORMAT_D16, BGFX_TEXTURE_RT_WRITE_ONLY)) {
        SDL_Log("Required RGBA16F render target or D16 depth buffer unavailable");
        goto fail;
    }
    bgfx_vertex_layout_begin(&vertex_layout, init.type);
    bgfx_vertex_layout_add(&vertex_layout, BGFX_ATTRIB_POSITION, 3, BGFX_ATTRIB_TYPE_FLOAT, false, false);
    bgfx_vertex_layout_add(&vertex_layout, BGFX_ATTRIB_NORMAL, 3, BGFX_ATTRIB_TYPE_FLOAT, false, false);
    bgfx_vertex_layout_add(&vertex_layout, BGFX_ATTRIB_TEXCOORD0, 2, BGFX_ATTRIB_TYPE_FLOAT, false, false);
    bgfx_vertex_layout_end(&vertex_layout);
    bgfx_vertex_layout_begin(&fx_layout, init.type);
    bgfx_vertex_layout_add(&fx_layout, BGFX_ATTRIB_POSITION, 3, BGFX_ATTRIB_TYPE_FLOAT, false, false);
    bgfx_vertex_layout_add(&fx_layout, BGFX_ATTRIB_COLOR0, 4, BGFX_ATTRIB_TYPE_FLOAT, false, false);
    bgfx_vertex_layout_add(&fx_layout, BGFX_ATTRIB_TEXCOORD0, 2, BGFX_ATTRIB_TYPE_FLOAT, false, false);
    bgfx_vertex_layout_add(&fx_layout, BGFX_ATTRIB_TEXCOORD1, 4, BGFX_ATTRIB_TYPE_FLOAT, false, false);
    bgfx_vertex_layout_end(&fx_layout);
    bgfx_vertex_layout_begin(&text_layout, init.type);
    bgfx_vertex_layout_add(&text_layout, BGFX_ATTRIB_POSITION, 2, BGFX_ATTRIB_TYPE_FLOAT, false, false);
    bgfx_vertex_layout_add(&text_layout, BGFX_ATTRIB_COLOR0, 4, BGFX_ATTRIB_TYPE_UINT8, true, false);
    bgfx_vertex_layout_add(&text_layout, BGFX_ATTRIB_TEXCOORD0, 2, BGFX_ATTRIB_TYPE_FLOAT, false, false);
    bgfx_vertex_layout_end(&text_layout);
    bgfx_vertex_layout_begin(&skin_layout, init.type);
    bgfx_vertex_layout_add(&skin_layout, BGFX_ATTRIB_POSITION, 3, BGFX_ATTRIB_TYPE_FLOAT, false, false);
    bgfx_vertex_layout_add(&skin_layout, BGFX_ATTRIB_NORMAL, 3, BGFX_ATTRIB_TYPE_FLOAT, false, false);
    bgfx_vertex_layout_add(&skin_layout, BGFX_ATTRIB_TEXCOORD0, 2, BGFX_ATTRIB_TYPE_FLOAT, false, false);
    bgfx_vertex_layout_add(&skin_layout, BGFX_ATTRIB_TEXCOORD1, 4, BGFX_ATTRIB_TYPE_FLOAT, false, false);
    bgfx_vertex_layout_add(&skin_layout, BGFX_ATTRIB_TEXCOORD2, 4, BGFX_ATTRIB_TYPE_FLOAT, false, false);
    bgfx_vertex_layout_end(&skin_layout);
    const bgfx_caps_t *caps = bgfx_get_caps();
    const uint32_t required = BGFX_CAPS_FORMAT_TEXTURE_2D | BGFX_CAPS_FORMAT_TEXTURE_VERTEX;
    const bool skin_supported = caps->limits.maxTextureSamplers > POSE_STAGE &&
        (caps->formats[BGFX_TEXTURE_FORMAT_RGBA32F] & required) == required;
    for (int i = 0; i < PASS_COUNT; i++) {
        if ((i == SKIN_SHADOW || i == SKIN_SCENE) && !skin_supported) continue;
        if (!load_program(i))
            goto fail;
    }
    for (int i = 0; i < UNIFORM_COUNT; i++) {
        uniforms[i] = bgfx_create_uniform(uniform_names[i],
                                          i == LIGHT_VP ? BGFX_UNIFORM_TYPE_MAT4 : BGFX_UNIFORM_TYPE_VEC4, 1);
        if (!VALID(uniforms[i]))
            goto fail;
    }
    for (int i = 0; i < SAMPLER_COUNT; i++) {
        samplers[i] = bgfx_create_uniform(sampler_names[i], BGFX_UNIFORM_TYPE_SAMPLER, 1);
        if (!VALID(samplers[i]))
            goto fail;
    }
    if (!load_environment(0, "environment/irradiance.rgba8", 4, 1, true) ||
        !load_environment(1, "environment/prefilter.rgba8", 16, 5, true) ||
        !load_environment(2, "environment/dfg.rgba8", 32, 1, false))
        goto fail;
    quad = rectangle(-1, 1, 1, -1);
    overlay = rectangle(-.96f, .94f, -.76f, .74f);
    const unsigned char rgba[] = {240, 50, 20, 220, 30, 240, 90, 200, 30, 60, 190, 210, 242, 170, 30, 0};
    icon = kh_texture(rgba, 2, 2, 0);
    shadow = target(1024, 1024, BGFX_TEXTURE_FORMAT_RGBA16F);
    hdr = target(w, h, BGFX_TEXTURE_FORMAT_RGBA16F);
    if (!quad || !overlay || !icon || !VALID(shadow) || !VALID(hdr))
        goto fail;
#ifdef KALAM_NATIVE_EDITOR
    if (!kh_ui_device_open(programs[EDITOR_UI], samplers[SCREEN]))
        goto fail;
#endif
    return 1;
fail:
    renderer_close();
    return 0;
}
void renderer_close(void) {
    if (!open_device)
        return;
#ifdef KALAM_NATIVE_EDITOR
    kh_ui_device_close();
#endif
    for (int i = 0; i < SLOTS; i++) {
        if (meshes[i].indices)
            kh_drop_mesh(mesh_gen[i] * SLOTS + i);
        if (VALID(textures[i]))
            kh_drop_texture(tex_gen[i] * SLOTS + i);
    }
    for (int i = 0; i < 3; i++)
        if (VALID(env[i]))
            bgfx_destroy_texture(env[i]);
    for (int i = 0; i < PASS_COUNT; i++)
        if (VALID(programs[i]))
            bgfx_destroy_program(programs[i]);
    for (int i = 0; i < UNIFORM_COUNT; i++)
        if (VALID(uniforms[i]))
            bgfx_destroy_uniform(uniforms[i]);
    for (int i = 0; i < SAMPLER_COUNT; i++)
        if (VALID(samplers[i]))
            bgfx_destroy_uniform(samplers[i]);
    if (VALID(hdr))
        bgfx_destroy_frame_buffer(hdr);
    if (VALID(shadow))
        bgfx_destroy_frame_buffer(shadow);
    bgfx_frame(0);
    bgfx_frame(0);
    bgfx_shutdown();
    open_device = false;
}
int renderer_resize(uint32_t w, uint32_t h) {
    if (!w || !h || w > UINT16_MAX || h > UINT16_MAX)
        return 0;
    bgfx_frame_buffer_handle_t replacement = target((uint16_t)w, (uint16_t)h, BGFX_TEXTURE_FORMAT_RGBA16F);
    if (!VALID(replacement))
        return 0;
    swap.width = w;
    swap.height = h;
    bgfx_reset(BGFX_RESET_VSYNC, &swap);
    bgfx_destroy_frame_buffer(hdr);
    hdr = replacement;
    return 1;
}

void kh_begin_geometry(uint16_t order, int shadow_pass, const float *view, const float *proj,
                       uint32_t clear) {
#ifdef KALAM_EMBEDDED
    if (!shadow_pass)
        memcpy(drawn_view, view, sizeof(drawn_view));
#endif
    bgfx_set_view_transform(order, view, proj);
    bgfx_set_view_frame_buffer(order, shadow_pass ? shadow : hdr);
    bgfx_set_view_clear(order, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, clear, 1, 0);
    bgfx_set_view_rect(order, 0, 0, shadow_pass ? 1024 : swap.width, shadow_pass ? 1024 : swap.height, 0, 1);
    bgfx_touch(order);
}
static void bind_mesh(uint32_t id) {
    mesh_t *m = &meshes[id % SLOTS];
    if (VALID(m->dynamic_vb)) bgfx_set_dynamic_vertex_buffer(0, m->dynamic_vb, 0, m->vertices);
    else bgfx_set_vertex_buffer(0, m->vb, 0, m->vertices);
    bgfx_set_index_buffer(m->ib, 0, m->indices);
}
static bool bind_pose(uint32_t mesh, uint32_t palette) {
    const uint16_t joints = meshes[mesh % SLOTS].joints;
    const uint32_t slot = palette % SLOTS;
    if (!joints) return !palette;
    if (!matches(palette, tex_gen) || !VALID(textures[slot]) ||
        pose_counts[slot] != joints || !pose_uploaded[slot]) return false;
    bgfx_set_texture(POSE_STAGE, samplers[SKIN_PALETTE], textures[slot],
        BGFX_SAMPLER_UVW_CLAMP | BGFX_SAMPLER_POINT);
    return true;
}
int kh_draw_shadow(uint16_t order, uint32_t mesh, const float *model, uint32_t palette) {
    if (!matches(mesh, mesh_gen) || !meshes[mesh % SLOTS].indices)
        return 0;
    if (!bind_pose(mesh, palette)) return 0;
    bind_mesh(mesh);
    bgfx_set_transform(model, 1);
    bgfx_set_state(BGFX_STATE_WRITE_MASK | BGFX_STATE_DEPTH_TEST_LESS | BGFX_STATE_MSAA, 0);
    bgfx_submit(order, programs[palette ? SKIN_SHADOW : SHADOW], 0, BGFX_DISCARD_ALL);
    return 1;
}
int kh_draw_lit(uint16_t order, uint32_t mesh, const float *model, const float *color, const float *material,
                const float *light_vp, const float *flags, const float *light_dir, const float *light_color,
                const float *eye, uint32_t base_tex, uint32_t mr_tex, uint32_t palette, uint32_t normal_tex) {
    if (!matches(mesh, mesh_gen) || !meshes[mesh % SLOTS].indices || !matches(base_tex, tex_gen) ||
        !VALID(textures[base_tex % SLOTS]) || !matches(mr_tex, tex_gen) || !VALID(textures[mr_tex % SLOTS]))
        return 0;
    if (normal_tex && (!matches(normal_tex, tex_gen) || !VALID(textures[normal_tex % SLOTS])))
        return 0;
    if (!bind_pose(mesh, palette)) return 0;
#ifdef KALAM_EMBEDDED
    drawn_light_x = light_dir[0];
#endif
    const uint64_t state = BGFX_STATE_WRITE_MASK | BGFX_STATE_DEPTH_TEST_LESS | BGFX_STATE_MSAA;
    bind_mesh(mesh);
    bgfx_set_transform(model, 1);
    float camera_flags[] = {bgfx_get_caps()->originBottomLeft ? 0.5f : flags[0], flags[1], flags[2], normal_tex ? 1.f : 0.f};
    const float *values[] = {color, material, light_dir, light_color, eye, light_vp, camera_flags};
    for (int i = BASE; i <= FLAGS; i++)
        bgfx_set_uniform(uniforms[i], values[i], 1);
    bgfx_set_texture(0, samplers[BASE_TEX], textures[base_tex % SLOTS], UINT32_MAX);
    bgfx_set_texture(1, samplers[MATERIAL_TEX], textures[mr_tex % SLOTS], UINT32_MAX);
    bgfx_set_texture(2, samplers[SHADOW_TEX], bgfx_get_texture(shadow, 0),
                     BGFX_SAMPLER_UVW_CLAMP | BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT);
    for (int i = 0; i < 3; i++)
        bgfx_set_texture(3 + i, samplers[IRRADIANCE + i], env[i], UINT32_MAX);
    bgfx_set_texture(7, samplers[NORMAL_TEX], textures[(normal_tex ? normal_tex : mr_tex) % SLOTS], UINT32_MAX);
    bgfx_set_state(state, 0);
    bgfx_submit(order, programs[palette ? SKIN_SCENE : SCENE], 0, BGFX_DISCARD_ALL);
    return 1;
}
void kh_begin_fx(uint16_t order, const float *view, const float *proj) {
    bgfx_set_view_mode(order, BGFX_VIEW_MODE_SEQUENTIAL);
    bgfx_set_view_transform(order, view, proj);
    bgfx_set_view_frame_buffer(order, hdr);
    bgfx_set_view_rect(order, 0, 0, swap.width, swap.height, 0, 1);
    bgfx_set_view_clear(order, BGFX_CLEAR_NONE, 0, 1, 0);
}
static bool fx_texture_valid(uint32_t id) {
    return matches(id, tex_gen) && VALID(textures[id % SLOTS]);
}
static void fx_state(uint32_t flags) {
    // The opaque pass stores signed depth/selection in HDR alpha for the outline.
    uint64_t state = BGFX_STATE_WRITE_RGB | BGFX_STATE_MSAA;
    if (!(flags & 2)) state |= BGFX_STATE_DEPTH_TEST_LEQUAL;
    state |= (flags & 1) ? BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_ONE, BGFX_STATE_BLEND_ONE)
                         : BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_ONE, BGFX_STATE_BLEND_INV_SRC_ALPHA);
    bgfx_set_state(state, 0);
}
// The Rust caller fills four tightly packed position/color/UV/mask vertices per quad.
// A full transient pool can shed decorative quads without interrupting gameplay.
int kh_fx_quads(uint16_t order, const uint32_t maps[3], uint32_t flags, const float opacity[4],
                const float *data, uint32_t count) {
    if (!data || count > UINT16_MAX / 4) return -1;
    for (int i = 0; i < 3; i++) if (!fx_texture_valid(maps[i])) return -1;
    uint32_t available_v = bgfx_get_avail_transient_vertex_buffer(count * 4, &fx_layout) / 4;
    uint32_t available_i = bgfx_get_avail_transient_index_buffer(count * 6, false) / 6;
    if (count > available_v) count = available_v;
    if (count > available_i) count = available_i;
    if (!count) return 0;
    bgfx_transient_vertex_buffer_t vertices;
    bgfx_transient_index_buffer_t indices;
    if (!bgfx_alloc_transient_buffers(&vertices, &fx_layout, count * 4, &indices, count * 6, false))
        return 0;
    memcpy(vertices.data, data, count * 4 * fx_layout.stride);
    uint16_t *out = (uint16_t *)indices.data;
    for (uint32_t i = 0; i < count; ++i)
        for (size_t j = 0; j < 6; ++j)
            *out++ = (uint16_t)(4 * i + quad_indices[j]);
    bgfx_set_transient_vertex_buffer(0, &vertices, 0, count * 4);
    bgfx_set_transient_index_buffer(&indices, 0, count * 6);
    const int slots[] = {SCREEN, FX_RAMP_TEX, FX_DISSOLVE_TEX};
    for (int i = 0; i < 3; i++)
        bgfx_set_texture((uint8_t)i, samplers[slots[i]], textures[maps[i] % SLOTS], BGFX_SAMPLER_UVW_CLAMP);
    bgfx_set_uniform(uniforms[FX_OPACITY], opacity, 1);
    fx_state(flags);
    bgfx_submit(order, programs[FX_QUADS], 0, BGFX_DISCARD_ALL);
    return (int)count;
}
int kh_fx_mesh(uint16_t order, uint32_t mesh, uint32_t flags, const float *model,
               const uint32_t maps[4], const float params[8][4]) {
    if (!matches(mesh, mesh_gen) || !meshes[mesh % SLOTS].indices || meshes[mesh % SLOTS].joints)
        return 0;
    const int slots[] = {SCREEN, FX_OPACITY_TEX, FX_RAMP_TEX, FX_DISSOLVE_TEX};
    for (int i = 0; i < 4; i++) if (!fx_texture_valid(maps[i])) return 0;
    for (int i = 0; i < 4; i++)
        bgfx_set_texture((uint8_t)i, samplers[slots[i]], textures[maps[i] % SLOTS],
                         i == 2 ? BGFX_SAMPLER_UVW_CLAMP : 0); // mesh scrolling repeats
    for (int i = 0; i < 8; i++) bgfx_set_uniform(uniforms[TINT + i], params[i], 1);
    bind_mesh(mesh);
    bgfx_set_transform(model, 1);
    fx_state(flags);
    bgfx_submit(order, programs[FX_MESH], 0, BGFX_DISCARD_ALL);
    return 1;
}
void kh_tonemap(uint16_t order, float multiplier, int show_outline) {
#ifdef KALAM_NATIVE_EDITOR
    ui_exposure = multiplier;
#endif
#ifdef KALAM_EMBEDDED
    if (editor_exposure > 0.f)
        multiplier = editor_exposure;
    drawn_exposure = multiplier;
#endif
    float show_ramp = 0.f;
#ifndef KALAM_GAME_APPLICATION
    show_ramp = capture_prefix ? 1.f : 0.f;
#endif
    const float exposure[] = {multiplier, show_ramp, (float)swap.width, (float)swap.height};
    const float outline_color[] = {1.f, .82f, .12f, show_outline ? 1.f : 0.f};
    bgfx_set_view_rect(order, 0, 0, swap.width, swap.height, 0, 1);
    bind_mesh(quad);
    bgfx_set_texture(0, samplers[SCREEN], bgfx_get_texture(hdr, 0),
                     BGFX_SAMPLER_UVW_CLAMP | BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT);
    bgfx_set_uniform(uniforms[EXPOSURE], exposure, 1);
    bgfx_set_uniform(uniforms[OUTLINE_COLOR], outline_color, 1);
    bgfx_set_state(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A, 0);
    bgfx_submit(order, programs[TONE], 0, BGFX_DISCARD_ALL);
}
void kh_overlay(uint16_t order, const float *tint) {
    bgfx_set_view_rect(order, 0, 0, swap.width, swap.height, 0, 1);
    bind_mesh(overlay);
    bgfx_set_texture(0, samplers[SCREEN], textures[icon % SLOTS],
                     BGFX_SAMPLER_UVW_CLAMP | BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT);
    bgfx_set_uniform(uniforms[TINT], tint, 1);
    bgfx_set_state(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_BLEND_ALPHA, 0);
    bgfx_submit(order, programs[SPRITE], 0, BGFX_DISCARD_ALL);
}
static int panel_draw(uint16_t order, uint32_t texture, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                      uint64_t blend) {
    if (!matches(texture, tex_gen) || !VALID(textures[texture % SLOTS]))
        return 0;
    const float tint[] = {1.f, 1.f, 1.f, 1.f};
    bgfx_set_view_rect(order, x, y, w, h, 0, 1);
    bind_mesh(quad);
    bgfx_set_texture(0, samplers[SCREEN], textures[texture % SLOTS],
                     BGFX_SAMPLER_UVW_CLAMP | BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT);
    bgfx_set_uniform(uniforms[TINT], tint, 1);
    bgfx_set_state(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | blend, 0);
    bgfx_submit(order, programs[SPRITE], 0, BGFX_DISCARD_ALL);
    return 1;
}

int kh_lens_draw(uint16_t order, uint32_t texture, uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
    return panel_draw(order, texture, x, y, w, h, 0);
}

int kh_game_overlay(uint16_t order, uint32_t texture, uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
    return panel_draw(order, texture, x, y, w, h, BGFX_STATE_BLEND_ALPHA);
}

int kh_msdf_text(uint32_t atlas, const kh_msdf_glyph *glyphs, uint32_t count,
                 float unit_range_x, float unit_range_y,
                 uint16_t clip_x, uint16_t clip_y, uint16_t clip_w, uint16_t clip_h,
                 float scale_x, float scale_y) {
    enum { TEXT_VIEW = 7 }; // Post-tonemap, above both game panels and below editor chrome.
    typedef struct { float x, y; uint32_t rgba; float u, v; } text_vertex;
    if (!count || !clip_w || !clip_h || clip_x >= swap.width || clip_y >= swap.height) return 1;
    if (!open_device || !glyphs || !matches(atlas, tex_gen) || !VALID(textures[atlas % SLOTS]) ||
        !(unit_range_x > 0.f && unit_range_y > 0.f && scale_x > 0.f && scale_y > 0.f) ||
        count > 16384) return 0;
    if (clip_w > swap.width - clip_x) clip_w = (uint16_t)(swap.width - clip_x);
    if (clip_h > swap.height - clip_y) clip_h = (uint16_t)(swap.height - clip_y);

    bgfx_transient_vertex_buffer_t vertices;
    bgfx_transient_index_buffer_t indices;
    // FX and text share the per-frame transient pools. Skip this panel's text
    // on exhaustion and let its cached layout be submitted again next frame.
    if (!bgfx_alloc_transient_buffers(&vertices, &text_layout, count * 4, &indices, count * 6, false))
        return 1;
    text_vertex *out = (text_vertex *)vertices.data;
    uint16_t *index = (uint16_t *)indices.data;
    const float left_origin = 2.f * clip_x / swap.width - 1.f;
    const float top_origin = 1.f - 2.f * clip_y / swap.height;
    const float sx = 2.f * scale_x / swap.width, sy = -2.f * scale_y / swap.height;
    for (uint32_t i = 0; i < count; ++i) {
        const kh_msdf_glyph *g = &glyphs[i];
        const float left = left_origin + g->rect[0] * sx, right = left_origin + g->rect[2] * sx;
        const float top = top_origin + g->rect[1] * sy, bottom = top_origin + g->rect[3] * sy;
        uint32_t rgba;
        memcpy(&rgba, g->color, sizeof(rgba));
        *out++ = (text_vertex){left,  top,    rgba, g->uv[0], g->uv[1]};
        *out++ = (text_vertex){right, top,    rgba, g->uv[2], g->uv[1]};
        *out++ = (text_vertex){right, bottom, rgba, g->uv[2], g->uv[3]};
        *out++ = (text_vertex){left,  bottom, rgba, g->uv[0], g->uv[3]};
        for (unsigned j = 0; j < 6; ++j) *index++ = (uint16_t)(4 * i + quad_indices[j]);
    }
    const float range[] = {unit_range_x, unit_range_y, 0.f, 0.f};
    bgfx_set_view_rect(TEXT_VIEW, 0, 0, (uint16_t)swap.width, (uint16_t)swap.height, 0, 1);
    bgfx_set_view_mode(TEXT_VIEW, BGFX_VIEW_MODE_SEQUENTIAL);
    bgfx_set_scissor(clip_x, clip_y, clip_w, clip_h);
    bgfx_set_texture(0, samplers[SCREEN], textures[atlas % SLOTS], BGFX_SAMPLER_UVW_CLAMP);
    bgfx_set_uniform(uniforms[MSDF_RANGE], range, 1);
    bgfx_set_transient_vertex_buffer(0, &vertices, 0, count * 4);
    bgfx_set_transient_index_buffer(&indices, 0, count * 6);
    bgfx_set_state(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_BLEND_ALPHA, 0);
    bgfx_submit(TEXT_VIEW, programs[MSDF_TEXT], 0, BGFX_DISCARD_ALL);
    return 1;
}

int kh_present(void) {
#ifdef KALAM_NATIVE_EDITOR
    const int ui_status = kh_ui_draw(ui_exposure);
    if (ui_status <= 0) {
        if (ui_status == 0) bgfx_frame(0);
        return ui_status;
    }
#endif
    if (++frames == 3 && capture_prefix)
        bgfx_request_screen_shot((bgfx_frame_buffer_handle_t){INVALID}, capture_prefix);
    bgfx_frame(0);
    if (frames == 3) {
        const bgfx_stats_t *stats = bgfx_get_stats();
        if (stats->cpuTimerFreq > 0)
            SDL_Log("frame=%u cpu-frame-ms=%.3f draws=%u", frames,
                    1000.0 * (double)stats->cpuTimeFrame / stats->cpuTimerFreq, stats->numDraw);
    }
    return 1;
}

#ifdef KALAM_EMBEDDED
unsigned kalam_embed_frames(void) {
    return frames;
}
unsigned kalam_embed_draw_count(void) {
    return bgfx_get_stats()->numDraw;
}
#endif
