//! Layout for RGB multi-channel signed-distance-field atlases from msdf-atlas-gen.
use serde::Deserialize;
use std::collections::HashMap;

/// Positions in logical pixels and UVs in normalized top-down texture coordinates.
/// Each rectangle is [left, top, right, bottom].
#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct GlyphQuad {
    pub rect: [f32; 4],
    pub uv: [f32; 4],
    pub color: [u8; 4],
}

impl GlyphQuad {
    pub fn clipped(self, bounds: [f32; 4]) -> Option<Self> {
        let [l, t, r, b] = self.rect;
        let rect = [
            l.max(bounds[0]),
            t.max(bounds[1]),
            r.min(bounds[2]),
            b.min(bounds[3]),
        ];
        if rect[0] >= rect[2] || rect[1] >= rect[3] {
            return None;
        }
        if rect == self.rect {
            return Some(self);
        }
        let [u, v, u1, v1] = self.uv;
        let (du, dv) = ((u1 - u) / (r - l), (v1 - v) / (b - t));
        Some(Self {
            rect,
            uv: [
                u + (rect[0] - l) * du,
                v + (rect[1] - t) * dv,
                u + (rect[2] - l) * du,
                v + (rect[3] - t) * dv,
            ],
            ..self
        })
    }
}

#[derive(Deserialize)]
#[serde(rename_all = "camelCase")]
struct Atlas {
    width: u32,
    height: u32,
    distance_range: f32,
    y_origin: String,
    #[serde(rename = "type")]
    kind: String,
    #[serde(default)]
    distance_range_middle: f32,
}
#[derive(Deserialize)]
#[serde(rename_all = "camelCase")]
struct Metrics {
    ascender: f32,
    line_height: f32,
    em_size: f32,
}
#[derive(Deserialize)]
struct Bounds {
    left: f32,
    top: f32,
    right: f32,
    bottom: f32,
}
#[derive(Deserialize)]
#[serde(rename_all = "camelCase")]
struct Entry {
    unicode: u32,
    advance: f32,
    plane_bounds: Option<Bounds>,
    atlas_bounds: Option<Bounds>,
}
#[derive(Deserialize)]
struct Pair {
    unicode1: u32,
    unicode2: u32,
    advance: f32,
}
#[derive(Deserialize)]
struct Descriptor {
    atlas: Atlas,
    metrics: Metrics,
    glyphs: Vec<Entry>,
    #[serde(default)]
    kerning: Vec<Pair>,
}

fn codepoint(code: u32) -> Result<char, String> {
    char::from_u32(code).ok_or_else(|| format!("Invalid MSDF Unicode scalar {code}"))
}

struct Glyph {
    advance: f32,
    ink: Option<([f32; 4], [f32; 4])>,
}
pub struct Font {
    atlas_size: [u32; 2],
    unit_range: [f32; 2],
    ascent: f32,
    line_height: f32,
    glyphs: HashMap<char, Glyph>,
    pairs: HashMap<(char, char), f32>,
}

