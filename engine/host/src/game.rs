//! Rust interface for a statically linked game. The native host owns SDL and bgfx.
pub use crate::host::{
    draws, entity_bounds, gpu_skinning_available, load_scene, pick_entity, render_view, render_view_at, upload,
    render_view_at_fx, Assets, Draw, FxBlend, FxMesh, FxMeshSurface, FxOpacity, FxQuad, FxQuadSurface, RenderAssets, WorldFx,
};
pub use crate::panel::{Image as HudImage, WIDTH as HUD_WIDTH};
pub use crate::resources::{Handle, Mesh, SkinPalette, Texture};
pub use crate::viewport::{
    camera_offset, frame_camera, viewport_projection, viewport_ray, viewport_size,
};
use glam::{Mat4, Vec2, Vec3};
use kalam_runtime::scene::Camera;
use std::ffi::{CStr, CString};
use std::sync::OnceLock;
#[cfg(all(windows, feature = "editor"))]
pub use crate::editor::tool::EditorUi;

pub trait Application: Sync {
    fn scene(&self) -> &str;
    fn documents(&self) -> &[&str];
    fn validate(&self, documents: &[u8]) -> Result<(), String>;
    fn run(&self, smoke: bool) -> Result<(), String>;
    /// An optional dockable project tool, drawn on the editor thread.
    #[cfg(all(windows, feature = "editor"))]
    fn editor_tool(&self) -> Option<(&'static str, fn(&mut EditorUi))> {
        None
    }
    /// Deliver Escape to this game's input while a play session is active.
    fn handles_escape(&self) -> bool {
        false
    }
    fn smoke_document(&self) -> Option<(&str, &str)> {
        None
    }
}

static APPLICATION: OnceLock<&'static dyn Application> = OnceLock::new();

/// Call from the game's entry point before the native host opens the editor.
pub fn register(application: &'static dyn Application) -> Result<(), &'static str> {
    APPLICATION
        .set(application)
        .map_err(|_| "a game is already registered")
}

pub(crate) fn application(scene: &str) -> Option<&'static dyn Application> {
    APPLICATION
        .get()
        .copied()
        .filter(|game| game.scene() == scene)
}

pub fn read_asset(path: &str, capacity: usize) -> Result<Vec<u8>, String> {
    let path = CString::new(path).map_err(|error| error.to_string())?;
    crate::host::read_document(&path, capacity)
}

pub fn take_document_edits() -> Option<Vec<u8>> {
    crate::host::EDITOR_DOCUMENTS.lock().unwrap().take()
}

#[derive(Clone, Copy, PartialEq, Eq)]
pub enum PlayState {
    Stopped,
    Playing,
    Paused,
}

#[derive(Clone, Copy)]
pub struct PlaySession {
    pub state: PlayState,
    /// Changes when a session starts, stops, or resets.
    pub revision: u32,
}

pub fn play_session() -> PlaySession {
    let value = unsafe { kh_play_session() };
    PlaySession {
        state: match value & 3 {
            1 => PlayState::Playing,
            2 => PlayState::Paused,
            _ => PlayState::Stopped,
        },
        revision: value >> 2,
    }
}

/// Pause or resume the current play session. A stopped editor stays stopped.
/// This preserves the session revision and shares the editor's Play/Pause button.
pub fn toggle_pause() {
    unsafe { kh_toggle_play_pause() };
}

#[derive(Clone, Copy, PartialEq, Eq)]
pub enum FrameStatus {
    Closed,
    Ready,
    Paused,
    DeviceRecreated,
    Reload,
}

pub struct Frame {
    pub status: FrameStatus,
    pub dt: f32,
    pub width: u32,
    pub height: u32,
    pub mouse_delta: Vec2,
}

pub fn next_frame() -> Result<Frame, String> {
    let (mut dt, mut dx, mut dy, mut width, mut height) = (0., 0., 0., 0, 0);
    let status =
        match unsafe { crate::host::kh_next(&mut dt, &mut dx, &mut dy, &mut width, &mut height) } {
            0 => FrameStatus::Closed,
            1 => FrameStatus::Ready,
            2 => FrameStatus::Paused,
            3 => FrameStatus::DeviceRecreated,
            4 => FrameStatus::Reload,
            _ => return Err("platform event or device error".into()),
        };
    Ok(Frame {
        status,
        dt,
        width,
        height,
        mouse_delta: Vec2::new(dx, dy),
    })
}

pub struct EditorInput {
    pub pick: Option<Vec2>,
    pub deselect: bool,
    pub frame_selection: bool,
    camera_keys: Option<u32>,
}

impl EditorInput {
    pub fn fly_camera(&self, camera: &mut Camera, delta: Vec2, dt: f32) -> bool {
        let Some(keys) = self.camera_keys else {
            return false;
        };
        crate::viewport::fly_editor_camera(camera, keys, delta.x, delta.y, dt);
        true
    }
}

/// Read the viewport tools for the current frame. Picking is consumed once.
pub fn editor_input() -> EditorInput {
    let (mut x, mut y) = (0., 0.);
    let bits = unsafe { crate::host::kh_editor_camera_input(&mut x, &mut y) };
    EditorInput {
        pick: (bits & (1 << 30) != 0 && bits & (1 << 28) != 0).then_some(Vec2::new(x, y)),
        deselect: bits & (1 << 27) != 0,
        frame_selection: bits & (1 << 26) != 0,
        camera_keys: (bits & (1 << 29) != 0).then_some(bits & 0x3ff),
    }
}

