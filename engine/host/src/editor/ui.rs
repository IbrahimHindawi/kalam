use glam::Vec2;
use std::ffi::{c_char, c_void, CStr, CString};

extern "C" {
    pub fn ki_dock(a: *const c_char, b: *const c_char, left: f32, right: f32);
    pub fn ki_begin(label: *const c_char) -> bool;
    pub fn ki_game_panel_begin(label: *const c_char, left: bool) -> bool;
    pub fn ki_end();
    pub fn ki_dock_id() -> u32;
    pub fn ki_begin_child(label: *const c_char, horizontal: bool) -> bool;
    pub fn ki_end_child();
    pub fn ki_menu_bar() -> bool;
    pub fn ki_end_menu_bar();
    pub fn ki_same_line();
    fn ki_frame_input(values: *mut f32);
    pub fn ki_separator();
    pub fn ki_align_text();
    pub fn ki_cursor_x(x: f32);
    pub fn ki_next_width(x: f32);
    pub fn ki_next_content_width(x: f32);
    pub fn ki_code_font(on: bool);
    pub fn ki_disabled(on: bool);
    pub fn ki_active_style(on: bool);
    fn ki_text(begin: *const c_char, end: *const c_char, disabled: bool, wrapped: bool);
    fn ki_button(label: *const c_char, w: f32, h: f32) -> bool;
    fn ki_selectable(label: *const c_char, selected: bool) -> bool;
    fn ki_tree(label: *const c_char) -> bool;
    pub fn ki_end_tree();
    fn ki_push_id(id: *const c_char);
    pub fn ki_pop_id();
    pub fn ki_bool(value: *mut bool) -> bool;
    pub fn ki_drag_float(data: *mut f64, count: i32, min: f64, max: f64, bounded: bool) -> bool;
    pub fn ki_drag_int(data: *mut i32, min: i32, max: i32, bounded: bool) -> bool;
    fn ki_input(
        label: *const c_char,
        hint: *const c_char,
        buffer: *mut u8,
        capacity: usize,
        enter: bool,
    ) -> bool;
    pub fn ki_tooltip(text: *const c_char);
    pub fn ki_open_popup(name: *const c_char);
    pub fn ki_popup(name: *const c_char, modal: bool) -> bool;
    pub fn ki_end_popup();
    pub fn ki_close_popup();
    pub fn ki_focus_first();
    pub fn ki_focus_viewport();
    pub fn ki_clear_active();
    pub fn ki_active() -> u32;
    pub fn ki_key_state() -> u32;
    pub fn ki_shortcut(key: u32) -> bool;
    fn ki_clipboard(text: *const c_char) -> bool;
    pub fn ki_capture(capture: bool);
    pub fn ki_quit();
    fn ki_measure(begin: *const c_char, end: *const c_char) -> f32;
    fn ki_metrics(values: *mut f32);
    pub fn ki_dummy(w: f32, h: f32);
    pub fn ki_cursor_resize();
    fn ki_draw_list(viewport: bool) -> *mut c_void;
    fn ki_clip(list: *mut c_void, push: bool, rect: *const f32);
    fn ki_shape(
        list: *mut c_void,
        kind: u32,
        points: *const [f32; 2],
        count: u32,
        color: u32,
        stroke: f32,
    );
    fn ki_draw_text(
        list: *mut c_void,
        x: f32,
        y: f32,
        color: u32,
        begin: *const c_char,
        end: *const c_char,
    );
    pub fn kh_ui_viewport_hit(x: f32, y: f32) -> bool;
    fn kh_ui_viewport_rect(rect: *mut f32) -> bool;
}

pub fn c_string(text: &str) -> CString {
    CString::new(text).unwrap_or_else(|_| CString::new(text.replace('\0', "�")).unwrap())
}

pub fn button(label: &str, w: f32, h: f32) -> bool {
    unsafe { ki_button(c_string(label).as_ptr(), w, h) }
}
pub fn selectable(label: &str, selected: bool) -> bool {
    unsafe { ki_selectable(c_string(label).as_ptr(), selected) }
}
pub fn tree(label: &str) -> bool {
    unsafe { ki_tree(c_string(label).as_ptr()) }
}
pub fn push_id(id: &str) {
    unsafe { ki_push_id(c_string(id).as_ptr()) };
}

fn span(value: &str) -> (*const c_char, *const c_char) {
    let start = value.as_ptr().cast();
    (start, value.as_bytes().as_ptr_range().end.cast())
}

pub fn text(value: &str, disabled: bool, wrapped: bool) {
    let (start, end) = span(value);
    unsafe { ki_text(start, end, disabled, wrapped) };
}
pub fn copy(value: &str) -> bool {
    unsafe { ki_clipboard(c_string(value).as_ptr()) }
}
pub fn measure(value: &str) -> f32 {
    let (start, end) = span(value);
    unsafe { ki_measure(start, end) }
}

