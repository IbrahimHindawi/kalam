use rmpv::Value;
use std::{
    collections::HashMap,
    ffi::{c_char, c_void, CStr},
    io::{BufReader, Write},
    os::windows::process::CommandExt,
    process::{Child, ChildStdin, Command, Stdio},
    sync::{mpsc, Mutex},
};

#[repr(C)]
pub struct Cell {
    text: [u8; 32],
    len: u32,
    foreground: u32,
    background: u32,
}

#[repr(C)]
pub struct Frame {
    cells: *const Cell,
    columns: u32,
    rows: u32,
    cursor_column: u32,
    cursor_row: u32,
    background: u32,
}

#[derive(Clone, Default)]
struct GridCell {
    text: String,
    highlight: u32,
}

#[derive(Default)]
struct Highlight {
    foreground: Option<u32>,
    background: Option<u32>,
    reverse: bool,
}

struct Nvim {
    child: Child,
    input: ChildStdin,
    messages: mpsc::Receiver<Value>,
    next_id: u64,
    channel: u64,
    size: (usize, usize),
    cursor: (u32, u32),
    colors: (u32, u32),
    highlights: HashMap<u32, Highlight>,
    grid: Vec<GridCell>,
    published: Vec<Cell>,
    published_size: (u32, u32),
    published_cursor: (u32, u32),
    disconnected: bool,
}

static NVIM: Mutex<Option<Nvim>> = Mutex::new(None);
type Change = extern "C" fn(*mut c_void, *const u8, usize, *const u8, usize, i32);

impl Nvim {
    fn open(clean: bool) -> Option<Self> {
        const CREATE_NO_WINDOW: u32 = 0x0800_0000;
        let mut command = Command::new("nvim");
        command.arg("--embed");
        if clean {
            command.args(["-u", "NONE", "-i", "NONE"]);
        }
        let mut child = command
            .creation_flags(CREATE_NO_WINDOW)
            .stdin(Stdio::piped())
            .stdout(Stdio::piped())
            .stderr(Stdio::inherit())
            .spawn()
            .ok()?;
        let input = child.stdin.take()?;
        let output = child.stdout.take()?;
        let (sender, messages) = mpsc::sync_channel(128);
        std::thread::spawn(move || {
            let mut reader = BufReader::new(output);
            while let Ok(message) = rmpv::decode::read_value(&mut reader) {
                if sender.send(message).is_err() {
                    break;
                }
            }
        });
        let mut nvim = Self {
            child,
            input,
            messages,
            next_id: 1,
            channel: 0,
            size: (0, 0),
            cursor: (0, 0),
            colors: (0xe3dfd8, 0x242529),
            highlights: HashMap::new(),
            grid: Vec::new(),
            published: Vec::new(),
            published_size: (0, 0),
            published_cursor: (0, 0),
            disconnected: false,
        };
        nvim.request("nvim_get_api_info", vec![]);
        nvim.request(
            "nvim_ui_attach",
            vec![
                56.into(),
                25.into(),
                Value::Map(vec![
                    ("rgb".into(), true.into()),
                    ("ext_linegrid".into(), true.into()),
                ]),
            ],
        );
        Some(nvim)
    }

    fn request(&mut self, method: &str, args: Vec<Value>) {
        if self.disconnected {
            return;
        }
        let mut bytes = Vec::new();
        let message = Value::Array(vec![
            0.into(),
            self.next_id.into(),
            method.into(),
            Value::Array(args),
        ]);
        self.next_id += 1;
        if rmpv::encode::write_value(&mut bytes, &message).is_err()
            || self.input.write_all(&bytes).is_err()
        {
            self.disconnected = true;
        }
    }

