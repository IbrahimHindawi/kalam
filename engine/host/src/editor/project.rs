use super::ui::*;
use std::collections::BTreeMap;
use std::fs;
use std::path::{Path, PathBuf};
use std::rc::Rc;

struct Entry {
    name: String,
    directory: bool,
}

#[derive(Default)]
pub(super) struct Browser {
    directories: BTreeMap<PathBuf, Option<Rc<[Entry]>>>,
}

impl Browser {
    pub(super) fn draw(&mut self, dock: u32, assets: Option<&Path>) -> Option<String> {
        let visible = unsafe { ki_tool_begin(c"Project".as_ptr(), std::ptr::null_mut(), dock) };
        let selected = if visible { self.contents(assets) } else { None };
        unsafe { ki_end() };
        selected
    }

    fn contents(&mut self, assets: Option<&Path>) -> Option<String> {
        let Ok(root) = std::env::current_dir() else {
            text("Project directory unavailable", true, false);
            return None;
        };
        if button("Refresh", 0., 0.) {
            self.directories.clear();
        }
        let mut selected = self.directory(&root, assets);
        if let Some(assets) = assets.filter(|path| !path.starts_with(&root)) {
            if unsafe { ki_project_folder(c"Assets".as_ptr(), true) } {
                selected = self.directory(assets, Some(assets)).or(selected);
                unsafe { ki_end_tree() };
            }
        }
        selected
    }

    fn directory(&mut self, path: &Path, assets: Option<&Path>) -> Option<String> {
        let entries = self
            .directories
            .entry(path.to_owned())
            .or_insert_with(|| list(path).ok().map(Rc::from))
            .clone();
        let Some(entries) = entries else {
            text("Could not read folder", true, false);
            return None;
        };
        let mut selected = None;
        for entry in entries.iter().filter(|entry| entry.directory) {
            let child = path.join(&entry.name);
            let default_open = assets.is_some_and(|root| root.starts_with(&child));
            let label = c_string(&entry.name);
            push_id(&entry.name);
            if unsafe { ki_project_folder(label.as_ptr(), default_open) } {
                selected = self.directory(&child, assets).or(selected);
                unsafe { ki_end_tree() };
            }
            unsafe { ki_pop_id() };
        }
        for entry in entries.iter().filter(|entry| !entry.directory) {
            let file = path.join(&entry.name);
            if let Some(relative) = asset_json(&file, assets) {
                let label = c_string(&entry.name);
                if unsafe { ki_project_file(label.as_ptr()) } {
                    selected = Some(relative);
                }
            } else {
                text(&entry.name, false, false);
            }
        }
        selected
    }
}

fn asset_json(file: &Path, assets: Option<&Path>) -> Option<String> {
    if !file.extension()?.eq_ignore_ascii_case("json") {
        return None;
    }
    let relative = file.strip_prefix(assets?).ok()?;
    let path = relative.to_str()?.replace('\\', "/");
    // The native bridge reserves this prefix for engine shaders.
    (!path.starts_with("shaders/")).then_some(path)
}

fn list(path: &Path) -> std::io::Result<Vec<Entry>> {
    let mut entries = Vec::new();
    for entry in fs::read_dir(path)?.flatten() {
        let Ok(name) = entry.file_name().into_string() else {
            continue;
        };
        if name.starts_with('.') || name == "target" || name == "vendor" {
            continue;
        }
        let Ok(kind) = entry.file_type() else {
            continue;
        };
        if kind.is_dir() || kind.is_file() {
            entries.push(Entry {
                name,
                directory: kind.is_dir(),
            });
        }
    }
    entries.sort_unstable_by(|a, b| b.directory.cmp(&a.directory).then(a.name.cmp(&b.name)));
    Ok(entries)
}
