use super::*;
use glam::{EulerRot, Mat4, Quat, Vec2};

fn corners(center: Vec3, right: Vec3, up: Vec3, size: Vec2, rotation: f32) -> [Vec3; 4] {
    let (s, c) = rotation.sin_cos();
    let r = (right * c + up * s) * (0.5 * size.x);
    let u = (up * c - right * s) * (0.5 * size.y);
    [
        center - r + u,
        center + r + u,
        center + r - u,
        center - r - u,
    ]
}

fn atlas(layer: &Layer, frame: u32) -> [f32; 4] {
    let col = frame % layer.particle_sheet_columns;
    let row = frame / layer.particle_sheet_columns;
    let w = 1.0 / layer.particle_sheet_columns as f32;
    let h = 1.0 / layer.particle_sheet_rows as f32;
    let mut uv = [
        col as f32 * w,
        row as f32 * h,
        (col + 1) as f32 * w,
        (row + 1) as f32 * h,
    ];
    if layer.texture_flip_u {
        uv.swap(0, 2);
    }
    if layer.texture_flip_v {
        uv.swap(1, 3);
    }
    uv
}

fn color(l: &Layer, t: f32, tint: [f32; 4]) -> [f32; 4] {
    std::array::from_fn(|i| (l.color[i] + (l.particle_color_end[i] - l.color[i]) * t) * tint[i])
}
fn size(l: &Layer, t: f32, scale: f32) -> Vec2 {
    Vec2::from_array(l.particle_size_start).lerp(Vec2::from_array(l.particle_size_end), t) * scale
}

fn ribbon_length(r: &Ribbon) -> f32 {
    // Also gives a rotating blade with a stationary hilt a nonzero UV span.
    (r.corners[0].distance(r.corners[1]) + r.corners[3].distance(r.corners[2])) * 0.5
}

fn curve_center(points: [Vec3; 4], t: f32) -> Vec3 {
    // Centripetal Catmull-Rom from Gin. Nonuniform spacing avoids loops near tight turns.
    let [a, b, c, d] = points;
    let t1 = a.distance(b).max(0.0001).sqrt();
    let t2 = t1 + b.distance(c).max(0.0001).sqrt();
    let t3 = t2 + c.distance(d).max(0.0001).sqrt();
    let u = t1 + t * (t2 - t1);
    let ab = a.lerp(b, u / t1);
    let bc = b.lerp(c, t);
    let cd = c.lerp(d, (u - t2) / (t3 - t2));
    ab.lerp(bc, u / t2)
        .lerp(bc.lerp(cd, (u - t1) / (t3 - t1)), t)
}