impl Font {
    pub fn parse(json: &[u8]) -> Result<Self, String> {
        let Descriptor {
            atlas,
            metrics,
            glyphs,
            kerning,
        } = serde_json::from_slice(json).map_err(|e| format!("MSDF font metadata: {e}"))?;
        let [w, h] = [atlas.width, atlas.height];
        if !matches!(atlas.kind.as_str(), "msdf" | "mtsdf")
            || w == 0
            || h == 0
            || !(atlas.distance_range.is_finite() && atlas.distance_range > 0.)
            || atlas.distance_range_middle != 0.
        {
            return Err(
                "MSDF needs nonempty atlas dimensions and a positive, symmetric distance range"
                    .into(),
            );
        }
        let top = match atlas.y_origin.as_str() {
            "top" => true,
            "bottom" => false,
            other => return Err(format!("Unknown MSDF atlas yOrigin {other}")),
        };
        let em = metrics.em_size;
        if !(em.is_finite()
            && em > 0.
            && metrics.line_height.is_finite()
            && metrics.line_height > 0.
            && metrics.ascender.is_finite())
        {
            return Err("Invalid MSDF font metrics".into());
        }
        let direction = if top { 1. } else { -1. };
        let mut font = Self {
            atlas_size: [w, h],
            unit_range: [
                atlas.distance_range / w as f32,
                atlas.distance_range / h as f32,
            ],
            ascent: -direction * metrics.ascender / em,
            line_height: metrics.line_height / em,
            glyphs: HashMap::with_capacity(glyphs.len()),
            pairs: HashMap::with_capacity(kerning.len()),
        };
        for glyph in glyphs {
            let ch = codepoint(glyph.unicode)?;
            let ink = match (glyph.plane_bounds, glyph.atlas_bounds) {
                (Some(p), Some(a)) => {
                    let plane = [
                        p.left / em,
                        direction * p.top / em,
                        p.right / em,
                        direction * p.bottom / em,
                    ];
                    let ay = |y: f32| if top { y / h as f32 } else { 1. - y / h as f32 };
                    let uv = [
                        a.left / w as f32,
                        ay(a.top),
                        a.right / w as f32,
                        ay(a.bottom),
                    ];
                    if ![plane, uv]
                        .iter()
                        .all(|r| r.iter().all(|n| n.is_finite()) && r[0] < r[2] && r[1] < r[3])
                        || !uv.iter().all(|u| (0.0..=1.0).contains(u))
                    {
                        return Err(format!("Invalid bounds for glyph {ch:?}"));
                    }
                    Some((plane, uv))
                }
                (None, None) => None,
                _ => return Err(format!("Incomplete bounds for glyph {ch:?}")),
            };
            if !glyph.advance.is_finite()
                || font
                    .glyphs
                    .insert(
                        ch,
                        Glyph {
                            advance: glyph.advance / em,
                            ink,
                        },
                    )
                    .is_some()
            {
                return Err(format!("Invalid advance or duplicate glyph {ch:?}"));
            }
        }
        if font.glyphs.is_empty() {
            return Err("Empty MSDF font".into());
        }
        for pair in kerning {
            if !pair.advance.is_finite() {
                return Err("Invalid MSDF kerning".into());
            }
            font.pairs.insert(
                (codepoint(pair.unicode1)?, codepoint(pair.unicode2)?),
                pair.advance / em,
            );
        }
        Ok(font)
    }

    pub fn atlas_size(&self) -> [u32; 2] {
        self.atlas_size
    }
    pub fn unit_range(&self) -> [f32; 2] {
        self.unit_range
    }

    /// Maximum line advance in logical pixels, including whitespace and kerning.
    pub fn measure(&self, text: &str, size: f32) -> f32 {
        self.walk(text, size, f32::INFINITY, |_, _, _| {})
    }

    /// Append quads using the top of a line box as the origin. Trailing glyphs
    /// that do not fit are omitted. Spaces and kerning still affect positioning.
    pub fn layout(
        &self,
        out: &mut Vec<GlyphQuad>,
        text: &str,
        origin: [f32; 2],
        size: f32,
        max_width: f32,
        color: [u8; 4],
    ) {
        if !origin.iter().all(|v| v.is_finite()) {
            return;
        }
        self.walk(text, size, max_width, |glyph, x, y| {
            if let Some((p, uv)) = glyph.ink {
                let left = origin[0] + x;
                let baseline = origin[1] + y + self.ascent * size;
                let q = GlyphQuad {
                    rect: [
                        left + p[0] * size,
                        baseline + p[1] * size,
                        left + p[2] * size,
                        baseline + p[3] * size,
                    ],
                    uv,
                    color,
                };
                if let Some(q) = q.clipped([
                    f32::NEG_INFINITY,
                    f32::NEG_INFINITY,
                    origin[0] + max_width,
                    f32::INFINITY,
                ]) {
                    out.push(q);
                }
            }
        });
    }

