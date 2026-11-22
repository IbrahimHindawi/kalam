use super::{
    document::{encode, inline, set_value},
    ui::*,
};
use glam::{EulerRot, Mat4, Quat, Vec2, Vec3};
use serde_json::Value;

#[derive(Clone, Copy, Default, PartialEq, Eq)]
pub enum Mode {
    Select,
    #[default]
    Move,
    Rotate,
    Scale,
}

#[derive(Clone)]
pub enum Object {
    Entity(String),
    Planar {
        path: String,
        array: String,
        index: usize,
    },
}
impl Object {
    fn active(&self, selected: &str, document: &str) -> bool {
        match self {
            Self::Entity(id) => id == selected,
            Self::Planar { path, .. } => path == document,
        }
    }
    fn path(&self, scene: &str) -> String {
        match self {
            Self::Entity(id) => format!("scenes/{scene}/{id}.json"),
            Self::Planar { path, .. } => path.clone(),
        }
    }
    fn planar(&self) -> bool {
        matches!(self, Self::Planar { .. })
    }
}

#[derive(Clone, Copy)]
struct Pose {
    center: Vec3,
    scale: Vec3,
    angles: Vec3,
}
impl Pose {
    fn rotation_axis(self, axis: usize) -> Vec3 {
        let yaw = Quat::from_rotation_y(self.angles.y.to_radians());
        match axis {
            0 => yaw * Vec3::X,
            1 => Vec3::Y,
            _ => yaw * Quat::from_rotation_x(self.angles.x.to_radians()) * Vec3::Z,
        }
    }
    fn scale_axis(self, axis: usize) -> Vec3 {
        Quat::from_euler(
            EulerRot::YXZ,
            self.angles.y.to_radians(),
            self.angles.x.to_radians(),
            self.angles.z.to_radians(),
        ) * AXES[axis]
    }
}

struct Ray {
    position: Vec3,
    direction: Vec3,
}
impl Ray {
    fn plane(&self, position: Vec3, normal: Vec3) -> Option<Vec3> {
        let denominator = self.direction.dot(normal);
        if denominator.abs() < 0.035 {
            return None;
        }
        let t = (position - self.position).dot(normal) / denominator;
        (t >= 0.).then_some(self.position + self.direction * t)
    }
}

#[derive(Clone, Copy)]
struct Camera {
    vp: Mat4,
    inverse: Mat4,
    size: Vec2,
}
impl Camera {
    fn project(self, value: Vec3) -> Option<Vec3> {
        let clip = self.vp * value.extend(1.);
        if clip.w <= 1e-6 {
            return None;
        }
        let ndc = clip.truncate() / clip.w;
        if !ndc.is_finite() || !(0. ..=1.).contains(&ndc.z) {
            return None;
        }
        let pixel = (ndc.truncate() * Vec2::new(0.5, -0.5) + Vec2::splat(0.5)) * self.size;
        Some(pixel.extend(ndc.z))
    }
    fn unproject(self, point: Vec2, depth: f32) -> Option<Vec3> {
        if self.size.min_element() <= 0. {
            return None;
        }
        let ndc = point * Vec2::new(2., -2.) / self.size + Vec2::new(-1., 1.);
        let result = self.inverse.project_point3(ndc.extend(depth));
        result.is_finite().then_some(result)
    }
    fn ray(self, point: Vec2) -> Option<Ray> {
        let position = self.unproject(point, 0.)?;
        let direction = (self.unproject(point, 1.)? - position).try_normalize()?;
        Some(Ray {
            position,
            direction,
        })
    }
}

