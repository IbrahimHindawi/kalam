//! Bounded, CPU-side playback of layered Gin FX descriptors. Each active instance
//! holds its original asset, so replacing a catalog entry cannot invalidate a trail.
mod descriptor;
mod draw;
#[cfg(test)]
mod tests;

pub use descriptor::{Blend, FxAsset, Kind, Layer, Orientation, MAX_LAYERS};
use glam::Vec3;
use std::sync::Arc;

pub const MAX_INSTANCES: usize = 128;
pub const MAX_PARTICLES: usize = 4096;
pub const MAX_RIBBONS: usize = 2048;
const MAX_SAMPLES_PER_LAYER_STEP: usize = 256;

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct FxId(u64);

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct FxAnchor {
    pub origin: Vec3,
    pub tip: Vec3,
    pub scale: f32,
    pub tint: [f32; 4],
}

impl FxAnchor {
    pub fn point(position: Vec3) -> Self {
        Self::segment(position, position)
    }
    pub fn segment(origin: Vec3, tip: Vec3) -> Self {
        Self {
            origin,
            tip,
            scale: 1.0,
            tint: [1.0; 4],
        }
    }
    fn valid(self) -> bool {
        self.origin.is_finite()
            && self.tip.is_finite()
            && self.scale.is_finite()
            && self.scale >= 0.0
            && self.tint.iter().all(|c| c.is_finite())
    }
    fn lerp(self, to: Self, t: f32) -> Self {
        Self {
            origin: self.origin.lerp(to.origin, t),
            tip: self.tip.lerp(to.tip, t),
            scale: self.scale + (to.scale - self.scale) * t,
            tint: std::array::from_fn(|i| self.tint[i] + (to.tint[i] - self.tint[i]) * t),
        }
    }
    fn yaw(self) -> f32 {
        let axis = self.tip - self.origin;
        if axis.x == 0.0 && axis.z == 0.0 {
            0.0
        } else {
            axis.x.atan2(axis.z)
        }
    }
}

#[derive(Clone, Copy)]
struct Random(u64);
impl Random {
    fn next(&mut self) -> u32 {
        // SplitMix64 also accepts zero as a seed.
        self.0 = self.0.wrapping_add(0x9e3779b97f4a7c15);
        let mut v = self.0;
        v = (v ^ (v >> 30)).wrapping_mul(0xbf58476d1ce4e5b9);
        v = (v ^ (v >> 27)).wrapping_mul(0x94d049bb133111eb);
        ((v ^ (v >> 31)) >> 32) as u32
    }
    fn unit(&mut self) -> f32 {
        (self.next() >> 8) as f32 / 16777216.0
    }
    fn range(&mut self, min: f32, max: f32) -> f32 {
        min + (max - min) * self.unit()
    }
}

#[derive(Clone, Copy)]
struct Track {
    next: f64,
    cycle: u64,
    first: bool,
    random: Random,
    previous_edge: Option<FxAnchor>,
    socket: Option<FxAnchor>,
    previous_socket: Option<FxAnchor>,
}

struct Instance {
    id: FxId,
    asset: Arc<FxAsset>,
    anchor: FxAnchor,
    previous_anchor: FxAnchor,
    age: f64,
    duration: f64,
    looping: bool,
    tracks: [Track; MAX_LAYERS],
}

struct Particle {
    owner: FxId,
    looping: bool,
    asset: Arc<FxAsset>,
    layer: usize,
    origin: Vec3,
    velocity: Vec3,
    acceleration: Vec3,
    age: f32,
    life: f32,
    size: f32,
    angle: f32,
    spin: f32,
    frame: u32,
    tint: [f32; 4],
}

struct Ribbon {
    owner: FxId,
    asset: Arc<FxAsset>,
    layer: usize,
    cycle: u64,
    sample: (u64, u8),
    corners: [Vec3; 4],
    tint: [f32; 4],
    age: f32,
    life: f32,
}

