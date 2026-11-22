use glam::{Vec2, Vec3};
use serde::Deserialize;

pub const TICK_HZ: u32 = 120;
const STEP: f64 = 1.0 / TICK_HZ as f64;
const RADIUS: f32 = 0.25;

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
pub struct WalkConfig {
    pub min_xz: [f32; 2],
    pub max_xz: [f32; 2],
    pub spawn_xz: [f32; 2],
    pub floor_y: f32,
    pub speed: f32,
}

impl WalkConfig {
    pub const SPEED_RANGE: (f32, f32) = (0.1, 20.);

    pub fn set_speed(&mut self, value: f32) -> Result<(), &'static str> {
        Self::validate_speed(value)?;
        self.speed = value;
        Ok(())
    }

    fn validate_speed(value: f32) -> Result<(), &'static str> {
        let (min, max) = Self::SPEED_RANGE;
        (min..=max)
            .contains(&value)
            .then_some(())
            .ok_or("walk.speed must be within 0.1..20")
    }

    pub fn validate(&self) -> Result<(), &'static str> {
        Self::validate_speed(self.speed)?;
        let finite = |v: f32| (-1000.0..=1000.0).contains(&v);
        (finite(self.floor_y)
            && (0..2).all(|axis| {
                let min = self.min_xz[axis] + RADIUS;
                let max = self.max_xz[axis] - RADIUS;
                finite(self.min_xz[axis])
                    && finite(self.max_xz[axis])
                    && min <= max
                    && (min..=max).contains(&self.spawn_xz[axis])
            }))
        .then_some(())
        .ok_or("walk: invalid room bounds, spawn or floor")
    }
}

pub struct Walker {
    position: Vec2,
    pending: f64,
    yaw: f32,
    pitch: f32,
    tick: u64,
}

impl Walker {
    pub fn new(config: &WalkConfig) -> Self {
        Self {
            position: Vec2::from_array(config.spawn_xz),
            pending: 0.,
            yaw: 0.,
            pitch: 0.,
            tick: 0,
        }
    }

    pub fn constrain(&mut self, config: &WalkConfig) {
        self.position = self.position.clamp(
            Vec2::from_array(config.min_xz) + Vec2::splat(RADIUS),
            Vec2::from_array(config.max_xz) - Vec2::splat(RADIUS),
        );
    }

    pub fn update(&mut self, config: &WalkConfig, dt: f32, keys: u32, dx: f32, dy: f32) {
        self.look(dx, dy);
        self.pending += f64::from(dt.clamp(0.0, 0.1));
        let ticks = (self.pending / STEP) as u32;
        self.pending -= f64::from(ticks) * STEP;
        self.step(config, keys, ticks);
    }

    pub fn look(&mut self, dx: f32, dy: f32) {
        self.yaw = (self.yaw + dx * 0.004).rem_euclid(std::f32::consts::TAU);
        self.pitch = (self.pitch - dy * 0.004).clamp(-1.5, 1.5);
    }

    pub fn tick(&self) -> u64 {
        self.tick
    }

    pub fn step(&mut self, config: &WalkConfig, keys: u32, ticks: u32) {
        let (sin, cos) = self.yaw.sin_cos();
        let axes = Vec2::new(
            ((keys & 8 != 0) as i32 - (keys & 4 != 0) as i32) as f32,
            ((keys & 1 != 0) as i32 - (keys & 2 != 0) as i32) as f32,
        )
        .normalize_or_zero();
        let velocity =
            (axes.x * Vec2::new(cos, sin) + axes.y * Vec2::new(sin, -cos)) * config.speed;
        for _ in 0..ticks {
            self.position += velocity * STEP as f32;
            self.constrain(config);
            self.tick += 1;
        }
    }

    pub fn view(&self, config: &WalkConfig) -> (Vec3, Vec3) {
        let eye = Vec3::new(self.position.x, config.floor_y + 1.65, self.position.y);
        let direction = Vec3::new(
            self.yaw.sin() * self.pitch.cos(),
            self.pitch.sin(),
            -self.yaw.cos() * self.pitch.cos(),
        );
        (eye, eye + direction)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn walker_keeps_body_clear_of_walls() {
        let spec = WalkConfig {
            min_xz: [-4.; 2],
            max_xz: [4.; 2],
            spawn_xz: [0.; 2],
            floor_y: 0.,
            speed: 4.,
        };
        let mut walker = Walker::new(&spec);
        for _ in 0..100 {
            walker.update(&spec, 1. / 60., 1, 0., 0.);
        }
        let (eye, _) = walker.view(&spec);
        assert_eq!(eye.z, -3.75);
        assert_eq!(eye.x, 0.);
    }
}
