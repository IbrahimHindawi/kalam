mod document;
mod gizmo;
mod project;
pub(crate) mod tool;
mod ui;

use crate::{game, host};
use document::{set_value, Document};
use gizmo::{Gizmo, Mode};
use glam::Vec2;
use serde_json::Value;
use std::cell::RefCell;
use std::collections::{BTreeMap, VecDeque};
use std::ffi::{c_char, CStr};
use std::fs;
use std::path::{Path, PathBuf};
use std::time::{Duration, Instant, SystemTime};
use ui::{self as imgui, *};

const MAX_DOCUMENT: usize = 256 * 1024;
const MATERIAL: &str = "materials/reference.json";

type Drafts = BTreeMap<String, String>;

struct Edit {
    path: String,
    before: String,
    after: String,
}
struct FormEdit {
    widget: u32,
    path: String,
    before: String,
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum Play {
    Stopped,
    Playing,
    Paused,
}

struct Editor {
    scene: String,
    manifest: String,
    document: String,
    selection: String,
    project_documents: Vec<String>,
    project: project::Browser,
    root: Option<PathBuf>,
    watched: BTreeMap<String, Option<SystemTime>>,
    documents: BTreeMap<String, Document>,
    previewed: Drafts,
    status: String,
    undo: VecDeque<Edit>,
    redo: Vec<Edit>,
    form_edit: Option<FormEdit>,
    preview_at: Option<Instant>,
    check_at: Instant,
    reload: bool,
    raw: bool,
    focused: bool,
    frame_selection: bool,
    play: Play,
    play_revision: u32,
    gizmo: Gizmo,
    modal: bool,
    quit: bool,
    command: [u8; 512],
    scene_dock: u32,
    tool_open: bool,
    tool_save: Option<fn() -> Result<(), String>>,
    document_dock: u32,
    smoke: bool,
    smoke_frames: u32,
    expected_exposure: f32,
}

extern "C" {
    fn kalam_select_entity(id: *const c_char);
    fn kh_ui_captures_keyboard() -> bool;
}

impl Editor {
    fn new(scene: &str, smoke: bool) -> Self {
        let manifest = format!("scenes/{scene}.json");
        let mut project_documents = vec![MATERIAL.to_owned()];
        if let Some(app) = game::application(scene) {
            project_documents.extend(app.documents().iter().map(|path| (*path).to_owned()));
        }
        let mut state = Self {
            scene: scene.into(),
            document: manifest.clone(),
            manifest: manifest.clone(),
            selection: String::new(),
            project_documents,
            project: project::Browser::default(),
            root: std::env::var_os("KALAM_CONTENT_ROOT").map(PathBuf::from),
            watched: BTreeMap::new(),
            documents: BTreeMap::new(),
            previewed: BTreeMap::new(),
            status: String::new(),
            undo: VecDeque::new(),
            redo: Vec::new(),
            form_edit: None,
            preview_at: None,
            check_at: Instant::now(),
            reload: false,
            raw: false,
            focused: false,
            frame_selection: false,
            play: Play::Stopped,
            play_revision: 0,
            gizmo: Gizmo::default(),
            modal: false,
            quit: false,
            command: [0; 512],
            scene_dock: 0,
            tool_open: std::env::var_os("KALAM_EDITOR_TOOL").is_some(),
            tool_save: None,
            document_dock: 0,
            smoke,
            smoke_frames: 0,
            expected_exposure: 0.,
        };
        state.watch(&manifest);
        for path in state.project_documents.clone() {
            state.watch(&path);
        }
        state
    }

    fn read(path: &str) -> Option<String> {
        let bytes = game::read_asset(path, MAX_DOCUMENT).ok()?;
        String::from_utf8(bytes).ok()
    }

    fn document(&mut self, path: &str) -> Option<&mut Document> {
        if !self.documents.contains_key(path) {
            let Some(text) = Self::read(path) else {
                self.status = format!("Could not open {path}");
                return None;
            };
            self.documents.insert(path.into(), Document::new(text));
        }
        self.documents.get_mut(path)
    }

    fn watch(&mut self, path: &str) {
        if !self.watched.contains_key(path) {
            self.watched
                .insert(path.into(), modified(self.root.as_deref(), path));
        }
    }

    fn refresh(&mut self) {
        let now = Instant::now();
        if now < self.check_at {
            return;
        }
        self.check_at = now + Duration::from_millis(250);
        let mut changes = Vec::new();
        for (path, previous) in &mut self.watched {
            let time = modified(self.root.as_deref(), path);
            if time != *previous {
                *previous = time;
                changes.push(path.clone());
            }
        }
        for path in changes {
            if self
                .documents
                .get(&path)
                .is_some_and(|doc| !doc.dirty() && Self::read(&path).as_ref() != Some(&doc.saved))
            {
                self.documents.remove(&path);
                self.forget(&path);
            }
            self.schedule(75);
        }
    }

