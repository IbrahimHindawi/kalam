#include "editor.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "backends/imgui_impl_sdl3.h"

#include <algorithm>
#include <cstring>
#include <string>

namespace {
constexpr uint16_t UiView = 8;
constexpr uint16_t Invalid = UINT16_MAX;
struct Platform {
    bool smoke = false;
    Uint64 started = 0;
    std::string layoutPath, codeFontPath;
    ImFont *codeFont = nullptr;
    ImFont *gameFont = nullptr;
    bgfx_texture_handle_t panel{Invalid}, button{Invalid};
    ImGuiID dockspace = 0;
    bgfx_program_handle_t program{Invalid};
    bgfx_uniform_handle_t sampler{Invalid};
    bgfx_texture_handle_t font{Invalid};
    bgfx_vertex_layout_t vertexLayout{};
};
Platform platform;

std::string gameUiPath(const char *name) {
    const char *root = SDL_getenv("KALAM_CONTENT_ROOT");
    if (!root) root = SDL_getenv("KALAM_ASSET_ROOT");
    const std::string base = root ? std::string(root) + "/" : std::string(SDL_GetBasePath()) + "assets/";
    return base + "ui/" + name;
}

bgfx_texture_handle_t loadPanel(const char *name) {
    SDL_Surface *source = SDL_LoadBMP(gameUiPath(name).c_str());
    if (!source) return {Invalid};
    SDL_Surface *rgba = SDL_ConvertSurface(source, SDL_PIXELFORMAT_RGBA32);
    SDL_DestroySurface(source);
    if (!rgba) return {Invalid};
    bgfx_texture_handle_t texture{Invalid};
    if (rgba->w > 0 && rgba->h > 0 && rgba->w <= 2048 && rgba->h <= 2048 &&
        rgba->pitch == rgba->w * 4) {
        texture = bgfx_create_texture_2d(uint16_t(rgba->w), uint16_t(rgba->h), false, 1,
            BGFX_TEXTURE_FORMAT_RGBA8, BGFX_SAMPLER_UVW_CLAMP,
            bgfx_copy(rgba->pixels, uint32_t(rgba->pitch * rgba->h)), 0);
    }
    SDL_DestroySurface(rgba);
    return texture;
}

// Sange's authored 1024px backgrounds use 192px corners. Preserve them as
// the equipment window changes size and stretch only the strips and center.
void drawPanel(bgfx_texture_handle_t texture, ImVec2 pos, ImVec2 size, float border,
               ImU32 tint = IM_COL32_WHITE) {
    if (texture.idx == Invalid || size.x <= 0 || size.y <= 0) return;
    const float bx = std::min(border, size.x * .5f), by = std::min(border, size.y * .5f);
    const float x[] = {pos.x, pos.x+bx, pos.x+size.x-bx, pos.x+size.x};
    const float y[] = {pos.y, pos.y+by, pos.y+size.y-by, pos.y+size.y};
    constexpr float uv[] = {0.f, 192.f/1024.f, 1.f-192.f/1024.f, 1.f};
    auto *draw = ImGui::GetWindowDrawList();
    for (int row = 0; row < 3; ++row)
        for (int col = 0; col < 3; ++col)
            draw->AddImage(texture.idx, {x[col],y[row]}, {x[col+1],y[row+1]},
                           {uv[col],uv[row]}, {uv[col+1],uv[row+1]}, tint);
}

std::string installedNerdFont(const std::string &directory) {
    if (directory.empty()) return {};
    std::string best;
    SDL_EnumerateDirectory(directory.c_str(), [](void *context, const char *, const char *filename) {
        std::string name(filename);
        std::string lower;
        lower.reserve(name.size());
        for (unsigned char c : name) {
            if (c != ' ' && c != '-' && c != '_')
                lower.push_back(static_cast<char>(SDL_tolower(c)));
        }
        if (lower.find("nerdfontmono") == std::string::npos ||
            (!lower.ends_with("regular.ttf") && !lower.ends_with("regular.otf")))
            return SDL_ENUM_CONTINUE;
        auto &best = *static_cast<std::string *>(context);
        if (best.empty() || name < best) best = name;
        return SDL_ENUM_CONTINUE;
    }, &best);
    return best.empty() ? std::string() : directory + "/" + best;
}

bool submitUi(const ImDrawData &data) {
    const int width = static_cast<int>(data.DisplaySize.x * data.FramebufferScale.x);
    const int height = static_cast<int>(data.DisplaySize.y * data.FramebufferScale.y);
    if (width <= 0 || height <= 0 || width > UINT16_MAX || height > UINT16_MAX) return true;
    auto &ui = platform;
    bgfx_set_view_mode(UiView, BGFX_VIEW_MODE_SEQUENTIAL);
    bgfx_set_view_rect(UiView, 0, 0, static_cast<uint16_t>(width), static_cast<uint16_t>(height), 0, 1);
    unsigned batches = 0;
    for (int listIndex = 0; listIndex < data.CmdListsCount; ++listIndex) {
        const ImDrawList &list = *data.CmdLists[listIndex];
        if (list.VtxBuffer.empty() || list.IdxBuffer.empty()) continue;
        bgfx_transient_vertex_buffer_t vertices;
        bgfx_transient_index_buffer_t indices;
        if (!bgfx_alloc_transient_buffers(&vertices, &ui.vertexLayout, list.VtxBuffer.Size,
                                          &indices, list.IdxBuffer.Size, sizeof(ImDrawIdx) == 4)) {
            return !ui.smoke;
        }
        auto *destination = reinterpret_cast<ImDrawVert *>(vertices.data);
        for (int i = 0; i < list.VtxBuffer.Size; ++i) {
            destination[i] = list.VtxBuffer[i];
            destination[i].pos.x = (destination[i].pos.x - data.DisplayPos.x) * 2.f / data.DisplaySize.x - 1.f;
            destination[i].pos.y = 1.f - (destination[i].pos.y - data.DisplayPos.y) * 2.f / data.DisplaySize.y;
        }
        std::memcpy(indices.data, list.IdxBuffer.Data, list.IdxBuffer.Size * sizeof(ImDrawIdx));
        for (const ImDrawCmd &command : list.CmdBuffer) {
            if (command.UserCallback) {
                if (command.UserCallback != ImDrawCallback_ResetRenderState) command.UserCallback(&list, &command);
                continue;
            }
            const auto clip = command.ClipRect;
            const int left = std::clamp(int((clip.x - data.DisplayPos.x) * data.FramebufferScale.x), 0, width);
            const int top = std::clamp(int((clip.y - data.DisplayPos.y) * data.FramebufferScale.y), 0, height);
            const int right = std::clamp(int((clip.z - data.DisplayPos.x) * data.FramebufferScale.x), 0, width);
            const int bottom = std::clamp(int((clip.w - data.DisplayPos.y) * data.FramebufferScale.y), 0, height);
            if (left >= right || top >= bottom) continue;
            bgfx_set_scissor(left, top, right - left, bottom - top);
            bgfx_set_state(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_BLEND_ALPHA, 0);
            bgfx_set_texture(0, ui.sampler, {static_cast<uint16_t>(command.GetTexID())}, BGFX_SAMPLER_UVW_CLAMP);
            bgfx_set_transient_vertex_buffer(0, &vertices, command.VtxOffset, list.VtxBuffer.Size - command.VtxOffset);
            bgfx_set_transient_index_buffer(&indices, command.IdxOffset, command.ElemCount);
            bgfx_submit(UiView, ui.program, 0, BGFX_DISCARD_ALL);
            ++batches;
        }
    }
    return !ui.smoke || batches > 0;
}

} // namespace