pub struct Target {
    object: Object,
    pose: Pose,
    camera: Camera,
}
impl Target {
    /// Native calls provide a nine-float transform and forward/inverse matrices.
    pub unsafe fn new(object: Object, transform: *const f32, projection: *const f32) -> Self {
        let v = unsafe { std::slice::from_raw_parts(transform, 9) };
        let m = unsafe { std::slice::from_raw_parts(projection, 32) };
        Self {
            object,
            pose: Pose {
                center: Vec3::from_slice(v),
                scale: Vec3::from_slice(&v[3..]),
                angles: Vec3::from_slice(&v[6..]),
            },
            camera: Camera {
                vp: Mat4::from_cols_slice(m),
                inverse: Mat4::from_cols_slice(&m[16..]),
                size: Vec2::ZERO,
            },
        }
    }
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum Part {
    Axis,
    Plane,
    Ring,
    Uniform,
}
#[derive(Clone, Copy)]
struct Handle {
    part: Part,
    axis: usize,
    points: [Vec2; 4],
    direction: Vec3,
}
impl Handle {
    fn hit(self, mouse: Vec2) -> Option<f32> {
        let [a, b, _, _] = self.points;
        match self.part {
            Part::Uniform => {
                let p = (mouse - a).abs();
                (p.x + p.y <= 10.).then_some(-1.)
            }
            Part::Plane => {
                let mut signs = [false; 2];
                for i in 0..4 {
                    let p = self.points[i];
                    let cross = (self.points[(i + 1) % 4] - p).perp_dot(mouse - p);
                    signs[0] |= cross < 0.;
                    signs[1] |= cross > 0.;
                }
                (!(signs[0] && signs[1])).then_some(0.)
            }
            _ => {
                let along = b - a;
                let len = along.length_squared();
                if len <= 0.01 {
                    return None;
                }
                let delta = mouse - a;
                let t = (delta.dot(along) / len).clamp(0., 1.);
                let distance = (delta - along * t).length();
                (distance < 8.).then_some(distance)
            }
        }
    }

    fn draw(self, draw: &DrawList, hovered: Option<Self>, mode: Mode) {
        let active = hovered.is_some_and(|h| h.part == self.part && h.axis == self.axis);
        let [a, b, c, _] = self.points;
        let colors = [
            rgba(242, 92, 86, 255),
            rgba(105, 213, 113, 255),
            rgba(91, 163, 255, 255),
        ];
        let color = if active {
            rgba(255, 219, 112, 255)
        } else {
            colors[self.axis]
        };
        let dark = rgba(16, 20, 24, 228);
        let stroke = if active { 3.3 } else { 2.2 };
        match self.part {
            Part::Plane => {
                draw.fill(
                    self.points,
                    (color & 0xffffff) | (if active { 138 } else { 62 } << 24),
                );
                draw.outline(self.points, color, if active { 2. } else { 1.2 });
            }
            Part::Ring => draw.line(a, b, color, stroke),
            Part::Uniform => {
                let diamond = [
                    a - Vec2::Y * 8.,
                    a + Vec2::X * 8.,
                    a + Vec2::Y * 8.,
                    a - Vec2::X * 8.,
                ];
                draw.fill(
                    diamond,
                    if active {
                        color
                    } else {
                        rgba(230, 234, 240, 255)
                    },
                );
                draw.outline(diamond, dark, 1.5);
            }
            Part::Axis => {
                let direction = (b - c).normalize();
                if mode == Mode::Move {
                    let base = b - direction * 13.;
                    draw.line(c, base, dark, stroke + 2.);
                    draw.line(c, base, color, stroke);
                    draw.fill(
                        [
                            b,
                            base + direction.perp() * 5.,
                            base - direction.perp() * 5.,
                        ],
                        color,
                    );
                } else {
                    draw.line(c, b, dark, stroke + 2.);
                    draw.line(c, b, color, stroke);
                    draw.rect(b - Vec2::splat(6.), b + Vec2::splat(6.), dark);
                    draw.rect(b - Vec2::splat(4.5), b + Vec2::splat(4.5), color);
                }
            }
        }
    }
}

#[derive(Default)]
pub struct Gizmo {
    pub target: Option<Target>,
    pub drag: Option<Drag>,
    mode: Mode,
    size: f32,
    handles: Vec<Handle>,
}

pub struct Pick {
    pub path: String,
    constraint: Constraint,
}
struct Constraint {
    object: Object,
    original: Pose,
    camera: Camera,
    mode: Mode,
    handle: Handle,
    mouse: Vec2,
    plane: Vec3,
    anchor: Vec3,
    size: f32,
}

pub struct Drag {
    pub path: String,
    pub original: String,
    pub last: String,
    pub pending: Option<Vec2>,
    constraint: Constraint,
}

const AXES: [Vec3; 3] = [Vec3::X, Vec3::Y, Vec3::Z];

impl Gizmo {
    pub fn set_mode(&mut self, mode: Mode) {
        self.mode = mode;
        self.handles.clear();
    }