    fn schedule(&mut self, millis: u64) {
        self.preview_at
            .get_or_insert_with(|| Instant::now() + Duration::from_millis(millis));
    }

    fn validate(&self, bytes: &[u8]) -> Result<(), String> {
        game::load_scene(&self.scene, Some(bytes))?;
        if let Some(app) = game::application(&self.scene) {
            app.validate(bytes)?;
        }
        Ok(())
    }

    fn publish(&mut self, save: bool) -> bool {
        let mut drafts: Drafts = self
            .documents
            .iter()
            .filter(|(_, doc)| doc.dirty())
            .map(|(path, doc)| (path.clone(), doc.text.clone()))
            .collect();
        let mut bytes = serde_json::to_vec(&drafts).expect("string drafts serialize");
        let mut diagnostics = Vec::new();
        if let Err(error) = self.validate(&bytes) {
            if save {
                self.status = error;
                return false;
            }
            let mut accepted = self.previewed.clone();
            accepted.retain(|path, _| drafts.contains_key(path));
            for (path, contents) in &drafts {
                if accepted.get(path) == Some(contents) {
                    continue;
                }
                let mut attempt = accepted.clone();
                attempt.insert(path.clone(), contents.clone());
                match self.validate(&serde_json::to_vec(&attempt).unwrap()) {
                    Ok(()) => accepted = attempt,
                    Err(error) => diagnostics.push(format!("Not previewed: {path}\n{error}")),
                }
            }
            bytes = serde_json::to_vec(&accepted).unwrap();
            if let Err(error) = self.validate(&bytes) {
                self.status = format!("Preview paused: {error}");
                return false;
            }
            drafts = accepted;
        }
        if save {
            let Some(root) = &self.root else {
                self.status = "Set KALAM_CONTENT_ROOT to the project asset directory.".into();
                return false;
            };
            for (path, doc) in self.documents.iter().filter(|(_, doc)| doc.dirty()) {
                if Self::read(path).as_ref() != Some(&doc.saved) {
                    self.status = format!(
                        "Changed on disk: {path}. Copy your draft or discard it before reloading."
                    );
                    return false;
                }
            }
            for (path, doc) in self.documents.iter_mut().filter(|(_, doc)| doc.dirty()) {
                if let Err(error) = write_draft(&root.join(path), &doc.text) {
                    self.status = format!("Could not save {path}: {error}");
                    return false;
                }
                doc.saved.clone_from(&doc.text);
                self.watched
                    .insert(path.clone(), modified(Some(root), path));
            }
            drafts.clear();
            bytes = b"{}".to_vec();
        }
        self.previewed = drafts;
        *host::EDITOR_DOCUMENTS.lock().unwrap() = Some(bytes);
        self.reload = true;
        self.status = if save {
            "Saved".into()
        } else {
            diagnostics.join("\n")
        };
        true
    }

    fn record(&mut self, edit: Edit) {
        if edit.before == edit.after {
            return;
        }
        if self.undo.len() == 64 {
            self.undo.pop_front();
        }
        self.undo.push_back(edit);
        self.redo.clear();
    }

    fn forget(&mut self, path: &str) {
        self.undo.retain(|edit| edit.path != path);
        self.redo.retain(|edit| edit.path != path);
        if self
            .form_edit
            .as_ref()
            .is_some_and(|edit| edit.path == path)
        {
            self.form_edit = None;
        }
    }

    fn finish_form(&mut self) {
        if let Some(form) = self.form_edit.take() {
            if let Some(doc) = self.documents.get(&form.path) {
                self.record(Edit {
                    path: form.path,
                    before: form.before,
                    after: doc.text.clone(),
                });
            }
        }
    }

    fn step_history(&mut self, forward: bool) {
        if self
            .form_edit
            .as_ref()
            .is_some_and(|form| form.widget == unsafe { ki_active() })
        {
            unsafe { ki_clear_active() };
        }
        self.finish_form();
        while let Some(edit) = if forward {
            self.redo.pop()
        } else {
            self.undo.pop_back()
        } {
            let Some(doc) = self.document(&edit.path) else {
                continue;
            };
            let (expected, next) = if forward {
                (&edit.before, &edit.after)
            } else {
                (&edit.after, &edit.before)
            };
            if &doc.text != expected {
                continue;
            }
            doc.replace(next.clone());
            if forward {
                self.undo.push_back(edit);
            } else {
                self.redo.push(edit);
            }
            self.preview_at = None;
            self.publish(false);
            return;
        }
    }