    fn poll(&mut self, callback: Change, context: *mut c_void) {
        for _ in 0..256 {
            let message = match self.messages.try_recv() {
                Ok(message) => message,
                Err(mpsc::TryRecvError::Empty) => return,
                Err(mpsc::TryRecvError::Disconnected) => {
                    self.disconnected = true;
                    return;
                }
            };
            let packet = array(&message);
            match integer(packet, 0) {
                1 if integer(packet, 1) == 1 => {
                    self.channel = integer(sequence(packet, 3), 0) as u64
                }
                1 if packet.get(2).is_some_and(|error| !error.is_nil()) => {
                    eprintln!("Neovim RPC: {}", packet[2]);
                }
                2 => match string(packet, 1) {
                    "redraw" => self.redraw(sequence(packet, 2)),
                    "kalam.document" => {
                        let args = sequence(packet, 2);
                        let (path, text) = (string(args, 0), string(args, 1));
                        let saved = args.get(2).and_then(Value::as_bool).unwrap_or(false);
                        if !path.is_empty() {
                            callback(
                                context,
                                path.as_ptr(),
                                path.len(),
                                text.as_ptr(),
                                text.len(),
                                saved.into(),
                            );
                        }
                    }
                    _ => {}
                },
                _ => {}
            }
        }
    }

    fn redraw(&mut self, events: &[Value]) {
        for event in events {
            let fields = array(event);
            let name = string(fields, 0);
            if name == "flush" {
                self.publish();
                continue;
            }
            for args in fields.iter().skip(1).map(array) {
                match name {
                    "grid_resize" if integer(args, 0) == 1 => {
                        self.size = (
                            integer(args, 1).clamp(1, 512) as usize,
                            integer(args, 2).clamp(1, 256) as usize,
                        );
                        self.grid = vec![GridCell::default(); self.size.0 * self.size.1];
                    }
                    "grid_clear" if integer(args, 0) == 1 => self.grid.fill(GridCell::default()),
                    "grid_cursor_goto" if integer(args, 0) == 1 => {
                        self.cursor = (integer(args, 2) as u32, integer(args, 1) as u32);
                    }
                    "grid_line" if integer(args, 0) == 1 => self.line(args),
                    "grid_scroll" if integer(args, 0) == 1 => self.scroll(args),
                    "default_colors_set" => {
                        self.colors = (
                            rgb(integer(args, 0)).unwrap_or(0xe3dfd8),
                            rgb(integer(args, 1)).unwrap_or(0x242529),
                        )
                    }
                    "hl_attr_define" => {
                        let mut attr = Highlight::default();
                        if let Some(map) = args.get(1).and_then(Value::as_map) {
                            for (key, val) in map {
                                match key.as_str().unwrap_or_default() {
                                    "foreground" => attr.foreground = val.as_i64().and_then(rgb),
                                    "background" => attr.background = val.as_i64().and_then(rgb),
                                    "reverse" => attr.reverse = val.as_bool().unwrap_or(false),
                                    _ => {}
                                }
                            }
                        }
                        self.highlights.insert(integer(args, 0) as u32, attr);
                    }
                    _ => {}
                }
            }
        }
    }

    fn line(&mut self, args: &[Value]) {
        let row = integer(args, 1) as usize;
        let mut column = integer(args, 2) as usize;
        let mut highlight = 0;
        if row >= self.size.1 {
            return;
        }
        for item in sequence(args, 3) {
            let parts = array(item);
            if parts.len() >= 2 {
                highlight = integer(parts, 1) as u32;
            }
            let repeat = parts.get(2).and_then(Value::as_u64).unwrap_or(1);
            for _ in 0..repeat {
                if column >= self.size.0 {
                    return;
                }
                let cell = &mut self.grid[row * self.size.0 + column];
                cell.text.clear();
                cell.text.push_str(string(parts, 0));
                cell.highlight = highlight;
                column += 1;
            }
        }
    }