pub fn select_entity(id: &str) {
    unsafe { crate::host::kh_editor_picked(id.as_ptr(), id.len()) };
}

pub fn selected_entity() -> String {
    let mut id = [0; 64];
    let length = unsafe { crate::host::kh_selected(id.as_mut_ptr(), id.len()) };
    std::str::from_utf8(&id[..length])
        .unwrap_or_default()
        .to_owned()
}

pub(crate) fn viewport_rect() -> [f32; 4] {
    let mut rect = [0.; 4];
    unsafe { kh_viewport_rect(rect.as_mut_ptr()) };
    rect
}

/// Open a registered project document by its index in Application::documents.
pub fn open_project_document(index: usize) {
    unsafe { kh_editor_project_document(index, true) };
}

pub fn project_document_is_open(index: usize) -> bool {
    unsafe { kh_editor_project_document(index, false) }
}

/// Attach the viewport handles to a planar item in a registered document.
/// The item stores its position in `center` (XZ), footprint in `half_size`,
/// and orientation in `yaw_degrees`.
pub fn planar_gizmo(
    document: usize,
    array: &CStr,
    index: usize,
    center: Vec3,
    half_size: Vec2,
    yaw_degrees: f32,
    view_projection: Mat4,
) {
    let transform = [
        center.x,
        center.y,
        center.z,
        half_size.x,
        1.,
        half_size.y,
        0.,
        yaw_degrees,
        0.,
    ];
    unsafe {
        kh_editor_planar_gizmo(
            document,
            array.as_ptr(),
            index,
            transform.as_ptr(),
            gizmo_projection(view_projection).as_ptr(),
        )
    };
}

pub(crate) fn gizmo_projection(view_projection: Mat4) -> [f32; 32] {
    let mut matrices = [0.; 32];
    matrices[..16].copy_from_slice(view_projection.as_ref());
    matrices[16..].copy_from_slice(view_projection.inverse().as_ref());
    matrices
}

pub fn present() -> Result<bool, String> {
    match unsafe { crate::host::kh_present() } {
        value if value < 0 => Err("GPU frame submission failed".into()),
        value => Ok(value > 0),
    }
}

pub struct ChoiceRow<'a> {
    pub label: &'a str,
    pub current: &'a str,
    pub current_detail: &'a str,
    pub choice: Option<(&'a str, &'a str)>,
    pub enabled: bool,
}

/// Present a transient editor overlay and return the clicked row, if any.
#[cfg(all(windows, feature = "editor"))]
pub fn present_choices(title: &str, rows: &[ChoiceRow<'_>]) -> Result<(bool, Option<usize>), String> {
    crate::editor::present_choices(title, rows)
}

extern "C" {
    fn kh_viewport_rect(rect: *mut f32);
    fn kh_play_session() -> u32;
    pub(crate) fn kh_game_handles_escape(enabled: bool);
    fn kh_toggle_play_pause();
    fn kh_editor_project_document(index: usize, open: bool) -> bool;
    fn kh_editor_planar_gizmo(
        document: usize,
        array: *const std::ffi::c_char,
        index: usize,
        transform: *const f32,
        view_projection: *const f32,
    );
    fn kh_play_input(cursor: *mut f32, rect: *mut f32, bindings: *const u32) -> u32;
    fn kh_play_audio(samples: *const f32, count: i32);
}

#[derive(Clone, Copy)]
pub struct Input {
    pub cursor: [f32; 2],
    pub viewport: [f32; 4],
    pub movement: [f32; 2],
    pub aim: bool,
    pub primary: bool,
    pub secondary: bool,
    pub inspect: bool,
    pub interact: bool,
    /// A fresh Escape press when the active application opts into handling it.
    pub back: bool,
    pub pressed: [bool; 4],
}

/// Read four held movement keys (forward, backward, left, right) and four
/// project-defined actions. Use lower-case letters, digits, or a space.
pub fn input(movement: [char; 4], actions: [char; 4]) -> Input {
    let (mut cursor, mut viewport) = ([0.; 2], [0.; 4]);
    let [forward, backward, left, right] = movement;
    let [a, b, c, d] = actions;
    let keys = [forward, backward, left, right, a, b, c, d].map(u32::from);
    let bits = unsafe { kh_play_input(cursor.as_mut_ptr(), viewport.as_mut_ptr(), keys.as_ptr()) };
    let down = |mask| bits & mask != 0;
    Input {
        cursor,
        viewport,
        movement: [
            f32::from(down(8)) - f32::from(down(4)),
            f32::from(down(2)) - f32::from(down(1)),
        ],
        aim: down(32),
        primary: down(16),
        secondary: down(1024),
        inspect: down(2048),
        interact: down(4096),
        back: down(8192),
        pressed: [down(64), down(128), down(256), down(512)],
    }
}

/// Play mono, 24 kHz floating-point PCM through the host's audio stream.
pub fn play_audio(samples: &[f32]) {
    if samples.len() <= i32::MAX as usize / std::mem::size_of::<f32>() && !samples.is_empty() {
        unsafe { kh_play_audio(samples.as_ptr(), samples.len() as i32) };
    }
}