    fn select(&mut self, id: &str) {
        self.selection = id.into();
        self.document = if id.is_empty() {
            self.manifest.clone()
        } else {
            format!("scenes/{}/{id}.json", self.scene)
        };
        unsafe { kalam_select_entity(c_string(id).as_ptr()) };
    }

    fn select_document(&mut self, path: &str) {
        if self.document != path {
            self.document = path.into();
            self.selection.clear();
            unsafe { kalam_select_entity(c"".as_ptr()) };
        }
    }

    fn focus(&mut self) {
        self.focused = true;
        unsafe { ki_focus_viewport() };
    }

    fn reload_files(&mut self) {
        self.finish_form();
        let paths: Vec<_> = self
            .documents
            .iter()
            .filter(|(_, doc)| !doc.dirty())
            .map(|(path, _)| path.clone())
            .collect();
        for path in paths {
            let doc = self.documents.remove(&path).unwrap();
            if Self::read(&path).as_ref() != Some(&doc.saved) {
                self.forget(&path);
            }
        }
        self.publish(false);
    }

    fn play_action(&mut self, key: u8) {
        if key == b'p' {
            self.play = match self.play {
                Play::Stopped => {
                    self.play_revision = self.play_revision.wrapping_add(1);
                    Play::Playing
                }
                Play::Playing => Play::Paused,
                Play::Paused => Play::Playing,
            };
        } else if self.play != Play::Stopped {
            self.play_revision = self.play_revision.wrapping_add(1);
            self.play = if key == b'q' {
                Play::Stopped
            } else {
                Play::Playing
            };
        } else {
            return;
        }
        self.finish_gizmo(false);
        if self.preview_at.take().is_some() {
            self.publish(false);
        }
        if self.play == Play::Playing {
            self.focus();
        }
    }

    fn keys(&mut self, key: u32, mods: u32, repeat: bool) -> bool {
        let Ok(key) = u8::try_from(key) else {
            return false;
        };
        let key_state = unsafe { ki_key_state() };
        let text_or_popup = key_state & 3 != 0;
        if mods == 4 && matches!(key, b'p' | b'q' | b'r') {
            if !repeat {
                self.play_action(key);
            }
            return true;
        }
        if key == b'z' && mods & 1 != 0 && mods & 12 == 0 && !text_or_popup {
            if !repeat && self.gizmo.drag.is_none() {
                self.step_history(mods & 2 != 0);
            }
            return true;
        }
        if mods != 0 || self.play == Play::Playing || self.gizmo.drag.is_some() || text_or_popup {
            return false;
        }
        if key == b'f' && (self.focused || key_state & 4 != 0) {
            if !repeat {
                self.frame_selection = true;
            }
            return true;
        }
        if !self.focused || unsafe { kh_ui_captures_keyboard() } {
            return false;
        }
        let mode = match key {
            b'q' => Mode::Select,
            b'w' => Mode::Move,
            b'e' => Mode::Rotate,
            b'r' => Mode::Scale,
            _ => return false,
        };
        if !repeat {
            self.gizmo.set_mode(mode);
        }
        true
    }

    fn toolbar(&mut self) {
        if !unsafe { ki_menu_bar() } {
            return;
        }
        let m = Metrics::get();
        let width = 30.;
        if let Some((label, _)) = game::application(&self.scene).and_then(|app| app.editor_tool()) {
            if button(label, 0., m.frame_height()) {
                self.tool_open = !self.tool_open;
            }
        }
        unsafe { ki_cursor_x((m.width() - 2. * width - m.spacing()) * 0.5) };
        for stop in [false, true] {
            let disabled = stop && self.play == Play::Stopped;
            if disabled {
                unsafe { ki_disabled(true) };
            }
            if button(
                if stop { "##stop" } else { "##play" },
                width,
                m.frame_height(),
            ) {
                self.play_action(if stop { b'q' } else { b'p' });
            }
            let (min, max) = Metrics::get().item_rect();
            let p = (min + max) * 0.5;
            if let Some(draw) = DrawList::get(false) {
                let color = if disabled {
                    rgba(163, 157, 148, 140)
                } else {
                    rgba(227, 223, 216, 255)
                };
                if stop {
                    draw.rect(p - Vec2::splat(4.), p + Vec2::splat(4.), color);
                } else if self.play == Play::Playing {
                    for x in [-4., 1.] {
                        draw.rect(p + Vec2::new(x, -5.), p + Vec2::new(x + 3., 5.), color);
                    }
                } else {
                    draw.fill(
                        [
                            p + Vec2::new(-4., -5.),
                            p + Vec2::new(5., 0.),
                            p + Vec2::new(-4., 5.),
                        ],
                        color,
                    );
                }
            }
            if disabled {
                unsafe { ki_disabled(false) };
            }
            let label = if stop {
                c"Stop (Alt+Q)"
            } else {
                match self.play {
                    Play::Playing => c"Pause (Alt+P)",
                    Play::Paused => c"Resume (Alt+P)",
                    Play::Stopped => c"Play (Alt+P)",
                }
            };
            unsafe {
                ki_tooltip(label.as_ptr());
                ki_same_line();
            }
        }
        let fps = format!("{:.0} FPS", m.fps());
        let fps_x = m.width() - measure(&fps) - m.padding() * 2.;
        if fps_x > Metrics::get().cursor_x() {
            unsafe {
                ki_cursor_x(fps_x);
                ki_align_text();
            }
            imgui::text(&fps, true, false);
        }
        unsafe { ki_end_menu_bar() };
    }