pub enum FxDraw<'a> {
    Quad {
        layer: &'a Layer,
        corners: [Vec3; 4],
        uv: [f32; 4],
        color: [f32; 4],
        dissolve: f32,
    },
    Mesh {
        layer: &'a Layer,
        model: glam::Mat4,
        color: [f32; 4],
        progress: f32,
        seconds: f32,
    },
}

pub struct FxWorld {
    next_id: u64,
    instances: Vec<Instance>,
    particles: Vec<Particle>,
    ribbons: Vec<Ribbon>,
}

impl Default for FxWorld {
    fn default() -> Self {
        Self {
            next_id: 0,
            instances: Vec::with_capacity(MAX_INSTANCES),
            particles: Vec::with_capacity(MAX_PARTICLES),
            ribbons: Vec::with_capacity(MAX_RIBBONS),
        }
    }
}

impl FxWorld {
    pub fn spawn(
        &mut self,
        asset: Arc<FxAsset>,
        anchor: FxAnchor,
        seed: u64,
        looping: bool,
    ) -> Option<FxId> {
        let duration = asset.seconds();
        self.spawn_timed(asset, anchor, seed, looping, duration)
    }

    /// For animation-authored FX, pass the driving clip's duration.
    pub fn spawn_timed(
        &mut self,
        asset: Arc<FxAsset>,
        anchor: FxAnchor,
        seed: u64,
        looping: bool,
        duration: f32,
    ) -> Option<FxId> {
        if self.instances.len() >= MAX_INSTANCES
            || asset.validate().is_err()
            || !(0.001..=3600.0).contains(&duration)
            || !anchor.valid()
        {
            return None;
        }
        self.next_id = self.next_id.checked_add(1)?;
        let id = FxId(self.next_id);
        let duration = f64::from(duration);
        let tracks = std::array::from_fn(|i| Track {
            next: asset.effects.get(i).map_or(f64::INFINITY, |l| {
                duration * f64::from(l.emission_window_start_normalized)
            }),
            cycle: 0,
            first: true,
            random: Random(seed ^ (i as u64).wrapping_mul(0x9e3779b97f4a7c15)),
            previous_edge: None,
            socket: None,
            previous_socket: None,
        });
        self.instances.push(Instance {
            id,
            asset,
            anchor,
            previous_anchor: anchor,
            age: 0.0,
            duration,
            looping,
            tracks,
        });
        Some(id)
    }

    pub fn set_anchor(&mut self, id: FxId, anchor: FxAnchor) -> bool {
        if !anchor.valid() {
            return false;
        }
        if let Some(inst) = self.instances.iter_mut().find(|i| i.id == id) {
            inst.anchor = anchor;
            return true;
        }
        false
    }

    /// Supply world-space socket positions from the game's already sampled pose.
    /// Pass None to resume using the common instance anchor.
    pub fn set_layer_anchor(&mut self, id: FxId, layer: usize, anchor: Option<FxAnchor>) -> bool {
        let Some(inst) = self.instances.iter_mut().find(|i| i.id == id) else {
            return false;
        };
        if layer >= inst.asset.effects.len() || anchor.is_some_and(|a| !a.valid()) {
            return false;
        }
        let track = &mut inst.tracks[layer];
        if track.socket.is_none() || anchor.is_none() {
            track.previous_socket = anchor;
            track.previous_edge = None;
        }
        track.socket = anchor;
        true
    }

    pub fn alive(&self, id: FxId) -> bool {
        self.instances.iter().any(|i| i.id == id)
            || self.particles.iter().any(|p| p.owner == id)
            || self.ribbons.iter().any(|r| r.owner == id)
    }

    /// End emission and let visible trails finish.
    pub fn release(&mut self, id: FxId) {
        self.instances.retain(|i| i.id != id);
    }

    /// Remove all visuals belonging to the instance immediately.
    pub fn stop(&mut self, id: FxId) {
        self.release(id);
        self.particles.retain(|p| p.owner != id);
        self.ribbons.retain(|r| r.owner != id);
    }