extern "C" {
bool kh_rs_open(const char *scene, bool smoke);
void kh_rs_close(void);
void kh_rs_draw(void);
int kh_rs_presented(float exposure);
bool kh_rs_key(uint32_t key, uint32_t modifiers, bool repeat);
bool kh_rs_pointer(uint32_t kind, uint32_t button, float x, float y);
void kh_rs_blur(void);

bool kh_ui_open(SDL_Window *window, const char *scene, bool smoke) {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    if (!ImGui_ImplSDL3_InitForOther(window)) {
        ImGui::DestroyContext();
        return false;
    }
    platform.smoke = smoke;
    platform.started = SDL_GetTicks();
    if (!kh_rs_open(scene, smoke)) {
        kh_ui_close();
        return false;
    }
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    if (!smoke) {
        if (char *prefs = SDL_GetPrefPath("Kalam", "Editor")) {
            platform.layoutPath = std::string(prefs) + "layout.ini";
            io.IniFilename = platform.layoutPath.c_str();
            SDL_free(prefs);
        }
    }
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_DockingEnable;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
    const char *windows = SDL_getenv("WINDIR");
    const auto fonts = std::string(windows ? windows : "C:/Windows") + "/Fonts/";
    const auto loadFont = [&](const std::string &path, float pixels, const ImWchar *ranges = nullptr) -> ImFont * {
        SDL_PathInfo info{};
        return !path.empty() && SDL_GetPathInfo(path.c_str(), &info) && info.type == SDL_PATHTYPE_FILE
            ? io.Fonts->AddFontFromFileTTF(path.c_str(), pixels, nullptr, ranges)
            : nullptr;
    };
    platform.gameFont = loadFont(gameUiPath("Marcellus-Regular.ttf"), 17.f);
    static const ImWchar codeRanges[] = {
        0x0020, 0x024f, 0x0370, 0x052f, 0x2000, 0x2bff,
        0xe000, 0xf8ff, 0xf0000, 0xffffd, 0
    };
    if (const char *overridePath = SDL_getenv("KALAM_NVIM_FONT"))
        platform.codeFontPath = overridePath;
    if (!(platform.codeFont = loadFont(platform.codeFontPath, 15.f, codeRanges))) {
        platform.codeFontPath.clear();
        const char *local = SDL_getenv("LOCALAPPDATA");
        const auto userFonts = local ? std::string(local) + "/Microsoft/Windows/Fonts/" : "";
        constexpr auto iosevka = "IosevkaTermNerdFontMono-Regular.ttf";
        for (const auto &directory : {userFonts, fonts}) {
            if (directory.empty()) continue;
            const auto path = directory + iosevka;
            if (auto *font = loadFont(path, 15.f, codeRanges)) {
                platform.codeFontPath = path;
                platform.codeFont = font;
                break;
            }
        }
        if (!platform.codeFont) {
            platform.codeFontPath = installedNerdFont(userFonts);
            if (platform.codeFontPath.empty()) platform.codeFontPath = installedNerdFont(fonts);
            platform.codeFont = loadFont(platform.codeFontPath, 15.f, codeRanges);
        }
    }
    if (!platform.codeFont) {
        platform.codeFontPath = fonts + "consola.ttf";
        platform.codeFont = loadFont(platform.codeFontPath, 15.f, codeRanges);
    }
    if (!platform.codeFont) platform.codeFont = io.Fonts->AddFontDefault();
    io.FontDefault = platform.codeFont;
    if (!platform.gameFont) platform.gameFont = io.FontDefault;
    auto &style = ImGui::GetStyle();
    ImGui::StyleColorsDark();
    style.WindowPadding = ImVec2(12, 12);
    style.FramePadding = ImVec2(8, 4);
    style.ItemSpacing = ImVec2(8, 8);
    style.IndentSpacing = 14;
    style.WindowRounding = style.FrameRounding = style.TabRounding = 0;
    style.FrameBorderSize = 0;
    style.ScrollbarSize = 11;
    // Semantic colors from Gin's res/fx/fxed_ui_theme.json.
    const ImVec4 panel(.01f,.01f,.01f,1), fold(.03f,.03f,.03f,1),
        button(.10f,.10f,.10f,1), hot(.24f,.28f,.36f,.99f), active(.18f,.20f,.28f,1),
        selected(.20f,.24f,.30f,.99f), track(.10f,.10f,.12f,.95f), fill(.30f,.50f,.95f,.95f);
    auto &c = style.Colors;
    c[ImGuiCol_Text] = {.9f,.9f,.9f,1};
    c[ImGuiCol_TextDisabled] = {.55f,.57f,.60f,1};
    c[ImGuiCol_WindowBg] = c[ImGuiCol_ChildBg] = c[ImGuiCol_PopupBg] = panel;
    c[ImGuiCol_MenuBarBg] = c[ImGuiCol_TitleBg] = c[ImGuiCol_Header] = fold;
    c[ImGuiCol_Border] = c[ImGuiCol_Separator] = {.17f,.18f,.20f,1};
    c[ImGuiCol_TitleBgActive] = c[ImGuiCol_TabSelected] = selected;
    c[ImGuiCol_FrameBg] = c[ImGuiCol_Tab] = track;
    c[ImGuiCol_Button] = button;
    c[ImGuiCol_FrameBgHovered] = c[ImGuiCol_HeaderHovered] = c[ImGuiCol_ButtonHovered] = c[ImGuiCol_TabHovered] = hot;
    c[ImGuiCol_FrameBgActive] = c[ImGuiCol_ButtonActive] = c[ImGuiCol_HeaderActive] = active;
    c[ImGuiCol_CheckMark] = {.95f,.95f,.95f,1};
    c[ImGuiCol_SliderGrab] = c[ImGuiCol_TabSelectedOverline] = c[ImGuiCol_NavCursor] = fill;
    c[ImGuiCol_SliderGrabActive] = {1,1,1,1};
    c[ImGuiCol_ScrollbarBg] = {.12f,.12f,.12f,.9f};
    c[ImGuiCol_ScrollbarGrab] = {.35f,.35f,.35f,1};
    return true;
}


void kh_ui_close(void) {
    if (!ImGui::GetCurrentContext()) return;
    kh_rs_close();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    platform = {};
}
bool kh_ui_device_open(bgfx_program_handle_t program, bgfx_uniform_handle_t sampler) {
    auto &ui = platform;
    ui.program = program;
    ui.sampler = sampler;
    bgfx_vertex_layout_begin(&ui.vertexLayout, bgfx_get_renderer_type());
    bgfx_vertex_layout_add(&ui.vertexLayout, BGFX_ATTRIB_POSITION, 2, BGFX_ATTRIB_TYPE_FLOAT, false, false);
    bgfx_vertex_layout_add(&ui.vertexLayout, BGFX_ATTRIB_TEXCOORD0, 2, BGFX_ATTRIB_TYPE_FLOAT, false, false);
    bgfx_vertex_layout_add(&ui.vertexLayout, BGFX_ATTRIB_COLOR0, 4, BGFX_ATTRIB_TYPE_UINT8, true, false);
    bgfx_vertex_layout_end(&ui.vertexLayout);
    auto *atlas = ImGui::GetIO().Fonts;
    unsigned char *rgba;
    int width, height;
    atlas->GetTexDataAsRGBA32(&rgba, &width, &height);
    const auto limit = bgfx_get_caps()->limits.maxTextureSize;
    if (!rgba || width <= 0 || height <= 0 || width > limit || height > limit) {
        SDL_Log("Editor font atlas is too large for this renderer");
        return false;
    }
    ui.font = bgfx_create_texture_2d(width, height, false, 1, BGFX_TEXTURE_FORMAT_RGBA8,
                                     BGFX_SAMPLER_UVW_CLAMP, bgfx_copy(rgba, width * height * 4), 0);
    atlas->SetTexID(ui.font.idx);
    ui.panel = loadPanel("panel.bmp");
    ui.button = loadPanel("button.bmp");
    return ui.font.idx != Invalid;
}

void kh_ui_device_close(void) {
    for (auto *texture : {&platform.panel, &platform.button}) {
        if (texture->idx != Invalid) bgfx_destroy_texture(*texture);
        texture->idx = Invalid;
    }
    if (platform.font.idx != Invalid) {
        bgfx_destroy_texture(platform.font);
        platform.font.idx = Invalid;
    }
}


void kh_ui_event(const SDL_Event *event) {
    if (event->type == SDL_EVENT_WINDOW_FOCUS_LOST ||
        (event->type == SDL_EVENT_MOUSE_BUTTON_DOWN && !kh_ui_viewport_hit(event->button.x, event->button.y)))
        kh_rs_blur();
    ImGui_ImplSDL3_ProcessEvent(event);
}

bool kh_ui_captures_keyboard(void) { return ImGui::GetIO().WantCaptureKeyboard; }

bool kh_ui_uses_escape(void) {
    return ImGui::GetIO().WantTextInput || ImGui::IsAnyItemActive() ||
        ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopup);
}

