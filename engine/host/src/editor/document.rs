use super::ui::{self, *};
use serde_json::{Number, Value};

pub enum Cache {
    Empty,
    Invalid,
    Form(Value),
    Text { lines: Vec<Line>, width: f32 },
}

pub struct Document {
    pub text: String,
    pub saved: String,
    pub cache: Cache,
}

impl Document {
    pub fn new(text: String) -> Self {
        Self {
            saved: text.clone(),
            text,
            cache: Cache::Empty,
        }
    }

    pub fn dirty(&self) -> bool {
        self.text != self.saved
    }

    pub fn replace(&mut self, text: String) {
        self.text = text;
        self.cache = Cache::Empty;
    }

    pub fn form(&mut self) -> Option<String> {
        if matches!(self.cache, Cache::Empty | Cache::Text { .. }) {
            self.cache = serde_json::from_str(&self.text).map_or(Cache::Invalid, Cache::Form);
        }
        let Cache::Form(root) = &mut self.cache else {
            ui::text("Fix the JSON file on disk and reload it.", false, true);
            return None;
        };
        let mut changed = false;
        if unsafe { ki_begin_child(c"Fields".as_ptr(), false) } {
            if let Value::Object(fields) = root {
                for (label, value) in fields {
                    changed |= field(label, value);
                }
            } else {
                changed = field("Value", root);
            }
        }
        unsafe { ki_end_child() };
        if !changed {
            return None;
        }
        let Some(next) = encode(root) else {
            self.cache = Cache::Empty;
            return None;
        };
        if next == self.text {
            return None;
        }
        Some(std::mem::replace(&mut self.text, next))
    }

    pub fn preview(&mut self) -> bool {
        let copied = !button("Copy JSON", 0., 0.) || ui::copy(&self.text);
        unsafe { ki_code_font(true) };
        if !matches!(self.cache, Cache::Text { .. }) {
            let lines = colorize(&self.text);
            let width = lines
                .iter()
                .map(|line| measure(&self.text[line.begin..line.end]))
                .fold(0., f32::max);
            self.cache = Cache::Text { lines, width };
        }
        let Cache::Text { lines, width } = &self.cache else {
            unreachable!()
        };
        unsafe { ki_next_content_width(*width) };
        if unsafe { ki_begin_child(c"##json-preview".as_ptr(), true) } {
            let metrics = Metrics::get();
            let height = metrics.line_height();
            let start = (metrics.scroll_y() / height).floor().max(0.) as usize;
            let end = (start + (metrics.available_height().max(0.) / height).ceil() as usize + 2)
                .min(lines.len());
            if let Some(draw) = DrawList::get(false) {
                let visible_start = start.min(end);
                let origin = metrics.cursor();
                for (row, line) in lines.iter().enumerate().take(end).skip(visible_start) {
                    let mut x = origin.x;
                    let mut previous = line.begin;
                    for token in &line.tokens {
                        x += measure(&self.text[previous..token.start]);
                        if x >= metrics.clip_right() {
                            break;
                        }
                        let value = &self.text[token.start..token.end];
                        draw.text(
                            glam::Vec2::new(x, origin.y + row as f32 * height),
                            token.color,
                            value,
                        );
                        x += measure(value);
                        previous = token.end;
                    }
                }
            }
            if !lines.is_empty() {
                unsafe { ki_dummy(0., (lines.len() - 1) as f32 * height + metrics.font()) };
            }
        }
        unsafe {
            ki_end_child();
            ki_code_font(false);
        }
        copied
    }
}

pub fn encode(value: &Value) -> Option<String> {
    let mut text = serde_json::to_string_pretty(value).ok()?;
    text.push('\n');
    (text.len() <= super::MAX_DOCUMENT).then_some(text)
}

pub fn inline(value: &Value) -> bool {
    value.get("type").is_some()
        && ["value", "default", "min", "max"]
            .iter()
            .any(|key| value.get(*key).is_some())
}

pub fn set_value(target: &mut Value, value: Value) {
    if inline(target) {
        target["value"] = value;
    } else {
        *target = value;
    }
}

fn label_field(label: &str) {
    let metrics = Metrics::get();
    let label_width = (metrics.available_width() * 0.39).clamp(82., 150.);
    ui::text(label, false, false);
    if measure(label) + 6. < label_width {
        unsafe { ki_same_line_at(metrics.cursor_x() + label_width) };
    }
    unsafe { ki_next_width(-f32::MIN_POSITIVE) };
}

extern "C" {
    fn ki_same_line_at(x: f32);
}

fn field(label: &str, value: &mut Value) -> bool {
    push_id(label);
    let changed = field_body(label, value);
    unsafe { ki_pop_id() };
    changed
}