    pub fn clear(&mut self) {
        self.instances.clear();
        self.particles.clear();
        self.ribbons.clear();
    }

    pub fn step(&mut self, dt: f32) {
        if !dt.is_finite() || dt <= 0.0 {
            return;
        }
        self.particles.retain_mut(|p| {
            p.age += dt;
            p.age < p.life
        });
        self.ribbons.retain_mut(|r| {
            r.age += dt;
            r.age < r.life
        });
        for instance in &mut self.instances {
            let end = instance.age + f64::from(dt);
            for (index, layer) in instance.asset.effects.iter().enumerate() {
                if !matches!(layer.kind, Kind::Particle | Kind::Ribbon) {
                    continue;
                }
                let track = &mut instance.tracks[index];
                let from = track.previous_socket.unwrap_or(instance.previous_anchor);
                let to = track.socket.unwrap_or(instance.anchor);
                let mut samples = 0;
                while track.next < end {
                    let elapsed = (end - track.next) as f32;
                    let anchor = from.lerp(to, (1.0 - elapsed / dt).clamp(0.0, 1.0));
                    if layer.kind == Kind::Particle {
                        let count = layer.particle_emit_count
                            + if track.first {
                                layer.particle_burst_count
                            } else {
                                0
                            };
                        let count = (count as usize).min(MAX_PARTICLES - self.particles.len());
                        for _ in 0..count {
                            let p = Particle::spawn(
                                instance.id,
                                &instance.asset,
                                index,
                                anchor,
                                elapsed,
                                &mut track.random,
                                instance.looping,
                            );
                            if p.age < p.life {
                                self.particles.push(p);
                            }
                        }
                    } else {
                        let mut edge = anchor;
                        if edge.origin == edge.tip {
                            edge.tip += Vec3::from_array(layer.attach_b_offset) * edge.scale;
                        }
                        if let Some(last) = track.previous_edge {
                            append_ribbon(
                                &mut self.ribbons,
                                instance.id,
                                &instance.asset,
                                index,
                                last,
                                edge,
                                elapsed,
                                track,
                            );
                        }
                        track.previous_edge = Some(edge);
                    }
                    advance(track, layer, instance.duration, instance.looping);
                    samples += 1;
                    if samples == MAX_SAMPLES_PER_LAYER_STEP && track.next < end {
                        // A suspended application can return with minutes of elapsed time.
                        // Skip obsolete emissions, keeping the instance's clock and loop phase.
                        skip_backlog(track, layer, end, instance.duration, instance.looping);
                        break;
                    }
                }
                track.previous_socket = track.socket;
            }
            instance.previous_anchor = instance.anchor;
            instance.age = end;
        }
        self.instances.retain(|i| i.looping || i.age < i.duration);
        self.ribbons
            .sort_unstable_by_key(|r| (r.owner.0, r.layer, r.cycle, r.sample));
    }
}

fn full_window(layer: &Layer) -> bool {
    layer.emission_window_start_normalized == 0.0 && layer.emission_window_end_normalized == 1.0
}
fn continuous(layer: &Layer) -> bool {
    layer.kind == Kind::Ribbon || layer.particle_emit_count > 0
}
fn advance(track: &mut Track, layer: &Layer, duration: f64, looping: bool) {
    track.first = false;
    let end = duration * (track.cycle as f64 + f64::from(layer.emission_window_end_normalized));
    let next = track.next + f64::from(layer.emit_interval);
    if continuous(layer) && (next < end || looping && full_window(layer)) {
        track.next = next;
    } else if looping && !full_window(layer) {
        track.cycle = track.cycle.saturating_add(1);
        track.next =
            duration * (track.cycle as f64 + f64::from(layer.emission_window_start_normalized));
        track.first = true;
        track.previous_edge = None;
    } else {
        track.next = f64::INFINITY;
    }
}
fn skip_backlog(track: &mut Track, layer: &Layer, end: f64, duration: f64, looping: bool) {
    track.previous_edge = None;
    if !looping && end >= duration {
        track.next = f64::INFINITY;
        return;
    }
    let interval = f64::from(layer.emit_interval);
    if looping && full_window(layer) {
        track.next = if continuous(layer) {
            track.next + ((end - track.next) / interval).ceil() * interval
        } else {
            f64::INFINITY
        };
        return;
    }
    track.cycle = if looping {
        (end / duration).floor() as u64
    } else {
        0
    };
    let start = duration * (track.cycle as f64 + f64::from(layer.emission_window_start_normalized));
    track.first = end <= start;
    track.next = start;
    if !track.first {
        let last =
            duration * (track.cycle as f64 + f64::from(layer.emission_window_end_normalized));
        track.next = start + ((end - start) / interval).ceil() * interval;
        if !continuous(layer) || track.next >= last {
            track.next = last;
            advance(track, layer, duration, looping);
        }
    }
}