bool kh_ui_viewport_key(const SDL_KeyboardEvent *event) {
    const auto mods = event->mod;
    const uint32_t mask = ((mods & SDL_KMOD_CTRL) ? 1u : 0u) |
        ((mods & SDL_KMOD_SHIFT) ? 2u : 0u) | ((mods & SDL_KMOD_ALT) ? 4u : 0u) |
        ((mods & (SDL_KMOD_GUI | SDL_KMOD_MODE)) ? 8u : 0u);
    return kh_rs_key(event->key, mask, event->repeat);
}

bool kh_ui_gizmo_event(const SDL_Event *event) {
    switch (event->type) {
    case SDL_EVENT_WINDOW_MINIMIZED:
    case SDL_EVENT_WINDOW_FOCUS_LOST: return kh_rs_pointer(0, 0, 0, 0);
    case SDL_EVENT_MOUSE_MOTION:
        if (event->motion.which == SDL_TOUCH_MOUSEID) return false;
        return kh_rs_pointer(1, 0, event->motion.x, event->motion.y);
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
        if (event->button.which == SDL_TOUCH_MOUSEID) return false;
        return kh_rs_pointer(event->type == SDL_EVENT_MOUSE_BUTTON_DOWN ? 2 : 3,
                             event->button.button, event->button.x, event->button.y);
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
        return kh_rs_pointer(event->type == SDL_EVENT_KEY_DOWN ? 4 : 5, event->key.key, 0, 0);
    default: return false;
    }
}