    fn scene_panel(&mut self) {
        if unsafe { ki_begin(c"Scene".as_ptr()) } {
            if selectable("Scene settings", self.document == self.manifest) {
                self.select("");
            }
            for path in self.project_documents.clone() {
                let label = path.rsplit('/').next().unwrap_or(&path);
                push_id(&path);
                if selectable(label, self.document == path) {
                    self.select_document(&path);
                }
                unsafe { ki_pop_id() };
            }
            let path = self.manifest.clone();
            let entities = self
                .document(&path)
                .and_then(|doc| serde_json::from_str::<Value>(&doc.text).ok())
                .and_then(|data| {
                    Some(
                        data.get("entities")?
                            .as_array()?
                            .iter()
                            .filter_map(|id| {
                                id.as_str().filter(|id| valid_id(id)).map(str::to_owned)
                            })
                            .collect::<Vec<_>>(),
                    )
                });
            for id in entities.into_iter().flatten() {
                self.watch(&format!("scenes/{}/{id}.json", self.scene));
                if selectable(&id, self.selection == id) {
                    self.select(&id);
                }
            }
        }
        unsafe {
            self.scene_dock = ki_dock_id();
            ki_end();
        }
    }

    fn document_panel(&mut self) {
        if unsafe { ki_begin(c"Document".as_ptr()) } {
            let path = self.document.clone();
            imgui::text(&path, true, false);
            if button("Save All", 0., 0.) || unsafe { ki_shortcut(b's' as u32) } {
                self.finish_form();
                self.publish(true);
            }
            unsafe { ki_same_line() };
            if button("Reload Files", 0., 0.) {
                self.reload_files();
            }
            let dirty = self.document(&path).is_some_and(|doc| doc.dirty());
            if !dirty {
                unsafe { ki_disabled(true) };
            }
            if button("Discard Draft", 0., 0.) {
                self.finish_form();
                self.forget(&path);
                self.documents.remove(&path);
                self.publish(false);
            }
            if !dirty {
                unsafe { ki_disabled(false) };
            }
            if !self.status.is_empty() {
                imgui::text(&self.status, false, true);
            }
            unsafe { ki_separator() };
            for (name, raw) in [("Form", false), ("JSON", true)] {
                let active = self.raw == raw;
                if active {
                    unsafe { ki_active_style(true) };
                }
                let pressed = button(name, 72., 0.);
                if active {
                    unsafe { ki_active_style(false) };
                }
                if pressed && !active {
                    self.finish_form();
                    self.raw = raw;
                }
                if !raw {
                    unsafe { ki_same_line() };
                }
            }
            let raw = self.raw;
            if let Some(doc) = self.document(&path) {
                push_id(&path);
                if raw {
                    if !doc.preview() {
                        self.status = "Could not copy JSON to the clipboard.".into();
                    }
                } else if let Some(before) = doc.form() {
                    let widget = unsafe { ki_active() };
                    if self
                        .form_edit
                        .as_ref()
                        .is_some_and(|edit| edit.widget != widget || edit.path != path)
                    {
                        // The form has already returned the previous document revision.
                        if self.form_edit.as_ref().unwrap().path == path {
                            let pending = self.form_edit.take().unwrap();
                            self.record(Edit {
                                path: pending.path,
                                before: pending.before,
                                after: before.clone(),
                            });
                        } else {
                            self.finish_form();
                        }
                    }
                    self.form_edit.get_or_insert(FormEdit {
                        widget,
                        path,
                        before,
                    });
                    self.schedule(75);
                }
                unsafe { ki_pop_id() };
            }
        }
        unsafe {
            self.document_dock = ki_dock_id();
            ki_end();
        }
        if self.form_edit.as_ref().is_some_and(|edit| {
            edit.widget == 0 || edit.widget != unsafe { ki_active() } || edit.path != self.document
        }) {
            self.finish_form();
        }
    }