    fn scroll(&mut self, args: &[Value]) {
        let top = integer(args, 1).clamp(0, self.size.1 as i64);
        let bottom = integer(args, 2).clamp(top, self.size.1 as i64);
        let left = integer(args, 3).clamp(0, self.size.0 as i64);
        let right = integer(args, 4).clamp(left, self.size.0 as i64);
        let (dy, dx) = (integer(args, 5), integer(args, 6));
        let old = self.grid.clone();
        for row in top..bottom {
            for col in left..right {
                let (y, x) = (row.saturating_add(dy), col.saturating_add(dx));
                let dst = row as usize * self.size.0 + col as usize;
                self.grid[dst] = if (top..bottom).contains(&y) && (left..right).contains(&x) {
                    old[y as usize * self.size.0 + x as usize].clone()
                } else {
                    GridCell::default()
                };
            }
        }
    }

    fn publish(&mut self) {
        self.published.clear();
        for cell in &self.grid {
            let (mut foreground, mut background) = self.colors;
            if let Some(attr) = self.highlights.get(&cell.highlight) {
                foreground = attr.foreground.unwrap_or(foreground);
                background = attr.background.unwrap_or(background);
                if attr.reverse {
                    std::mem::swap(&mut foreground, &mut background);
                }
            }
            let mut text = [0; 32];
            let mut len = cell.text.len().min(text.len());
            while !cell.text.is_char_boundary(len) {
                len -= 1;
            }
            text[..len].copy_from_slice(&cell.text.as_bytes()[..len]);
            self.published.push(Cell {
                text,
                len: len as u32,
                foreground,
                background,
            });
        }
        self.published_size = (self.size.0 as u32, self.size.1 as u32);
        self.published_cursor = self.cursor;
    }
}

impl Drop for Nvim {
    fn drop(&mut self) {
        let _ = self.child.kill();
        let _ = self.child.wait();
    }
}

fn array(value: &Value) -> &[Value] {
    value.as_array().map(Vec::as_slice).unwrap_or_default()
}
fn sequence(value: &[Value], index: usize) -> &[Value] {
    value.get(index).map(array).unwrap_or_default()
}
fn integer(value: &[Value], index: usize) -> i64 {
    value.get(index).and_then(Value::as_i64).unwrap_or_default()
}
fn string(value: &[Value], index: usize) -> &str {
    value.get(index).and_then(Value::as_str).unwrap_or_default()
}
fn rgb(number: i64) -> Option<u32> {
    (0..=0xffffff).contains(&number).then_some(number as u32)
}

#[no_mangle]
pub extern "C" fn kh_nvim_start(clean: i32) -> i32 {
    let mut state = NVIM.lock().unwrap();
    *state = Nvim::open(clean != 0);
    state.is_some().into()
}

#[no_mangle]
pub extern "C" fn kh_nvim_stop() {
    *NVIM.lock().unwrap() = None;
}

#[no_mangle]
pub extern "C" fn kh_nvim_poll(callback: Change, context: *mut c_void) -> i32 {
    let mut state = NVIM.lock().unwrap();
    let Some(nvim) = &mut *state else { return -1 };
    nvim.poll(callback, context);
    if nvim.disconnected {
        -1
    } else {
        (nvim.channel != 0).into()
    }
}

#[no_mangle]
pub extern "C" fn kh_nvim_resize(columns: u32, rows: u32) {
    if let Some(nvim) = &mut *NVIM.lock().unwrap() {
        nvim.request(
            "nvim_ui_try_resize",
            vec![columns.clamp(12, 512).into(), rows.clamp(4, 256).into()],
        );
    }
}

#[no_mangle]
pub unsafe extern "C" fn kh_nvim_input(keys: *const c_char) {
    if let Some(nvim) = &mut *NVIM.lock().unwrap() {
        let keys = unsafe { CStr::from_ptr(keys) }.to_string_lossy();
        nvim.request("nvim_input", vec![keys.as_ref().into()]);
    }
}

#[no_mangle]
pub unsafe extern "C" fn kh_nvim_mouse(
    button: *const c_char,
    action: *const c_char,
    row: u32,
    col: u32,
) {
    if let Some(nvim) = &mut *NVIM.lock().unwrap() {
        let button = unsafe { CStr::from_ptr(button) }.to_string_lossy();
        let action = unsafe { CStr::from_ptr(action) }.to_string_lossy();
        nvim.request(
            "nvim_input_mouse",
            vec![
                button.as_ref().into(),
                action.as_ref().into(),
                "".into(),
                1.into(),
                row.into(),
                col.into(),
            ],
        );
    }
}