    fn hit(&self, mouse: Vec2) -> Option<Handle> {
        self.handles
            .iter()
            .filter_map(|&handle| Some((handle.hit(mouse)?, handle)))
            .min_by(|a, b| a.0.total_cmp(&b.0))
            .map(|(_, handle)| handle)
    }

    pub fn draw(&mut self, enabled: bool, selection: &str, document: &str) {
        self.handles.clear();
        let Some(target) = self
            .target
            .as_mut()
            .filter(|t| t.object.active(selection, document))
        else {
            return;
        };
        if !enabled || self.mode == Mode::Select {
            return;
        }
        let Some(rect) = viewport_rect() else {
            return;
        };
        let (display, mouse) = frame_input();
        target.camera.size = display;
        let camera = target.camera;
        let pose = target.pose;
        let Some(origin) = camera.project(pose.center) else {
            return;
        };
        let center = origin.truncate();
        let bounds = Vec2::new(rect[0], rect[1]);
        if !center.cmpge(bounds).all() || !center.cmplt(bounds + Vec2::new(rect[2], rect[3])).all()
        {
            return;
        }
        let Some(edge) = camera.unproject(center - Vec2::Y * 76., origin.z) else {
            return;
        };
        let Some(ray) = camera.ray(center) else {
            return;
        };
        self.size = edge.distance(pose.center);
        if self.size < 1e-4 {
            return;
        }
        let size = self.size;
        let project = |p| camera.project(p).map(Vec3::truncate);
        let planar = target.object.planar();
        if self.mode == Mode::Move {
            for (axis, &normal) in AXES.iter().enumerate() {
                if (planar && axis != 1) || ray.direction.dot(normal).abs() < 0.18 {
                    continue;
                }
                let [u, v] = [AXES[(axis + 1) % 3], AXES[(axis + 2) % 3]];
                let corners = [(0.24, 0.24), (0.46, 0.24), (0.46, 0.46), (0.24, 0.46)]
                    .map(|(a, b)| project(pose.center + (u * a + v * b) * size));
                if let [Some(a), Some(b), Some(c), Some(d)] = corners {
                    self.handles.push(Handle {
                        part: Part::Plane,
                        axis,
                        points: [a, b, c, d],
                        direction: normal,
                    });
                }
            }
        }
        if self.mode == Mode::Rotate {
            for axis in 0..3 {
                if planar && axis != 1 {
                    continue;
                }
                let normal = pose.rotation_axis(axis);
                if ray.direction.dot(normal).abs() < 0.12 {
                    continue;
                }
                let u = normal
                    .cross(if normal.y.abs() < 0.8 {
                        Vec3::Y
                    } else {
                        Vec3::X
                    })
                    .normalize();
                let v = normal.cross(u);
                let mut previous = None;
                for i in 0..=64 {
                    let (sin, cos) = (i as f32 * std::f32::consts::TAU / 64.).sin_cos();
                    let point = project(pose.center + (u * cos + v * sin) * (size * 0.82));
                    if let Some((a, b)) = previous.zip(point) {
                        self.handles.push(Handle {
                            part: Part::Ring,
                            axis,
                            points: [a, b, Vec2::ZERO, Vec2::ZERO],
                            direction: normal,
                        });
                    }
                    previous = point;
                }
            }
        } else {
            for (axis, &world) in AXES.iter().enumerate() {
                if planar && axis == 1 {
                    continue;
                }
                let direction = if self.mode == Mode::Scale {
                    pose.scale_axis(axis)
                } else {
                    world
                };
                if let Some((a, b)) = project(pose.center + direction * (size * 0.2))
                    .zip(project(pose.center + direction * size))
                {
                    if b.distance(center) >= 16. {
                        self.handles.push(Handle {
                            part: Part::Axis,
                            axis,
                            points: [a, b, center, Vec2::ZERO],
                            direction,
                        });
                    }
                }
            }
            if self.mode == Mode::Scale {
                self.handles.push(Handle {
                    part: Part::Uniform,
                    axis: 0,
                    points: [center; 4],
                    direction: Vec3::ZERO,
                });
            }
        }
        if let Some(draw) = DrawList::get(true) {
            let hover = self
                .drag
                .as_ref()
                .map(|d| d.constraint.handle)
                .or_else(|| viewport_hit(mouse).then(|| self.hit(mouse)).flatten());
            if hover.is_some() {
                unsafe { ki_cursor_resize() };
            }
            draw.with_clip(rect, || {
                for &handle in &self.handles {
                    handle.draw(&draw, hover, self.mode);
                }
            });
        }
    }