    fn popups(&mut self) {
        if unsafe { ki_shortcut(b':' as u32) } {
            self.command.fill(0);
            unsafe { ki_open_popup(c"Command".as_ptr()) };
        }
        if unsafe { ki_popup(c"Command".as_ptr(), false) } {
            unsafe {
                ki_focus_first();
                ki_next_width(340.);
            }
            if input(
                c"##command",
                c"scene, entity <id>, form, text, write, reload, viewport",
                &mut self.command,
                true,
            ) {
                let command = buffer_text(&self.command);
                let command = command.trim_start_matches(':');
                match command {
                    "scene" => self.select(""),
                    "form" => self.raw = false,
                    "text" => self.raw = true,
                    "write" => {
                        self.finish_form();
                        self.publish(true);
                    }
                    "reload" => self.reload_files(),
                    "viewport" => self.focus(),
                    _ => {
                        if let Some(id) = command.strip_prefix("entity ").filter(|id| {
                            valid_id(id)
                                && self
                                    .watched
                                    .contains_key(&format!("scenes/{}/{id}.json", self.scene))
                        }) {
                            self.select(id);
                        } else {
                            self.status = format!("Unknown command: {command}");
                        }
                    }
                }
                unsafe { ki_close_popup() };
            }
            unsafe { ki_end_popup() };
        }
        if std::mem::take(&mut self.modal) {
            unsafe { ki_open_popup(c"Unsaved changes".as_ptr()) };
        }
        if unsafe { ki_popup(c"Unsaved changes".as_ptr(), true) } {
            imgui::text("Save the edited documents before closing?", false, false);
            let save = button("Save and Close", 0., 0.);
            unsafe { ki_same_line() };
            let discard = button("Discard and Close", 0., 0.);
            unsafe { ki_same_line() };
            if button("Cancel", 0., 0.) {
                unsafe { ki_close_popup() };
            }
            let mut saved = save && self.publish(true);
            if saved {
                if let Some(save_tool) = self.tool_save {
                    match save_tool() {
                        Ok(()) => self.tool_save = None,
                        Err(error) => {
                            self.status = error;
                            saved = false;
                        }
                    }
                }
            }
            if discard || saved {
                self.quit = true;
                unsafe {
                    ki_quit();
                    ki_close_popup();
                }
            } else if !self.status.is_empty() {
                imgui::text(&self.status, false, true);
            }
            unsafe { ki_end_popup() };
        }
    }

    fn draw(&mut self) {
        if let Some(pos) = self
            .gizmo
            .drag
            .as_mut()
            .and_then(|drag| drag.pending.take())
        {
            self.update_drag(pos);
        }
        self.refresh();
        if self.preview_at.is_some_and(|when| Instant::now() >= when) {
            self.preview_at = None;
            self.publish(false);
        }
        let selection = game::selected_entity();
        if selection != self.selection {
            self.select(&selection);
        }
        let tool_was_open = self.tool_open;
        self.toolbar();
        let (display, _) = frame_input();
        let right = (440. / (display.x * 0.83_f32)).clamp(0.32, 0.52);
        unsafe { ki_dock(c"Scene".as_ptr(), c"Document".as_ptr(), 0.17, right) };
        self.gizmo
            .draw(self.play != Play::Playing, &self.selection, &self.document);
        self.scene_panel();
        if let Some(path) = self.project.draw(self.scene_dock, self.root.as_deref()) {
            if self.document(&path).is_some() {
                self.finish_form();
                self.watch(&path);
                self.select_document(&path);
            }
        }
        self.document_panel();
        if self.tool_open || tool_was_open {
            if let Some((name, draw)) = game::application(&self.scene).and_then(|app| app.editor_tool()) {
                if self.tool_open {
                    let visible = unsafe {
                        ki_tool_begin(c_string(name).as_ptr(), &mut self.tool_open, self.document_dock)
                    };
                    if visible && self.tool_open {
                        let mut frame = tool::EditorUi::new(true);
                        draw(&mut frame);
                        self.tool_save = frame.save_draft;
                    }
                    unsafe { ki_end() };
                }
                if !self.tool_open {
                    let mut frame = tool::EditorUi::new(false);
                    draw(&mut frame);
                    self.tool_save = frame.save_draft;
                }
            }
        }
        self.popups();
    }

