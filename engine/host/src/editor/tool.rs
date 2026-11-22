use super::ui::*;
use std::marker::PhantomData;
use std::rc::Rc;

/// ImGui controls for a project-owned dockable panel. The host owns the
/// window and supplies this handle only on the thread driving the frame.
pub struct EditorUi {
    visible: bool,
    pub(crate) save_draft: Option<fn() -> Result<(), String>>,
    thread: PhantomData<Rc<()>>,
}

impl EditorUi {
    pub(crate) fn new(visible: bool) -> Self {
        Self {
            visible,
            save_draft: None,
            thread: PhantomData,
        }
    }

    /// False for the final notification when the user closes the panel.
    pub fn visible(&self) -> bool {
        self.visible
    }
    /// Participate in the editor's Save and Close dialog while a project
    /// tool holds a dirty document, even after its panel has been hidden.
    pub fn unsaved(&mut self, save: fn() -> Result<(), String>) {
        self.save_draft = Some(save);
    }

    pub fn text(&self, s: &str) {
        text(s, false, true);
    }
    pub fn note(&self, s: &str) {
        text(s, true, true);
    }
    pub fn inline(&self) {
        unsafe { ki_same_line() }
    }
    pub fn section(&self, name: &str, initially_open: bool) -> bool {
        unsafe { ki_section(c_string(name).as_ptr(), initially_open) }
    }
    pub fn button(&self, label: &str, enabled: bool) -> bool {
        if !enabled { unsafe { ki_disabled(true) }; }
        let hit = button(label, 0., 0.);
        if !enabled { unsafe { ki_disabled(false) }; }
        enabled && hit
    }
    pub fn choice<S: AsRef<str>>(&self, name: &str, current: &str, options: &[S]) -> Option<usize> {
        if !unsafe { ki_combo(c_string(name).as_ptr(), c_string(current).as_ptr()) } {
            return None;
        }
        let picked = options.iter().enumerate().find_map(|(index, option)| {
            selectable(option.as_ref(), option.as_ref() == current).then_some(index)
        });
        unsafe { ki_end_combo() };
        picked
    }
    pub fn field<T>(&self, name: &str, edit: impl FnOnce() -> T) -> T {
        push_id(name);
        text(name, true, false);
        unsafe { ki_next_width(-1.) };
        let value = edit();
        unsafe { ki_pop_id() };
        value
    }
    pub fn string(&self, s: &str) -> Option<String> {
        if s.len() > 4096 {
            self.note("This text is too long to edit inline.");
            return None;
        }
        input_string(s)
    }
    pub fn toggle(&self, value: &mut bool) -> bool {
        unsafe { ki_bool(value) }
    }
    pub fn numbers(&self, values: &mut [f64]) -> bool {
        assert!((1..=4).contains(&values.len()));
        unsafe { ki_drag_float(values.as_mut_ptr(), values.len() as i32, 0., 0., false) }
    }
    pub fn integer(&self, value: &mut i32) -> bool {
        unsafe { ki_drag_int(value, 0, i32::MAX, false) }
    }
    pub fn slider(&self, name: &str, value: &mut f32, min: f32, max: f32) -> bool {
        unsafe { ki_slider(c_string(name).as_ptr(), value, min, max) }
    }
    pub fn color(&self, rgba: &mut [f32; 4]) -> bool {
        unsafe { ki_color(c"##value".as_ptr(), rgba.as_mut_ptr()) }
    }
}