    fn walk(
        &self,
        text: &str,
        size: f32,
        limit: f32,
        mut visit: impl FnMut(&Glyph, f32, f32),
    ) -> f32 {
        if !size.is_finite() || size <= 0. || limit.is_nan() || limit <= 0. {
            return 0.;
        }
        let mut longest = 0f32;
        for (line_no, line) in text.split('\n').enumerate() {
            let mut pen = 0.;
            let mut previous = None;
            for ch in line.trim_end_matches('\r').chars() {
                let Some((&key, glyph)) = self
                    .glyphs
                    .get_key_value(&ch)
                    .or_else(|| self.glyphs.get_key_value(&'\u{fffd}'))
                    .or_else(|| self.glyphs.get_key_value(&'?'))
                else {
                    previous = None;
                    continue;
                };
                let kern = previous
                    .and_then(|p| self.pairs.get(&(p, key)))
                    .copied()
                    .unwrap_or(0.);
                let left = pen + kern * size;
                let next = left + glyph.advance * size;
                if next > limit {
                    break;
                }
                visit(glyph, left, line_no as f32 * self.line_height * size);
                pen = next;
                previous = Some(key);
            }
            longest = longest.max(pen);
        }
        longest
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    fn data(top: bool) -> serde_json::Value {
        let (py, ay) = if top {
            ([-0.7, 0.1], [2., 10.])
        } else {
            ([0.7, -0.1], [14., 6.])
        };
        let plane = json!({"left":0.0,"top":py[0],"right":0.5,"bottom":py[1]});
        let atlas = json!({"left":1,"top":ay[0],"right":6,"bottom":ay[1]});
        json!({
            "atlas":{"type":"msdf","width":16,"height":16,"distanceRange":4,"yOrigin":if top {"top"} else {"bottom"}},
            "metrics":{"emSize":1,"lineHeight":1.25,"ascender":if top {-0.8} else {0.8}},
            "glyphs":[{"unicode":65,"advance":0.6,"planeBounds":plane,"atlasBounds":atlas},
                      {"unicode":63,"advance":0.5,"planeBounds":plane,"atlasBounds":atlas},
                      {"unicode":32,"advance":0.25}],
            "kerning":[{"unicode1":65,"unicode2":65,"advance":-0.1}]
        })
    }
    #[test]
    fn both_origins_layout_the_same_quads_and_measure_with_kerning() {
        for top in [true, false] {
            let f = Font::parse(data(top).to_string().as_bytes()).unwrap();
            assert_eq!(f.unit_range(), [0.25, 0.25]);
            assert!((f.measure("AA\r\nA ?", 20.) - 27.).abs() < 0.001);
            let mut q = Vec::new();
            f.layout(&mut q, "AA\n \u{2603}", [3., 8.], 20., 99., [200; 4]);
            assert_eq!(q.len(), 3);
            assert_eq!(q[0].uv, [1. / 16., 2. / 16., 6. / 16., 10. / 16.]);
            assert!((q[0].rect[1] - 10.).abs() < 0.001);
            assert!((q[1].rect[0] - 13.).abs() < 0.001);
            assert!((q[2].rect[1] - 35.).abs() < 0.001);
            assert_eq!(q[2].rect[0], 8.);
            q.clear();
            f.layout(&mut q, "AA", [0.; 2], 20., 15., [255; 4]);
            assert_eq!(q.len(), 1);
        }
    }
    #[test]
    fn clip_interpolates_texture_coordinates_and_discards_empty_intersections() {
        let q = GlyphQuad {
            rect: [10., 20., 30., 60.],
            uv: [0., 0.25, 1., 0.75],
            color: [11; 4],
        };
        let c = q.clipped([15., 30., 25., 50.]).unwrap();
        assert_eq!(c.uv, [0.25, 0.375, 0.75, 0.625]);
        assert_eq!(c.color, q.color);
        assert!(q.clipped([0., 0., 10., 20.]).is_none());
    }
    #[test]
    fn malformed_atlases_do_not_produce_gpu_geometry() {
        for (path, value) in [
            ("/glyphs/1/unicode", json!(65)),
            ("/atlas/distanceRange", json!(0)),
            ("/glyphs/0/atlasBounds/left", json!(-1)),
            ("/metrics/emSize", json!(0)),
        ] {
            let mut d = data(true);
            *d.pointer_mut(path).unwrap() = value;
            assert!(Font::parse(d.to_string().as_bytes()).is_err());
        }
    }
}