    fn pointer(&mut self, kind: u32, button: u32, pos: Vec2) -> bool {
        if let Some(drag) = self.gizmo.drag.as_mut() {
            match (kind, button) {
                (0, _) => {
                    self.finish_gizmo(true);
                    return false;
                }
                (4, 27) => self.finish_gizmo(true),
                (1, _) => drag.pending = Some(pos),
                (3, 1) => {
                    self.update_drag(pos);
                    self.finish_gizmo(false);
                }
                (2 | 4 | 5, _) => {}
                _ => return false,
            }
            return true;
        }
        if (kind, button) != (2, 1) || self.play == Play::Playing || !viewport_hit(pos) {
            return false;
        }
        let Some(pick) = self
            .gizmo
            .pick(pos, &self.selection, &self.document, &self.scene)
        else {
            return false;
        };
        let Some(text) = self.document(&pick.path).map(|doc| doc.text.clone()) else {
            return false;
        };
        self.finish_form();
        self.gizmo.start(pick, text);
        self.focus();
        unsafe { ki_capture(true) };
        true
    }

    fn update_drag(&mut self, pos: Vec2) {
        let Some(drag) = &self.gizmo.drag else {
            return;
        };
        let path = drag.path.clone();
        let Some(doc) = self.documents.get(&path) else {
            self.finish_gizmo(false);
            return;
        };
        if doc.text != drag.last {
            self.finish_gizmo(false);
            return;
        }
        let Some(next) = drag.edit(pos, unsafe { ki_key_state() } & 8 != 0) else {
            return;
        };
        if next == doc.text {
            return;
        }
        let mut drafts = self.previewed.clone();
        drafts.insert(path.clone(), next.clone());
        if let Err(error) = self.validate(&serde_json::to_vec(&drafts).unwrap()) {
            self.status = error;
            return;
        }
        self.gizmo.drag.as_mut().unwrap().last.clone_from(&next);
        self.documents.get_mut(&path).unwrap().replace(next);
        self.schedule(30);
    }

    fn finish_gizmo(&mut self, cancel: bool) {
        let Some(drag) = self.gizmo.drag.take() else {
            return;
        };
        unsafe { ki_capture(false) };
        let Some(doc) = self.documents.get_mut(&drag.path) else {
            return;
        };
        if doc.text != drag.last || doc.text == drag.original {
            return;
        }
        if cancel {
            doc.replace(drag.original);
            self.preview_at = Some(Instant::now());
        } else {
            self.record(Edit {
                path: drag.path,
                before: drag.original,
                after: drag.last,
            });
        }
    }