bool kh_ui_viewport_rect(float rect[4]) {
    const auto *node = platform.dockspace ? ImGui::DockBuilderGetCentralNode(platform.dockspace) : nullptr;
    if (!node || node->Size.x < 1 || node->Size.y < 1) return false;
    rect[0] = node->Pos.x; rect[1] = node->Pos.y;
    rect[2] = node->Size.x; rect[3] = node->Size.y;
    return true;
}

bool kh_ui_viewport_hit(float x, float y) {
    float rect[4];
    if (!kh_ui_viewport_rect(rect) || x < rect[0] || y < rect[1] ||
        x >= rect[0] + rect[2] || y >= rect[1] + rect[3] ||
        ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopup)) return false;
    ImGuiWindow *hovered = nullptr, *underMoving = nullptr;
    ImGui::FindHoveredWindowEx(ImVec2(x, y), true, &hovered, &underMoving);
    return !hovered;
}

int kh_ui_draw(float exposure) {
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    kh_rs_draw();
    if (platform.smoke) {
        const auto *fallback = ImGui::FindWindowByName("Debug##Default");
        if (fallback && fallback->WriteAccessed) {
            ImGui::EndFrame();
            SDL_Log("Editor drew into ImGui's implicit fallback window");
            return -1;
        }
    }
    ImGui::Render();
    const auto &data = *ImGui::GetDrawData();
    if (platform.smoke && data.TotalVtxCount == 0) {
        if (SDL_GetTicks() - platform.started < 10000) return 0;
        SDL_Log("Editor smoke timed out waiting for ImGui draw data");
        return -1;
    }
    if (!submitUi(data)) return -1;
    return kh_rs_presented(exposure);
}