    pub fn pick(&self, mouse: Vec2, selected: &str, document: &str, scene: &str) -> Option<Pick> {
        let target = self
            .target
            .as_ref()
            .filter(|target| target.object.active(selected, document))?;
        let handle = self.hit(mouse)?;
        let mut plane = Vec3::ZERO;
        let mut anchor = Vec3::ZERO;
        if handle.part != Part::Uniform {
            let ray = target.camera.ray(mouse)?;
            plane = if handle.part == Part::Axis {
                (ray.direction - handle.direction * ray.direction.dot(handle.direction))
                    .try_normalize()?
            } else {
                handle.direction
            };
            anchor = ray.plane(target.pose.center, plane)?;
        }
        Some(Pick {
            path: target.object.path(scene),
            constraint: Constraint {
                object: target.object.clone(),
                original: target.pose,
                camera: target.camera,
                mode: self.mode,
                handle,
                mouse,
                plane,
                anchor,
                size: self.size,
            },
        })
    }

    pub fn start(&mut self, pick: Pick, text: String) {
        self.drag = Some(Drag {
            path: pick.path,
            original: text.clone(),
            last: text,
            pending: None,
            constraint: pick.constraint,
        });
    }

    pub fn smoke_drag(
        &self,
        selection: &str,
        document: &str,
        scene: &str,
    ) -> Option<(String, Vec2, Vec2)> {
        let target = self
            .target
            .as_ref()
            .filter(|t| t.object.active(selection, document))?;
        let handle = self
            .handles
            .iter()
            .find(|handle| handle.part == Part::Axis)?;
        let [a, b, _, _] = handle.points;
        Some((target.object.path(scene), a.lerp(b, 0.65), a.lerp(b, 0.8)))
    }
}

impl Drag {
    pub fn edit(&self, mouse: Vec2, snapping: bool) -> Option<String> {
        let d = &self.constraint;
        let mut pose = d.original;
        let step = |value: f32, interval: f32| {
            if snapping {
                (value / interval).round() * interval
            } else {
                value
            }
        };
        let delta = if d.handle.part == Part::Uniform {
            let offset = mouse - d.mouse;
            let change = step((offset.x - offset.y) / 145., 0.1);
            pose.scale *= (1. + change).max(0.001);
            change.abs()
        } else {
            let hit = d.camera.ray(mouse)?.plane(pose.center, d.plane)?;
            let axis = d.handle.axis;
            let direction = d.handle.direction;
            let offset = hit - d.anchor;
            match d.mode {
                Mode::Scale => {
                    let amount = step(offset.dot(direction) / d.size, 0.1);
                    pose.scale[axis] *= (1. + amount).max(0.001);
                    amount.abs()
                }
                Mode::Rotate => {
                    let from = d.anchor - pose.center;
                    let to = hit - pose.center;
                    if to.length() < d.size * 0.1 {
                        return None;
                    }
                    let angle = step(
                        direction
                            .dot(from.cross(to))
                            .atan2(from.dot(to))
                            .to_degrees(),
                        15.,
                    );
                    pose.angles[axis] = (pose.angles[axis] + angle + 180.).rem_euclid(360.) - 180.;
                    angle.abs()
                }
                Mode::Move => {
                    let moved = if d.handle.part == Part::Axis {
                        direction * step(offset.dot(direction), 0.25)
                    } else {
                        Vec3::from_array(offset.to_array().map(|p| step(p, 0.25)))
                    };
                    pose.center += moved;
                    moved.length()
                }
                Mode::Select => return None,
            }
        };
        if delta < 1e-5 {
            return Some(self.original.clone());
        }
        let mut root: Value = serde_json::from_str(&self.original).ok()?;
        match &d.object {
            Object::Entity(_) => edit_entity(&mut root, pose, d.mode, d.handle)?,
            Object::Planar { array, index, .. } => {
                let wall = root.get_mut(array)?.get_mut(*index)?.as_object_mut()?;
                match d.mode {
                    Mode::Move => write_field(
                        wall,
                        "center",
                        [pose.center.x, pose.center.z],
                        f32::MIN,
                        f32::MAX,
                    )?,
                    Mode::Scale => write_field(
                        wall,
                        "half_size",
                        [pose.scale.x, pose.scale.z],
                        0.001,
                        f32::MAX,
                    )?,
                    Mode::Rotate => write_field(wall, "yaw_degrees", [pose.angles.y], -360., 360.)?,
                    Mode::Select => return None,
                }
            }
        }
        encode(&root)
    }
}

fn edit_entity(root: &mut Value, pose: Pose, mode: Mode, handle: Handle) -> Option<()> {
    let components = root
        .as_object_mut()?
        .entry("components")
        .or_insert_with(|| Value::Object(Default::default()))
        .as_object_mut()?;
    let inherited = !components.contains_key("transform");
    let data = components
        .entry("transform")
        .or_insert_with(|| Value::Object(Default::default()))
        .as_object_mut()?;
    if inherited || mode == Mode::Move {
        write_field(data, "position", pose.center.to_array(), -1000., 1000.)?;
    }
    if inherited || mode == Mode::Scale {
        let old = data.get("scale");
        let scalar = old.is_none_or(|field| {
            if inline(field) {
                field.get("type").and_then(Value::as_str) == Some("number")
            } else {
                field.is_number()
            }
        }) && (mode != Mode::Scale || handle.part == Part::Uniform)
            && pose.scale.min_element() == pose.scale.max_element();
        if scalar {
            write_field(data, "scale", [pose.scale.x], 0.01, 100.)?;
        } else {
            write_field(data, "scale", pose.scale.to_array(), 0.01, 100.)?;
        }
    }
    for (axis, name) in ["pitch_degrees", "yaw_degrees", "roll_degrees"]
        .into_iter()
        .enumerate()
    {
        if inherited || (mode == Mode::Rotate && axis == handle.axis) {
            write_field(data, name, [pose.angles[axis]], -360., 360.)?;
        }
    }
    Some(())
}

fn write_field<const N: usize>(
    data: &mut serde_json::Map<String, Value>,
    key: &str,
    values: [f32; N],
    min: f32,
    max: f32,
) -> Option<()> {
    let field = data.entry(key).or_insert(Value::Null);
    let lo = field
        .get("min")
        .and_then(Value::as_f64)
        .unwrap_or(min as f64)
        .max(min as f64);
    let hi = field
        .get("max")
        .and_then(Value::as_f64)
        .unwrap_or(max as f64)
        .min(max as f64);
    if lo > hi || values.iter().any(|n| !n.is_finite()) {
        return None;
    }
    let values = values.map(|n| Value::from((n as f64).clamp(lo, hi)));
    let next = if N == 1 {
        values[0].clone()
    } else {
        Value::from(Vec::from(values))
    };
    if N == 3 && inline(field) && field["type"] == "number" {
        field["type"] = "vec3".into();
        if let Some(reset) = field.get("default").filter(|v| v.is_number()).cloned() {
            field["default"] = Value::from(vec![reset; 3]);
        }
    }
    set_value(field, next);
    Some(())
}