fn visit_strip(strip: &[Ribbon], visitor: &mut impl FnMut(FxDraw<'_>)) {
    // Use a single texture across the visible trail, even across emitter updates.
    let total = strip
        .iter()
        .map(ribbon_length)
        .sum::<f32>()
        .max(f32::EPSILON);
    let mut distance = 0.0;
    for (i, r) in strip.iter().enumerate() {
        let layer = &r.asset.effects[r.layer];
        let length = ribbon_length(r);
        let previous = &strip[i.saturating_sub(1)].corners;
        let next = &strip[(i + 1).min(strip.len() - 1)].corners;
        let [a, b, c, d] = r.corners;
        let points = [
            (previous[0] + previous[3]) * 0.5,
            (a + d) * 0.5,
            (b + c) * 0.5,
            (next[1] + next[2]) * 0.5,
        ];
        let mut rgba = color(layer, 0.0, r.tint);
        rgba[3] *= (1.0 - r.age / r.life).clamp(0.0, 1.0);
        let mut start = (a, d);
        let mut u0 = distance / total;
        let steps = if strip.len() > 1 { 3 } else { 1 };
        for step in 1..=steps {
            let t = step as f32 / steps as f32;
            let end = if step == steps {
                (b, c)
            } else {
                let center = curve_center(points, t);
                let half_width = (d - a).lerp(c - b, t) * 0.5;
                (center - half_width, center + half_width)
            };
            let u1 = ((distance + t * length) / total).min(1.0);
            let mut uv = [u0, 0.0, u1, 1.0];
            if layer.texture_flip_u {
                uv[0] = 1.0 - u0;
                uv[2] = 1.0 - u1;
            }
            if layer.texture_flip_v {
                uv.swap(1, 3);
            }
            visitor(FxDraw::Quad {
                layer,
                corners: [start.0, end.0, end.1, start.1],
                uv,
                color: rgba,
                dissolve: (r.age / r.life).clamp(0.0, 1.0),
            });
            u0 = u1;
            start = end;
        }
        distance += length;
    }
}

impl FxWorld {
    /// World-space corners are ordered top-left, top-right, bottom-right, bottom-left.
    /// The caller owns material lookup, depth ordering and the transform into clip space.
    pub fn visit(&self, right: Vec3, up: Vec3, mut visitor: impl FnMut(FxDraw<'_>)) {
        let right = right.try_normalize().unwrap_or(Vec3::X);
        let up = up.try_normalize().unwrap_or(Vec3::Y);
        for p in &self.particles {
            let l = &p.asset.effects[p.layer];
            let t = (p.age / p.life).clamp(0.0, 1.0);
            let pos = p.origin + p.velocity * p.age + 0.5 * p.acceleration * p.age * p.age;
            let frame = if l.particle_sheet_frame_count > 0 {
                ((t * l.particle_sheet_frame_count as f32) as u32)
                    .min(l.particle_sheet_frame_count - 1)
            } else {
                p.frame
            };
            let (r, u) = if l.orientation == Orientation::Ground {
                (Vec3::X, Vec3::NEG_Z)
            } else {
                (right, up)
            };
            let mut rgba = color(l, t, p.tint);
            if p.looping {
                rgba[3] *= (t / 0.15).min(1.0);
            }
            visitor(FxDraw::Quad {
                layer: l,
                corners: corners(pos, r, u, size(l, t, p.size), p.angle + p.spin * p.age),
                uv: atlas(l, frame),
                color: rgba,
                dissolve: 0.0,
            });
        }
        for strip in self.ribbons.chunk_by(|a, b| {
            a.owner == b.owner
                && a.layer == b.layer
                && a.cycle == b.cycle
                && a.corners[1] == b.corners[0]
                && a.corners[2] == b.corners[3]
        }) {
            visit_strip(strip, &mut visitor);
        }
        for i in &self.instances {
            let normalized = (i.age.rem_euclid(i.duration) / i.duration) as f32;
            for (index, l) in i.asset.effects.iter().enumerate() {
                if matches!(l.kind, Kind::Particle | Kind::Ribbon)
                    || !(l.emission_window_start_normalized..=l.emission_window_end_normalized)
                        .contains(&normalized)
                {
                    continue;
                }
                let t = ((normalized - l.emission_window_start_normalized)
                    / (l.emission_window_end_normalized - l.emission_window_start_normalized)
                        .max(f32::EPSILON))
                .clamp(0.0, 1.0);
                let a = i.tracks[index].socket.unwrap_or(i.anchor);
                match l.kind {
                    Kind::Mesh => {
                        let r = l.mesh_rotate_deg.map(f32::to_radians);
                        let yaw = Quat::from_rotation_y(a.yaw());
                        let model = Mat4::from_scale_rotation_translation(
                            Vec3::from_array(l.mesh_scale) * a.scale,
                            yaw * Quat::from_euler(EulerRot::XYZ, r[0], r[1], r[2]),
                            a.origin + yaw * (Vec3::from_array(l.mesh_translate) * a.scale),
                        );
                        visitor(FxDraw::Mesh {
                            layer: l,
                            model,
                            color: color(l, 0.0, a.tint),
                            progress: t,
                            seconds: i.age as f32,
                        });
                    }
                    Kind::Beam => {
                        let delta = a.tip - a.origin;
                        if let Some(dir) = delta.try_normalize() {
                            let width = size(l, t, a.scale).x;
                            let side = right.cross(up).cross(dir).try_normalize().unwrap_or(right)
                                * (0.5 * width);
                            visitor(FxDraw::Quad {
                                layer: l,
                                corners: [
                                    a.origin + side,
                                    a.tip + side,
                                    a.tip - side,
                                    a.origin - side,
                                ],
                                uv: atlas(l, 0),
                                color: color(l, t, a.tint),
                                dissolve: 0.0,
                            });
                        }
                    }
                    Kind::Decal => {
                        let pos = a.origin + Vec3::from_array(l.mesh_translate) * a.scale;
                        visitor(FxDraw::Quad {
                            layer: l,
                            corners: corners(
                                pos,
                                Vec3::X,
                                Vec3::NEG_Z,
                                size(l, t, a.scale),
                                a.yaw(),
                            ),
                            uv: atlas(l, 0),
                            color: color(l, t, a.tint),
                            dissolve: 0.0,
                        });
                    }
                    _ => {}
                }
            }
        }
    }
}