const OPEN_DOCUMENT: &str = r#"
local channel, path, text, saved, focus = ...
vim.cmd('filetype on')
if not vim.g.syntax_on then vim.cmd('syntax enable') end
local buffer = vim.fn.bufadd(path)
vim.bo[buffer].swapfile = false
vim.fn.bufload(buffer)
if focus then vim.cmd('hide buffer ' .. buffer) end
if vim.bo[buffer].filetype == '' then vim.bo[buffer].filetype = 'json' end
if vim.bo[buffer].filetype == 'json' and vim.bo[buffer].syntax == '' then
  vim.bo[buffer].syntax = 'json'
end
if not vim.g.colors_name or vim.g.colors_name == 'default' then
  vim.api.nvim_set_hl(0, 'jsonKeyword', {fg = '#e6b180', bold = true})
  vim.api.nvim_set_hl(0, 'jsonString', {fg = '#b5cca1'})
  vim.api.nvim_set_hl(0, 'jsonNumber', {fg = '#a8c6e8'})
  vim.api.nvim_set_hl(0, 'jsonBoolean', {fg = '#cbb4e5'})
  vim.api.nvim_set_hl(0, 'jsonNull', {fg = '#cbb4e5'})
end
local function contents()
  local lines = vim.api.nvim_buf_get_lines(buffer, 0, -1, false)
  local newline = vim.bo[buffer].fileformat == 'dos' and '\r\n' or '\n'
  return table.concat(lines, newline) .. (vim.bo[buffer].endofline and newline or '')
end
if contents() ~= text then
  local clean = text:gsub('\r\n', '\n')
  vim.api.nvim_buf_set_lines(buffer, 0, -1, false,
    vim.split(clean:gsub('\n$', ''), '\n', {plain = true}))
  vim.bo[buffer].endofline = clean:sub(-1) == '\n'
  vim.bo[buffer].fileformat = text:find('\r\n', 1, true) and 'dos' or 'unix'
end
vim.bo[buffer].modified = not saved
if not vim.b[buffer].kalam_native then
  vim.b[buffer].kalam_native = true
  vim.api.nvim_create_autocmd({'TextChanged', 'TextChangedI', 'TextChangedP', 'BufWritePost'}, {
    buffer = buffer,
    callback = function(event)
      vim.rpcnotify(channel, 'kalam.document', path, contents(), event.event == 'BufWritePost')
    end,
  })
end
"#;

#[no_mangle]
pub unsafe extern "C" fn kh_nvim_document(
    path: *const c_char,
    text: *const u8,
    size: usize,
    saved: i32,
    focus: i32,
) {
    let path = unsafe { CStr::from_ptr(path) }.to_string_lossy();
    let text = String::from_utf8_lossy(unsafe { std::slice::from_raw_parts(text, size) });
    if let Some(nvim) = &mut *NVIM.lock().unwrap() {
        let args = vec![
            nvim.channel.into(),
            path.as_ref().into(),
            text.as_ref().into(),
            (saved != 0).into(),
            (focus != 0).into(),
        ];
        nvim.request(
            "nvim_exec_lua",
            vec![OPEN_DOCUMENT.into(), Value::Array(args)],
        );
    }
}

#[no_mangle]
pub unsafe extern "C" fn kh_nvim_frame(frame: *mut Frame) -> i32 {
    let state = NVIM.lock().unwrap();
    let Some(nvim) = &*state else { return 0 };
    if nvim.published.is_empty() {
        return 0;
    }
    unsafe {
        *frame = Frame {
            cells: nvim.published.as_ptr(),
            columns: nvim.published_size.0,
            rows: nvim.published_size.1,
            cursor_column: nvim.published_cursor.0,
            cursor_row: nvim.published_cursor.1,
            background: nvim.colors.1,
        };
    }
    1
}