fn field_body(label: &str, value: &mut Value) -> bool {
    if inline(value) {
        label_field(label);
        let kind = value["type"].as_str().unwrap_or("");
        let Some(input) = value.get("value").or_else(|| value.get("default")) else {
            return false;
        };
        let bounds = (
            value.get("min").and_then(Value::as_f64),
            value.get("max").and_then(Value::as_f64),
        );
        let next = match kind {
            "integer" => edit_int(input, bounds),
            "number" => edit_numbers(input, 1, bounds),
            "vec2" => edit_numbers(input, 2, bounds),
            "vec3" => edit_numbers(input, 3, bounds),
            "vec4" => edit_numbers(input, 4, bounds),
            "boolean" => edit_bool(input),
            "string" => input.as_str().and_then(input_string).map(Value::String),
            _ => None,
        };
        if let Some(next) = next {
            set_value(value, next);
            return true;
        }
        return false;
    }
    let numbers = value
        .as_array()
        .filter(|list| (2..=4).contains(&list.len()) && list.iter().all(Value::is_number))
        .map(Vec::len);
    let next = match value {
        Value::Array(_) if numbers.is_some() => {
            label_field(label);
            edit_numbers(value, numbers.unwrap(), (None, None))
        }
        Value::Object(map) => {
            if !tree(label) {
                return false;
            }
            let mut changed = false;
            for (key, value) in map {
                changed |= field(key, value);
            }
            unsafe { ki_end_tree() };
            return changed;
        }
        Value::Array(list) => {
            if !tree(label) {
                return false;
            }
            let mut changed = false;
            for (index, value) in list.iter_mut().enumerate() {
                changed |= field(&format!("[{index}]"), value);
            }
            unsafe { ki_end_tree() };
            return changed;
        }
        Value::Number(n) if label == "version" => {
            label_field(label);
            ui::text(&n.to_string(), true, false);
            None
        }
        Value::Number(_) => {
            label_field(label);
            if matches!(label, "max_bounces" | "chasers" | "turrets") {
                edit_int(value, (Some(0.), None))
            } else {
                edit_numbers(value, 1, (None, None))
            }
        }
        Value::Bool(_) => {
            label_field(label);
            edit_bool(value)
        }
        Value::String(text) => {
            label_field(label);
            input_string(text).map(Value::String)
        }
        Value::Null => {
            ui::text(&format!("{label}   null"), true, false);
            None
        }
    };
    if let Some(next) = next {
        *value = next;
        true
    } else {
        false
    }
}

type Bounds = (Option<f64>, Option<f64>);
fn edit_int(value: &Value, (lo, hi): Bounds) -> Option<Value> {
    let mut number = i32::try_from(value.as_i64()?).ok()?;
    let min = lo.map_or(i32::MIN, |n| n as i32);
    let max = hi.map_or(i32::MAX, |n| n as i32);
    if min > max {
        return None;
    }
    unsafe { ki_drag_int(&mut number, min, max, lo.is_some() || hi.is_some()) }
        .then(|| Value::from(number.clamp(min, max)))
}
fn edit_bool(value: &Value) -> Option<Value> {
    let mut number = value.as_bool()?;
    unsafe { ki_bool(&mut number) }.then_some(Value::Bool(number))
}
fn edit_numbers(value: &Value, count: usize, (lo, hi): Bounds) -> Option<Value> {
    let mut numbers = [0.; 4];
    if count == 1 {
        numbers[0] = value.as_f64()?;
    } else {
        let items = value.as_array().filter(|items| items.len() == count)?;
        for (out, item) in numbers.iter_mut().zip(items) {
            *out = item.as_f64()?;
        }
    }
    let min = lo.unwrap_or(f64::MIN);
    let max = hi.unwrap_or(f64::MAX);
    if min > max
        || !unsafe {
            ki_drag_float(
                numbers.as_mut_ptr(),
                count as i32,
                min,
                max,
                lo.is_some() || hi.is_some(),
            )
        }
    {
        return None;
    }
    let values = numbers[..count]
        .iter()
        .map(|&n| Number::from_f64(n.clamp(min, max)).map(Value::Number));
    if count == 1 {
        values.into_iter().next().flatten()
    } else {
        values.collect::<Option<Vec<_>>>().map(Value::Array)
    }
}

struct Token {
    start: usize,
    end: usize,
    color: u32,
}
pub struct Line {
    begin: usize,
    end: usize,
    tokens: Vec<Token>,
}

fn colorize(text: &str) -> Vec<Line> {
    let mut lines = Vec::new();
    let mut at = 0;
    for source in text.split_inclusive('\n') {
        let begin = at;
        let end = begin + source.trim_end_matches(['\n', '\r']).len();
        let mut tokens = Vec::new();
        while at < end {
            let start = at;
            let ch = text.as_bytes()[at];
            at += text[at..].chars().next().map_or(1, char::len_utf8);
            let color = match ch {
                b' ' | b'\t' => continue,
                b'"' => {
                    let mut escaped = false;
                    while at < end {
                        let c = text[at..].chars().next().unwrap();
                        at += c.len_utf8();
                        if c == '"' && !escaped {
                            break;
                        }
                        escaped = c == '\\' && !escaped;
                    }
                    if text[at..].trim_start().starts_with(':') {
                        rgba(141, 202, 243, 255)
                    } else {
                        rgba(199, 225, 165, 255)
                    }
                }
                b'-' | b'0'..=b'9' => {
                    while at < end && b"0123456789.+-eE".contains(&text.as_bytes()[at]) {
                        at += 1;
                    }
                    rgba(245, 198, 137, 255)
                }
                b'a'..=b'z' => {
                    while at < end && text.as_bytes()[at].is_ascii_lowercase() {
                        at += 1;
                    }
                    rgba(210, 174, 238, 255)
                }
                _ => rgba(170, 177, 189, 255),
            };
            tokens.push(Token {
                start,
                end: at,
                color,
            });
        }
        lines.push(Line { begin, end, tokens });
        at = begin + source.len();
    }
    lines
}
