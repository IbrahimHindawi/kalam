use font8x8::{UnicodeFonts, BASIC_FONTS};
use kalam_runtime::{
    lens::{Binding, Descriptor},
    scene::Scene,
};

pub const WIDTH: u32 = 360;
const MARGIN: u32 = 16;
const HEADER: u32 = 52;
const ROW: u32 = 58;
const TRACK_X: u32 = 20;
const TRACK_W: u32 = WIDTH - TRACK_X * 2;

pub struct Panel {
    pub descriptor: Descriptor,
    drag: Option<Binding>,
}

#[derive(Clone, Copy)]
pub struct Rect {
    pub x: u32,
    pub y: u32,
    pub w: u32,
    pub h: u32,
}

impl Panel {
    pub fn new(descriptor: Descriptor) -> Self {
        Self {
            descriptor,
            drag: None,
        }
    }

    pub fn height(&self, scene: &Scene) -> u32 {
        HEADER + self.visible(scene).count() as u32 * ROW + MARGIN
    }

    pub fn rect(&self, scene: &Scene, w: u32, h: u32) -> Option<Rect> {
        let max_w = w.saturating_sub(MARGIN * 2);
        let max_h = h.saturating_sub(MARGIN * 2);
        let source_h = self.height(scene);
        let fit_w = WIDTH.min(max_w).min(max_h.saturating_mul(WIDTH) / source_h);
        (fit_w >= 140).then_some(Rect {
            x: MARGIN,
            y: MARGIN,
            w: fit_w,
            h: source_h * fit_w / WIDTH,
        })
    }

    fn visible<'a>(&'a self, scene: &'a Scene) -> impl Iterator<Item = (&'a str, Binding, f32)> {
        self.descriptor.sliders.iter().filter_map(|slider| {
            Some((slider.label.as_str(), slider.bind, slider.bind.read(scene)?))
        })
    }

    pub fn input(
        &mut self,
        scene: &mut Scene,
        rect: Rect,
        x: f32,
        y: f32,
        pressed: bool,
        held: bool,
    ) -> bool {
        let x = (x - rect.x as f32) * WIDTH as f32 / rect.w as f32;
        let y = (y - rect.y as f32) * self.height(scene) as f32 / rect.h as f32;
        if pressed {
            self.drag = None;
            if x >= TRACK_X as f32 - 8.
                && x <= (TRACK_X + TRACK_W) as f32 + 8.
                && y >= HEADER as f32
                && y < self.height(scene) as f32
            {
                let row = (y as u32 - HEADER) / ROW;
                let binding = self
                    .visible(scene)
                    .nth(row as usize)
                    .map(|(_, binding, _)| binding);
                self.drag = binding;
            }
        }
        if !held && !pressed {
            self.drag = None;
        }
        let Some(binding) = self.drag else {
            return false;
        };
        let t = ((x - TRACK_X as f32) / TRACK_W as f32).clamp(0., 1.);
        let (lo, hi) = binding.range(scene);
        let value = (lo + t * (hi - lo)).clamp(lo, hi);
        binding.read(scene) != Some(value) && binding.write(scene, value).is_ok()
    }

    pub fn pixels(&self, scene: &Scene) -> Vec<u8> {
        let height = self.height(scene);
        let mut image = Image(vec![255; (WIDTH * height * 4) as usize]);
        image.fill(0, 0, WIDTH, height, [17, 24, 39, 255]);
        image.fill(0, 0, WIDTH, 42, [29, 43, 64, 255]);
        image.text(20, 13, &self.descriptor.title, [237, 243, 251, 255]);
        image.text(314, 13, "F1", [139, 169, 200, 255]);
        for (row, (label, binding, value)) in self.visible(scene).enumerate() {
            let y = HEADER + row as u32 * ROW;
            image.text(20, y + 2, label, [225, 235, 246, 255]);
            let value_text = format!("{value:.2}");
            image.text(
                WIDTH - 20 - value_text.len() as u32 * 9,
                y + 2,
                &value_text,
                [120, 225, 192, 255],
            );
            let (lo, hi) = binding.range(scene);
            let thumb = if lo == hi {
                0
            } else {
                (((value - lo) / (hi - lo)) * TRACK_W as f32) as u32
            };
            image.fill(TRACK_X, y + 27, TRACK_W, 5, [59, 79, 101, 255]);
            image.fill(TRACK_X, y + 27, thumb, 5, [77, 198, 161, 255]);
            image.fill(TRACK_X + thumb - 4, y + 22, 8, 15, [179, 251, 224, 255]);
        }
        image.0
    }
}

pub struct Image(pub Vec<u8>);

impl Image {
    pub fn fill(&mut self, x: u32, y: u32, w: u32, h: u32, color: [u8; 4]) {
        for y in y..y + h {
            let start = ((y * WIDTH + x) * 4) as usize;
            for pixel in self.0[start..start + (w * 4) as usize].chunks_exact_mut(4) {
                pixel.copy_from_slice(&color);
            }
        }
    }

    pub fn text(&mut self, x: u32, y: u32, text: &str, color: [u8; 4]) {
        for (column, glyph) in text
            .chars()
            .filter_map(|ch| BASIC_FONTS.get(ch))
            .enumerate()
        {
            for (dy, bits) in glyph.into_iter().enumerate() {
                for dx in 0..8 {
                    if bits & (1 << dx) != 0 {
                        self.fill(x + column as u32 * 9 + dx, y + dy as u32 * 2, 1, 2, color);
                    }
                }
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use kalam_runtime::content;

    #[test]
    fn scaled_panel_hit_test_applies_the_bound_edit() {
        let mut scene = content::load_scene("room", |path, _| {
            std::fs::read(
                std::path::Path::new(env!("CARGO_MANIFEST_DIR"))
                    .join("../assets")
                    .join(path),
            )
            .map_err(|error| format!("{path}: {error}"))
        })
        .unwrap()
        .scene;
        let mut panel = Panel::new(
            Descriptor::parse(include_bytes!("../../assets/lenses/inspect.json")).unwrap(),
        );
        let rect = panel.rect(&scene, 256, 220).unwrap();
        let x = rect.x as f32 + rect.w as f32 * 0.5;
        let y = rect.y as f32 + rect.h as f32 * (HEADER + 30) as f32 / panel.height(&scene) as f32;
        assert!(panel.input(&mut scene, rect, x, y, true, true));
        assert_eq!(Binding::WalkSpeed.read(&scene), Some(10.05));
        assert!(!panel.input(&mut scene, rect, x + 20., y, false, false));
        assert_eq!(
            panel.pixels(&scene).len(),
            (WIDTH * panel.height(&scene) * 4) as usize
        );
        drop(panel);
        assert_eq!(Binding::WalkSpeed.read(&scene), Some(10.05));
    }
}