    fn presented(&mut self, exposure: f32) -> Result<(), String> {
        if !self.smoke {
            return Ok(());
        }
        let frame = self.smoke_frames;
        self.smoke_frames += 1;
        if frame == 0 {
            if self.scene_dock == 0
                || self.document_dock == 0
                || self.scene_dock == self.document_dock
            {
                return Err("Scene and Document did not dock separately".into());
            }
            self.expected_exposure = if exposure == 1.25 { 1.5 } else { 1.25 };
            let path = self.manifest.clone();
            let expected = self.expected_exposure;
            let doc = self.document(&path).ok_or("missing manifest")?;
            let mut json: Value = serde_json::from_str(&doc.text).map_err(|err| err.to_string())?;
            set_value(
                json.get_mut("exposure").ok_or("missing exposure")?,
                Value::from(expected),
            );
            doc.replace(document::encode(&json).ok_or("cannot encode manifest")?);
            self.document(MATERIAL)
                .ok_or("missing material")?
                .replace("{".into());
            if let Some((path, draft)) =
                game::application(&self.scene).and_then(|app| app.smoke_document())
            {
                self.document(path)
                    .ok_or("missing project smoke document")?
                    .replace(draft.into());
                self.document = path.into();
            }
            if !self.publish(false) {
                return Err(self.status.clone());
            }
        } else {
            if (exposure - self.expected_exposure).abs() > 0.001
                || !self
                    .status
                    .contains("Not previewed: materials/reference.json")
            {
                return Err("valid exposure was not previewed alongside the rejected draft".into());
            }
            if frame == 1 && self.scene == "entities" {
                self.select("centerpiece");
                self.focus();
                self.raw = true;
            }
            if frame == 2 {
                eprintln!("Rust editor docked both panels and previewed a valid JSON draft");
                if self.gizmo.target.is_some() {
                    let (path, point, motion) = self
                        .gizmo
                        .smoke_drag(&self.selection, &self.document, &self.scene)
                        .ok_or("selected item did not draw a move gizmo")?;
                    let before = self
                        .document(&path)
                        .ok_or("gizmo document missing")?
                        .text
                        .clone();
                    if !self.pointer(2, 1, point) {
                        return Err("viewport gizmo did not accept a drag".into());
                    }
                    self.update_drag(motion);
                    if self.documents[&path].text == before {
                        return Err("gizmo drag did not update the draft".into());
                    }
                    self.finish_gizmo(true);
                    if self.documents[&path].text != before {
                        return Err("gizmo cancellation did not restore the draft".into());
                    }
                    if !self.pointer(2, 1, point) || !self.pointer(3, 1, motion) {
                        return Err("gizmo drag could not commit on release".into());
                    }
                    let after = self.documents[&path].text.clone();
                    if after == before
                        || !self.keys(b'z'.into(), 1, false)
                        || self.documents[&path].text != before
                        || !self.keys(b'z'.into(), 3, false)
                        || self.documents[&path].text != after
                    {
                        return Err("gizmo draft undo/redo failed".into());
                    }
                    self.step_history(false);
                    eprintln!("Rust editor verified gizmo preview, cancellation, undo and redo");
                }
            }
        }
        Ok(())
    }
}

fn valid_id(id: &str) -> bool {
    !id.is_empty()
        && id.len() <= 64
        && id
            .bytes()
            .all(|c| c.is_ascii_alphanumeric() || matches!(c, b'_' | b'-'))
}

fn modified(root: Option<&Path>, relative: &str) -> Option<SystemTime> {
    fs::metadata(root?.join(relative)).ok()?.modified().ok()
}

fn write_draft(path: &Path, text: &str) -> std::io::Result<()> {
    use std::io::Write;
    let stamp = SystemTime::now()
        .duration_since(SystemTime::UNIX_EPOCH)
        .unwrap_or_default()
        .as_nanos();
    let name = path.file_name().unwrap_or_default().to_string_lossy();
    let stage = path.with_file_name(format!("{name}.{}.{stamp}.tmp", std::process::id()));
    let mut file = fs::OpenOptions::new()
        .create_new(true)
        .write(true)
        .open(&stage)?;
    let result = file.write_all(text.as_bytes());
    drop(file);
    let result = result.and_then(|_| fs::rename(&stage, path));
    if result.is_err() {
        let _ = fs::remove_file(stage);
    }
    result
}

thread_local! { static EDITOR: RefCell<Option<Editor>> = const { RefCell::new(None) }; }
fn editor<T>(f: impl FnOnce(&mut Editor) -> T) -> Option<T> {
    EDITOR.with(|state| state.borrow_mut().as_mut().map(f))
}

#[no_mangle]
unsafe extern "C" fn kh_rs_open(scene: *const c_char, smoke: bool) -> bool {
    let Ok(scene) = (unsafe { CStr::from_ptr(scene) }).to_str() else {
        return false;
    };
    if !valid_id(scene) {
        return false;
    }
    EDITOR.with(|state| *state.borrow_mut() = Some(Editor::new(scene, smoke)));
    true
}
#[no_mangle]
extern "C" fn kh_rs_close() {
    EDITOR.with(|state| *state.borrow_mut() = None);
}
#[no_mangle]
extern "C" fn kh_rs_draw() {
    editor(Editor::draw);
    CHOICES.with(|state| {
        let mut state = state.borrow_mut();
        let Some(panel) = state.as_mut() else { return };
        for (title, left) in [(c"Equipped", true), (panel.title.as_c_str(), false)] {
            if !left && !panel.rows.iter().any(|row| row.choice.is_some()) {
                break;
            }
            if unsafe { ki_game_panel_begin(title.as_ptr(), left) } {
                for (index, row) in panel.rows.iter().enumerate() {
                    let item = if left { Some(&row.current) } else { row.choice.as_ref() };
                    let Some((name, detail)) = item else { continue };
                    imgui::push_id(&row.label);
                    imgui::text(&row.label, true, false);
                    let enabled = left || row.enabled;
                    if !enabled { unsafe { ki_disabled(true); } }
                    if unsafe { ki_game_button(c_string(name).as_ptr()) } && !left && enabled {
                        panel.picked = Some(index);
                    }
                    unsafe { ki_tooltip(detail.as_ptr()); }
                    if !enabled { unsafe { ki_disabled(false); } }
                    unsafe { ki_pop_id(); }
                }
            }
            unsafe { ki_game_panel_end(); }
        }
    });
}

struct ChoiceRow {
    label: String,
    current: (String, std::ffi::CString),
    choice: Option<(String, std::ffi::CString)>,
    enabled: bool,
}

struct Choices {
    title: std::ffi::CString,
    rows: Vec<ChoiceRow>,
    picked: Option<usize>,
}

thread_local! {
    static CHOICES: std::cell::RefCell<Option<Choices>> = const { std::cell::RefCell::new(None) };
}

pub(crate) fn present_choices(title: &str, rows: &[game::ChoiceRow<'_>]) -> Result<(bool, Option<usize>), String> {
    let rows = rows.iter().map(|row| ChoiceRow {
        label: row.label.to_owned(),
        current: (row.current.to_owned(), ui::c_string(row.current_detail)),
        choice: row.choice.map(|(name, detail)| (name.to_owned(), ui::c_string(detail))),
        enabled: row.enabled,
    }).collect();
    CHOICES.with(|state| *state.borrow_mut() = Some(Choices {
        title: ui::c_string(title), rows, picked: None,
    }));
    let result = game::present();
    let choice = CHOICES.with(|state| state.borrow_mut().take().and_then(|panel| panel.picked));
    result.map(|presented| (presented, choice))
}
#[no_mangle]
extern "C" fn kh_rs_presented(exposure: f32) -> i32 {
    match editor(|ui| ui.presented(exposure)) {
        Some(Ok(())) => 1,
        Some(Err(error)) => {
            eprintln!("Kalam editor smoke: {error}");
            -1
        }
        None => -1,
    }
}
#[no_mangle]
extern "C" fn kh_rs_key(key: u32, modifiers: u32, repeat: bool) -> bool {
    editor(|ui| ui.keys(key, modifiers, repeat)).unwrap_or(false)
}
#[no_mangle]
extern "C" fn kh_rs_pointer(kind: u32, button: u32, x: f32, y: f32) -> bool {
    editor(|ui| ui.pointer(kind, button, Vec2::new(x, y))).unwrap_or(false)
}
#[no_mangle]
extern "C" fn kh_rs_blur() {
    editor(|ui| ui.focused = false);
}
#[no_mangle]
extern "C" fn kh_ui_focus_viewport() {
    editor(Editor::focus);
}
#[no_mangle]
extern "C" fn kh_ui_take_frame_selection() -> bool {
    editor(|ui| std::mem::take(&mut ui.frame_selection)).unwrap_or(false)
}
#[no_mangle]
extern "C" fn kh_ui_project_document(index: usize, open: bool) -> bool {
    editor(|ui| {
        let Some(path) = game::application(&ui.scene).and_then(|app| app.documents().get(index))
        else {
            return false;
        };
        if open {
            ui.select_document(path);
        }
        ui.document == *path
    })
    .unwrap_or(false)
}
#[no_mangle]
extern "C" fn kh_ui_play_session() -> u32 {
    editor(|ui| {
        if ui.smoke {
            1
        } else {
            (ui.play_revision << 2) | ui.play as u32
        }
    })
    .unwrap_or(0)
}
#[no_mangle]
extern "C" fn kh_ui_toggle_play_pause() {
    editor(|ui| {
        if ui.play != Play::Stopped {
            ui.play_action(b'p');
        }
    });
}
#[no_mangle]
extern "C" fn kh_ui_request_close() -> bool {
    editor(|ui| {
        if ui.quit || (ui.tool_save.is_none() && !ui.documents.values().any(Document::dirty)) {
            return true;
        }
        ui.modal = true;
        false
    })
    .unwrap_or(true)
}
#[no_mangle]
extern "C" fn kh_ui_reload() -> bool {
    editor(|ui| std::mem::take(&mut ui.reload)).unwrap_or(false)
}
#[no_mangle]
unsafe extern "C" fn kh_ui_gizmo_target(
    id: *const c_char,
    transform: *const f32,
    projection: *const f32,
) {
    editor(|ui| {
        ui.gizmo.target = if id.is_null() {
            None
        } else {
            let id = unsafe { CStr::from_ptr(id) }.to_string_lossy().into_owned();
            Some(unsafe { gizmo::Target::new(gizmo::Object::Entity(id), transform, projection) })
        };
    });
}
#[no_mangle]
unsafe extern "C" fn kh_ui_planar_gizmo(
    document: usize,
    array: *const c_char,
    index: usize,
    transform: *const f32,
    projection: *const f32,
) {
    editor(|ui| {
        let Some(path) = game::application(&ui.scene).and_then(|app| app.documents().get(document))
        else {
            return;
        };
        let array = unsafe { CStr::from_ptr(array) }
            .to_string_lossy()
            .into_owned();
        let target = gizmo::Object::Planar {
            path: (*path).into(),
            array,
            index,
        };
        ui.gizmo.target = Some(unsafe { gizmo::Target::new(target, transform, projection) });
    });
}