pub fn input(label: &CStr, hint: &CStr, buffer: &mut [u8], enter: bool) -> bool {
    unsafe {
        ki_input(
            label.as_ptr(),
            hint.as_ptr(),
            buffer.as_mut_ptr(),
            buffer.len(),
            enter,
        )
    }
}

pub fn input_string(value: &str) -> Option<String> {
    let mut buffer = vec![0; (value.len() + 4096).min(super::MAX_DOCUMENT + 1)];
    buffer[..value.len()].copy_from_slice(value.as_bytes());
    input(c"##value", c"", &mut buffer, false).then(|| buffer_text(&buffer))
}

pub fn buffer_text(buffer: &[u8]) -> String {
    let end = buffer
        .iter()
        .position(|&ch| ch == 0)
        .unwrap_or(buffer.len());
    String::from_utf8_lossy(&buffer[..end]).into_owned()
}

pub struct Metrics([f32; 22]);

pub fn frame_input() -> (Vec2, Vec2) {
    let mut input = [0.; 4];
    unsafe { ki_frame_input(input.as_mut_ptr()) };
    (Vec2::from_slice(&input), Vec2::from_slice(&input[2..]))
}

impl Metrics {
    pub fn get() -> Self {
        let mut metrics = Self([0.; 22]);
        unsafe { ki_metrics(metrics.0.as_mut_ptr()) };
        metrics
    }
    pub fn fps(&self) -> f32 {
        self.0[4]
    }
    pub fn frame_height(&self) -> f32 {
        self.0[5]
    }
    pub fn spacing(&self) -> f32 {
        self.0[6]
    }
    pub fn padding(&self) -> f32 {
        self.0[7]
    }
    pub fn width(&self) -> f32 {
        self.0[8]
    }
    pub fn cursor_x(&self) -> f32 {
        self.0[9]
    }
    pub fn cursor(&self) -> Vec2 {
        Vec2::from_slice(&self.0[10..12])
    }
    pub fn item_rect(&self) -> (Vec2, Vec2) {
        (
            Vec2::from_slice(&self.0[12..14]),
            Vec2::from_slice(&self.0[14..16]),
        )
    }
    pub fn font(&self) -> f32 {
        self.0[16]
    }
    pub fn line_height(&self) -> f32 {
        self.0[17]
    }
    pub fn clip_right(&self) -> f32 {
        self.0[18]
    }
    pub fn available_width(&self) -> f32 {
        self.0[19]
    }
    pub fn scroll_y(&self) -> f32 {
        self.0[20]
    }
    pub fn available_height(&self) -> f32 {
        self.0[21]
    }
}

pub fn viewport_rect() -> Option<[f32; 4]> {
    let mut rect = [0.; 4];
    unsafe { kh_ui_viewport_rect(rect.as_mut_ptr()) }.then_some(rect)
}
pub fn viewport_hit(point: Vec2) -> bool {
    unsafe { kh_ui_viewport_hit(point.x, point.y) }
}

pub struct DrawList(*mut c_void);
impl DrawList {
    pub fn get(viewport: bool) -> Option<Self> {
        let ptr = unsafe { ki_draw_list(viewport) };
        (!ptr.is_null()).then_some(Self(ptr))
    }
    pub fn with_clip(&self, rect: [f32; 4], draw: impl FnOnce()) {
        unsafe { ki_clip(self.0, true, rect.as_ptr()) };
        draw();
        unsafe { ki_clip(self.0, false, std::ptr::null()) };
    }
    fn shape<const N: usize>(&self, kind: u32, points: [Vec2; N], color: u32, thickness: f32) {
        let points = points.map(|point| point.to_array());
        unsafe { ki_shape(self.0, kind, points.as_ptr(), N as u32, color, thickness) };
    }
    pub fn line(&self, a: Vec2, b: Vec2, color: u32, thickness: f32) {
        self.shape(0, [a, b], color, thickness);
    }
    pub fn rect(&self, min: Vec2, max: Vec2, color: u32) {
        self.shape(1, [min, max], color, 0.);
    }
    pub fn fill<const N: usize>(&self, points: [Vec2; N], color: u32) {
        self.shape(2, points, color, 0.);
    }
    pub fn outline<const N: usize>(&self, points: [Vec2; N], color: u32, thickness: f32) {
        self.shape(3, points, color, thickness);
    }
    pub fn text(&self, pos: Vec2, color: u32, value: &str) {
        let (start, end) = span(value);
        unsafe { ki_draw_text(self.0, pos.x, pos.y, color, start, end) };
    }
}
pub const fn rgba(r: u8, g: u8, b: u8, a: u8) -> u32 {
    u32::from_le_bytes([r, g, b, a])
}