impl Particle {
    fn spawn(
        owner: FxId,
        asset: &Arc<FxAsset>,
        index: usize,
        anchor: FxAnchor,
        age: f32,
        rng: &mut Random,
        looping: bool,
    ) -> Self {
        let l = &asset.effects[index];
        let position = anchor.origin.lerp(anchor.tip, rng.unit());
        let offset = Vec3::from_array(std::array::from_fn(|_| {
            rng.range(-l.particle_spawn_radius, l.particle_spawn_radius)
        }));
        let velocity = Vec3::from_array(std::array::from_fn(|i| {
            rng.range(l.particle_velocity_min[i], l.particle_velocity_max[i])
        }));
        let frame = if l.particle_sheet_frame_count > 0 {
            0
        } else if l.particle_sheet_frame < 0 {
            rng.next() % (l.particle_sheet_columns * l.particle_sheet_rows)
        } else {
            l.particle_sheet_frame as u32
        };
        Self {
            owner,
            looping,
            asset: Arc::clone(asset),
            layer: index,
            origin: position + anchor.scale * offset,
            velocity: velocity * anchor.scale,
            acceleration: Vec3::from_array(l.particle_acceleration) * anchor.scale,
            age,
            life: rng.range(l.particle_life_min, l.particle_life_max),
            size: anchor.scale
                * (1.0 + rng.range(-l.particle_size_random, l.particle_size_random)).max(0.05),
            angle: rng
                .range(l.particle_rotation_min_deg, l.particle_rotation_max_deg)
                .to_radians(),
            spin: rng
                .range(l.particle_spin_min_deg, l.particle_spin_max_deg)
                .to_radians(),
            frame,
            tint: anchor.tint,
        }
    }
}

fn append_ribbon(
    ribbons: &mut Vec<Ribbon>,
    owner: FxId,
    asset: &Arc<FxAsset>,
    index: usize,
    last: FxAnchor,
    edge: FxAnchor,
    age: f32,
    track: &Track,
) {
    let layer = &asset.effects[index];
    if age >= layer.sample_life {
        return;
    }
    let distance = last
        .origin
        .distance(edge.origin)
        .max(last.tip.distance(edge.tip));
    if distance <= f32::EPSILON {
        return;
    }
    let steps = if layer.max_sample_gap > 0.0 {
        (distance / layer.max_sample_gap).ceil().clamp(1.0, 8.0) as usize
    } else {
        1
    };
    let mut a = last;
    for i in 1..=steps.min(MAX_RIBBONS - ribbons.len()) {
        let b = if i == steps {
            edge
        } else {
            last.lerp(edge, i as f32 / steps as f32)
        };
        ribbons.push(Ribbon {
            owner,
            asset: Arc::clone(asset),
            layer: index,
            cycle: track.cycle,
            sample: (track.next.to_bits(), i as u8),
            corners: [a.origin, b.origin, b.tip, a.tip],
            tint: b.tint,
            age,
            life: layer.sample_life,
        });
        a = b;
    }
}