// These functions expose ImGui and SDL primitives to the Rust editor.
void ki_dock(const char *sceneName, const char *documentName, float left, float right) {
    const auto *viewport = ImGui::GetMainViewport();
    const auto id = ImGui::GetID("KalamWorkspace");
    platform.dockspace = id;
    const auto flags = ImGuiDockNodeFlags_PassthruCentralNode | ImGuiDockNodeFlags_NoDockingOverCentralNode;
    if (!ImGui::DockBuilderGetNode(id) && viewport->WorkSize.x > 0 && viewport->WorkSize.y > 0) {
        ImGui::DockBuilderAddNode(id, flags | ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(id, viewport->WorkSize);
        ImGuiID center = id, scene = 0, document = 0;
        ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, left, &scene, &center);
        ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, right, &document, &center);
        ImGui::DockBuilderDockWindow(sceneName, scene);
        ImGui::DockBuilderDockWindow(documentName, document);
        ImGui::DockBuilderFinish(id);
    }
    ImGui::DockSpaceOverViewport(id, viewport, flags);
}
bool ki_begin(const char *label) { return ImGui::Begin(label); }
bool ki_tool_begin(const char *label, bool *open, uint32_t dock) {
    if (dock) ImGui::SetNextWindowDockID(dock, ImGuiCond_FirstUseEver);
    return ImGui::Begin(label, open);
}
bool ki_combo(const char *label, const char *value) { return ImGui::BeginCombo(label, value); }
void ki_end_combo(void) { ImGui::EndCombo(); }
bool ki_slider(const char *label, float *v, float lo, float hi) {
    return ImGui::SliderFloat(label, v, lo, hi, "%.3g", ImGuiSliderFlags_AlwaysClamp);
}
bool ki_color(const char *label, float rgba[4]) {
    return ImGui::ColorEdit4(label, rgba, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR);
}
bool ki_section(const char *label, bool open) {
    return ImGui::CollapsingHeader(label, open ? ImGuiTreeNodeFlags_DefaultOpen : 0);
}
bool ki_game_panel_begin(const char *label, bool left) {
    float rect[4];
    if (kh_ui_viewport_rect(rect)) {
        ImGui::SetNextWindowPos(ImVec2(rect[0] + rect[2] * 0.5f + (left ? -5.f : 5.f),
                                      rect[1] + 18.f),
                               ImGuiCond_Always, ImVec2(left ? 1.f : 0.f, 0.f));
        ImGui::SetNextWindowSize(ImVec2(SDL_min(244.f, rect[2] * 0.47f), 0.f), ImGuiCond_Always);
    }
    ImGui::PushFont(platform.gameFont);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16,16));
    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(232,227,216,255));
    ImGui::PushStyleColor(ImGuiCol_TextDisabled, IM_COL32(188,183,169,255));
    ImGui::PushStyleColor(ImGuiCol_Separator, IM_COL32(116,106,85,180));
    const bool framed = platform.panel.idx != Invalid;
    const bool visible = ImGui::Begin(label, nullptr, (framed ? ImGuiWindowFlags_NoBackground : 0) |
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize |
        ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    if (visible) {
        if (framed) {
            auto *draw = ImGui::GetWindowDrawList();
            const ImVec2 pos = ImGui::GetWindowPos(), size = ImGui::GetWindowSize();
            draw->PushClipRect(pos, {pos.x + size.x, pos.y + size.y}, false);
            drawPanel(platform.panel, pos, size, 32.f);
            draw->PopClipRect();
        }
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(232,204,145,255));
        ImGui::TextUnformatted(label);
        ImGui::PopStyleColor();
        ImGui::Separator();
    }
    if (ImGui::IsWindowFocused() || ImGui::IsWindowHovered())
        ImGui::SetNextFrameWantCaptureKeyboard(false);
    return visible;
}
void ki_game_panel_end(void) {
    ImGui::End();
    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar();
    ImGui::PopFont();
}
bool ki_game_button(const char *label) {
    if (platform.button.idx == Invalid) return ImGui::Button(label, {-1,0});
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const ImVec2 size(std::max(1.f, ImGui::GetContentRegionAvail().x), ImGui::GetFrameHeight()+4.f);
    const bool pressed = ImGui::InvisibleButton(label, size, ImGuiButtonFlags_EnableNav);
    const ImU32 tint = ImGui::IsItemActive() ? IM_COL32(174,181,192,255) :
        ImGui::IsItemHovered() ? IM_COL32_WHITE : IM_COL32(203,209,216,255);
    drawPanel(platform.button, pos, size, 11.f, tint);
    const float textWidth = ImGui::CalcTextSize(label).x;
    auto *dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(pos, {pos.x+size.x,pos.y+size.y}, true);
    dl->AddText({pos.x+std::max(4.f,(size.x-textWidth)*.5f),pos.y+(size.y-ImGui::GetFontSize())*.5f},
                ImGui::GetColorU32(ImGuiCol_Text), label);
    dl->PopClipRect();
    if (ImGui::IsItemFocused())
        dl->AddRect(pos, {pos.x+size.x,pos.y+size.y}, IM_COL32(232,204,145,255));
    return pressed;
}
void ki_end(void) { ImGui::End(); }
uint32_t ki_dock_id(void) { return ImGui::GetWindowDockID(); }
bool ki_begin_child(const char *label, bool horizontal) {
    return ImGui::BeginChild(label, ImVec2(), ImGuiChildFlags_Borders,
                            horizontal ? ImGuiWindowFlags_HorizontalScrollbar : ImGuiWindowFlags_None);
}
void ki_end_child(void) { ImGui::EndChild(); }
bool ki_menu_bar(void) { return ImGui::BeginMainMenuBar(); }
void ki_end_menu_bar(void) { ImGui::EndMainMenuBar(); }
void ki_same_line(void) { ImGui::SameLine(); }
void ki_same_line_at(float x) { ImGui::SameLine(x); }
void ki_frame_input(float values[4]) {
    const auto &io = ImGui::GetIO();
    values[0] = io.DisplaySize.x;
    values[1] = io.DisplaySize.y;
    values[2] = io.MousePos.x;
    values[3] = io.MousePos.y;
}
void ki_separator(void) { ImGui::Separator(); }
void ki_align_text(void) { ImGui::AlignTextToFramePadding(); }
void ki_cursor_x(float x) { ImGui::SetCursorPosX(x); }
void ki_next_width(float x) { ImGui::SetNextItemWidth(x); }
void ki_next_content_width(float x) { ImGui::SetNextWindowContentSize(ImVec2(x, 0)); }
void ki_code_font(bool on) { if (on) ImGui::PushFont(platform.codeFont); else ImGui::PopFont(); }
void ki_disabled(bool on) { if (on) ImGui::BeginDisabled(); else ImGui::EndDisabled(); }
void ki_active_style(bool on) {
    if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_Header));
    else ImGui::PopStyleColor();
}
void ki_text(const char *begin, const char *end, bool disabled, bool wrapped) {
    if (disabled) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    if (wrapped) ImGui::PushTextWrapPos();
    ImGui::TextUnformatted(begin, end);
    if (wrapped) ImGui::PopTextWrapPos();
    if (disabled) ImGui::PopStyleColor();
}
bool ki_button(const char *label, float w, float h) { return ImGui::Button(label, {w, h}); }
bool ki_selectable(const char *label, bool selected) { return ImGui::Selectable(label, selected); }
bool ki_tree(const char *label) { return ImGui::TreeNodeEx(label, ImGuiTreeNodeFlags_DefaultOpen); }
void ki_end_tree(void) { ImGui::TreePop(); }
void ki_push_id(const char *id) { ImGui::PushID(id); }
void ki_pop_id(void) { ImGui::PopID(); }
bool ki_bool(bool *value) { return ImGui::Checkbox("##value", value); }
bool ki_drag_float(double *data, int count, double min, double max, bool bounded) {
    if (count == 1)
        return ImGui::DragScalar("##value", ImGuiDataType_Double, data, .02f, bounded ? &min : nullptr,
                                 bounded ? &max : nullptr, "%.6g", ImGuiSliderFlags_AlwaysClamp);
    return ImGui::DragScalarN("##value", ImGuiDataType_Double, data, count, .02f,
                              bounded ? &min : nullptr, bounded ? &max : nullptr,
                              "%.6g", ImGuiSliderFlags_AlwaysClamp);
}
bool ki_drag_int(int32_t *data, int32_t min, int32_t max, bool bounded) {
    return ImGui::DragScalar("##value", ImGuiDataType_S32, data, .1f, bounded ? &min : nullptr,
                             bounded ? &max : nullptr, "%d", ImGuiSliderFlags_AlwaysClamp);
}
bool ki_input(const char *label, const char *hint, char *buffer, size_t capacity, bool enter) {
    const auto flags = enter ? ImGuiInputTextFlags_EnterReturnsTrue : ImGuiInputTextFlags_None;
    return ImGui::InputTextWithHint(label, hint ? hint : "", buffer, capacity, flags);
}
void ki_tooltip(const char *text) {
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", text);
}
void ki_open_popup(const char *name) { ImGui::OpenPopup(name); }
bool ki_popup(const char *name, bool modal) {
    return modal ? ImGui::BeginPopupModal(name, nullptr, ImGuiWindowFlags_AlwaysAutoResize)
                 : ImGui::BeginPopup(name);
}
void ki_end_popup(void) { ImGui::EndPopup(); }
void ki_close_popup(void) { ImGui::CloseCurrentPopup(); }
void ki_focus_first(void) { if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere(); }
void ki_focus_viewport(void) { ImGui::SetWindowFocus(nullptr); }
void ki_clear_active(void) { ImGui::ClearActiveID(); }
uint32_t ki_active(void) { return ImGui::GetActiveID(); }
uint32_t ki_key_state(void) {
    const auto &io = ImGui::GetIO();
    auto *scene = ImGui::FindWindowByName("Scene");
    return (io.WantTextInput ? 1u : 0u) |
        (ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopup) ? 2u : 0u) |
        (scene && scene == GImGui->NavWindow ? 4u : 0u) |
        ((SDL_GetModState() & SDL_KMOD_CTRL) ? 8u : 0u);
}
bool ki_shortcut(uint32_t key) {
    const auto &io = ImGui::GetIO();
    if (key == 's') return io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false);
    return (!io.WantTextInput && io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_Semicolon, false)) ||
           (io.KeyCtrl && io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_P, false));
}
bool ki_clipboard(const char *text) { return SDL_SetClipboardText(text); }
void ki_capture(bool capture) { SDL_CaptureMouse(capture); }
void ki_quit(void) { SDL_Event event{}; event.type = SDL_EVENT_QUIT; SDL_PushEvent(&event); }
float ki_measure(const char *begin, const char *end) {
    return ImGui::GetFont()->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX, 0.f, begin, end).x;
}
void ki_metrics(float values[22]) {
    const auto &io = ImGui::GetIO();
    const auto &style = ImGui::GetStyle();
    const auto cursor = ImGui::GetCursorScreenPos();
    const auto lo = ImGui::GetItemRectMin(), hi = ImGui::GetItemRectMax();
    const float data[] = {io.DisplaySize.x, io.DisplaySize.y, io.MousePos.x, io.MousePos.y,
        io.Framerate, ImGui::GetFrameHeight(), style.ItemSpacing.x, style.FramePadding.x,
        ImGui::GetWindowWidth(), ImGui::GetCursorPosX(), cursor.x, cursor.y, lo.x, lo.y, hi.x, hi.y,
        ImGui::GetFontSize(), ImGui::GetTextLineHeightWithSpacing(),
        ImGui::GetWindowDrawList()->GetClipRectMax().x, ImGui::GetContentRegionAvail().x,
        ImGui::GetScrollY(), ImGui::GetContentRegionAvail().y};
    std::copy_n(data, 22, values);
}
void ki_dummy(float w, float h) { ImGui::Dummy({w, h}); }
void ki_cursor_resize(void) { ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll); }
void *ki_draw_list(bool viewport) {
    if (!viewport) return ImGui::GetWindowDrawList();
    const auto *node = ImGui::DockBuilderGetNode(platform.dockspace);
    return node && node->HostWindow ? node->HostWindow->DrawList : nullptr;
}
void ki_clip(void *list, bool push, const float *rect) {
    auto *draw = static_cast<ImDrawList *>(list);
    if (push) draw->PushClipRect({rect[0], rect[1]}, {rect[0]+rect[2], rect[1]+rect[3]}, false);
    else draw->PopClipRect();
}
void ki_shape(void *list, uint32_t kind, const float *points, uint32_t count, uint32_t color, float stroke) {
    auto *draw = static_cast<ImDrawList *>(list);
    ImVec2 v[4]{};
    if (count > 4) return;
    for (unsigned i = 0; i < count; ++i) v[i] = ImVec2(points[2*i], points[2*i+1]);
    switch (kind) {
    case 0: draw->AddLine(v[0], v[1], color, stroke); break;
    case 1: draw->AddRectFilled(v[0], v[1], color); break;
    case 2: draw->AddConvexPolyFilled(v, count, color); break;
    case 3: draw->AddPolyline(v, count, color, ImDrawFlags_Closed, stroke); break;
    }
}
void ki_draw_text(void *list, float x, float y, uint32_t color, const char *begin, const char *end) {
    static_cast<ImDrawList *>(list)->AddText({x, y}, color, begin, end);
}
} // extern "C"
