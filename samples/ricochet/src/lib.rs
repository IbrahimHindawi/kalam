use glam::Vec2;
use serde::Deserialize;

pub const SCENE: &str = "ricochet";
pub const TICK_HZ: u32 = 120;
const DT: f32 = 1. / TICK_HZ as f32;
const SKIN: f32 = 0.001;
const MAX_SHOTS: usize = 256;
const MAX_IMPACTS: usize = 128;
const MAX_BASE_BOUNCES: u8 = 8;
const MAX_SHOT_BOUNCES: u8 = 12;
const TRACE_POINTS: usize = MAX_SHOT_BOUNCES as usize + 2;

pub const DOCUMENTS: [(&str, &str); 5] = [
    ("Arena", "samples/ricochet/arena.json"),
    ("Weapon", "samples/ricochet/weapon.json"),
    ("Enemies", "samples/ricochet/enemies.json"),
    ("Waves", "samples/ricochet/waves.json"),
    ("Effects", "samples/ricochet/fx.json"),
];

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Arena {
    pub half_size: Vec2,
    pub player_spawn: Vec2,
    pub range_target: Vec2,
    pub player_speed: f32,
    pub walls: Vec<Wall>,
}

#[derive(Clone, Copy, Deserialize, PartialEq)]
#[serde(deny_unknown_fields)]
pub struct Wall {
    pub center: Vec2,
    pub half_size: Vec2,
    pub reflective: bool,
    #[serde(default)]
    pub yaw_degrees: f32,
}

impl Wall {
    /// Positive 3D yaw maps world X toward negative Z.
    pub fn local_rotation(self) -> Vec2 {
        Vec2::from_angle(self.yaw_degrees.to_radians())
    }
}

#[derive(Clone, Copy, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Weapon {
    pub projectile_speed: f32,
    pub max_bounces: u8,
    pub damage: f32,
    pub shots_per_second: f32,
}

impl Weapon {
    fn validate(self) -> Result<(), &'static str> {
        if self.max_bounces > MAX_BASE_BOUNCES {
            return Err("max_bounces: exceeds the runtime limit of 8");
        }
        // Allow room for all wave upgrades and the three-second trace distance.
        if self.projectile_speed <= 0. || !(self.projectile_speed * 16.).is_finite() {
            return Err("projectile_speed: must produce a finite positive trace");
        }
        if self.damage <= 0. || !self.damage.is_finite() {
            return Err("damage: must be finite and positive");
        }
        if !(0. < self.shots_per_second && self.shots_per_second <= TICK_HZ as f32)
            || !self.shots_per_second.recip().is_finite()
        {
            return Err("shots_per_second: requires a finite cooldown of at least one tick");
        }
        Ok(())
    }
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Enemies {
    pub chaser_speed: f32,
    pub chaser_health: f32,
    pub chaser_damage: f32,
    pub turret_health: f32,
    pub turret_cooldown: f32,
    pub turret_projectile_speed: f32,
    pub turret_damage: f32,
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Wave {
    pub chasers: u8,
    pub turrets: u8,
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Effects {
    pub bounce_color: [f32; 3],
    pub hit_color: [f32; 3],
    pub duration: f32,
}

pub struct Config {
    pub arena: Arena,
    pub weapon: Weapon,
    pub enemies: Enemies,
    pub waves: Vec<Wave>,
    pub effects: Effects,
}

impl Config {
    pub fn load_overrides(
        bytes: Option<&[u8]>,
        mut read: impl FnMut(&str) -> Result<Vec<u8>, String>,
    ) -> Result<Self, String> {
        let drafts: std::collections::BTreeMap<String, String> = bytes
            .map(serde_json::from_slice)
            .transpose()
            .map_err(|e| e.to_string())?
            .unwrap_or_default();
        Self::load(|path| {
            drafts
                .get(path)
                .map_or_else(|| read(path), |text| Ok(text.as_bytes().to_vec()))
        })
    }

    pub fn load(mut read: impl FnMut(&str) -> Result<Vec<u8>, String>) -> Result<Self, String> {
        fn document<T: serde::de::DeserializeOwned>(
            index: usize,
            read: &mut impl FnMut(&str) -> Result<Vec<u8>, String>,
        ) -> Result<T, String> {
            let path = DOCUMENTS[index].1;
            kalam_runtime::inline_fields::decode(&read(path)?).map_err(|e| format!("{path}: {e}"))
        }
        let config = Self {
            arena: document(0, &mut read)?,
            weapon: document(1, &mut read)?,
            enemies: document(2, &mut read)?,
            waves: document(3, &mut read)?,
            effects: document(4, &mut read)?,
        };
        let (a, w, e, f) = (
            &config.arena,
            config.weapon,
            &config.enemies,
            &config.effects,
        );
        let range = |x, min, max| (min..=max).contains(&x);
        w.validate()
            .map_err(|error| format!("{}: {error}", DOCUMENTS[1].1))?;
        // These minima protect the collision math if the user changes the JSON field declarations.
        if !a.half_size.is_finite() || a.half_size.min_element() <= 0.5 {
            return Err(format!(
                "{}: half_size must exceed 0.5 on both axes",
                DOCUMENTS[0].1
            ));
        }
        let valid = [
            range(e.chaser_speed, 0.25, 10.)
                && range(e.chaser_health, 1., 500.)
                && range(e.chaser_damage, 1., 100.)
                && range(e.turret_health, 1., 500.)
                && range(e.turret_cooldown, 0.3, 10.)
                && range(e.turret_projectile_speed, 1., 40.)
                && range(e.turret_damage, 1., 100.),
            !config.waves.is_empty()
                && config.waves.len() <= 8
                && config.waves.iter().all(|wave| {
                    (1..=24).contains(&(u16::from(wave.chasers) + u16::from(wave.turrets)))
                }),
            range(f.duration, 0.025, 2.)
                && f.bounce_color
                    .into_iter()
                    .chain(f.hit_color)
                    .all(|channel| range(channel, 0., 1.)),
        ];
        if let Some(index) = valid.iter().position(|v| !v) {
            return Err(format!(
                "{}: values outside the supported range or invalid layout",
                DOCUMENTS[[2, 3, 4][index]].1
            ));
        }
        Ok(config)
    }
}

#[derive(Clone, Copy, PartialEq, Eq)]
pub enum EnemyKind {
    Chaser,
    Turret,
}

pub struct Enemy {
    pub position: Vec2,
    pub kind: EnemyKind,
    pub health: f32,
    cooldown: f32,
}

#[derive(Clone, Copy)]
pub struct Shot {
    pub position: Vec2,
    direction: Vec2,
    speed: f32,
    damage: f32,
    bounces: u8,
    age: f32,
    pub hostile: bool,
}

#[derive(Clone, Copy, PartialEq, Eq)]
pub enum ImpactKind {
    Bounce,
    Wall,
    Hit,
    Destroyed,
}

pub struct Impact {
    pub position: Vec2,
    pub kind: ImpactKind,
    pub remaining: f32,
    pub lifetime: f32,
    pub color: [f32; 3],
}

#[derive(Clone, Copy, PartialEq, Eq)]
pub enum Phase {
    Play,
    Upgrade,
    Win,
    Loss,
}

#[derive(Clone, Copy, Default)]
pub struct Input {
    pub movement: Vec2,
    pub aim: Vec2,
    pub fire: bool,
}

pub struct Game {
    pub player: Vec2,
    pub facing: Vec2,
    pub health: f32,
    pub enemies: Vec<Enemy>,
    pub shots: Vec<Shot>,
    pub impacts: Vec<Impact>,
    pub wave: usize,
    pub phase: Phase,
    pub range: bool,
    pub tick: u64,
    pub hits: u32,
    pub kills: u32,
    pub sound: Option<ImpactKind>,
    fire_timer: f32,
    invulnerability: f32,
    extra_bounces: u8,
    speed_scale: f32,
}

impl Game {
    pub fn new(config: &Config) -> Self {
        let mut game = Self {
            player: config.arena.player_spawn,
            facing: Vec2::X,
            health: 100.,
            enemies: Vec::with_capacity(24),
            shots: Vec::with_capacity(MAX_SHOTS),
            impacts: Vec::with_capacity(MAX_IMPACTS),
            wave: 0,
            phase: Phase::Play,
            range: false,
            tick: 0,
            hits: 0,
            kills: 0,
            sound: None,
            fire_timer: 0.,
            invulnerability: 0.,
            extra_bounces: 0,
            speed_scale: 1.,
        };
        game.reset(config, false);
        game
    }

    pub fn reset(&mut self, config: &Config, range: bool) {
        self.range = range;
        self.wave = 0;
        self.tick = 0;
        self.hits = 0;
        self.kills = 0;
        self.facing = Vec2::X;
        self.extra_bounces = 0;
        self.speed_scale = 1.;
        self.impacts.clear();
        self.start_wave(config);
    }

    fn start_wave(&mut self, config: &Config) {
        self.player = config.arena.player_spawn;
        self.health = 100.;
        self.phase = Phase::Play;
        self.fire_timer = 0.;
        self.invulnerability = 0.;
        self.sound = None;
        self.shots.clear();
        self.enemies.clear();
        let wave = &config.waves[self.wave];
        let (turrets, chasers) = if self.range {
            (1, 0)
        } else {
            (wave.turrets, wave.chasers)
        };
        let extents = config.arena.half_size - Vec2::splat(0.5);
        let count = turrets + chasers;
        for index in 0..count {
            let kind = if index < turrets {
                EnemyKind::Turret
            } else {
                EnemyKind::Chaser
            };
            let position = if index == 0 && kind == EnemyKind::Turret {
                config.arena.range_target
            } else {
                let angle = (f32::from(index) + 0.5) * std::f32::consts::TAU / f32::from(count);
                let direction = Vec2::from_angle(angle);
                let mut spawn = direction / (direction.abs() / extents).max_element();
                if config
                    .arena
                    .walls
                    .iter()
                    .any(|wall| inside(spawn, *wall, 0.4))
                    || spawn.distance(self.player) < 0.85
                {
                    spawn = config.arena.range_target;
                }
                spawn
            };
            self.enemies.push(Enemy {
                position,
                kind,
                health: if kind == EnemyKind::Turret {
                    config.enemies.turret_health
                } else {
                    config.enemies.chaser_health
                },
                cooldown: 0.7 + f32::from(index) * 0.13,
            });
        }
    }

    pub fn upgrade(&mut self, config: &Config, more_bounces: bool) {
        if self.phase != Phase::Upgrade {
            return;
        }
        if more_bounces {
            self.extra_bounces += 1;
        } else {
            self.speed_scale *= 1.25;
        }
        self.wave = (self.wave + 1).min(config.waves.len() - 1);
        self.start_wave(config);
    }

    pub fn weapon(&self, config: &Config) -> Weapon {
        let mut weapon = config.weapon;
        weapon.max_bounces = (weapon.max_bounces + self.extra_bounces).min(MAX_SHOT_BOUNCES);
        weapon.projectile_speed *= self.speed_scale;
        weapon
    }

    pub fn tick(&mut self, config: &Config, input: Input) -> bool {
        for impact in &mut self.impacts {
            impact.remaining -= DT;
        }
        self.impacts.retain(|impact| impact.remaining > 0.);
        if self.phase != Phase::Play {
            return false;
        }
        self.tick += 1;
        self.invulnerability -= DT;
        self.fire_timer -= DT;
        let aim = (input.aim - self.player).normalize_or_zero();
        if aim != Vec2::ZERO {
            self.facing = aim;
        }
        move_body(
            &config.arena,
            &mut self.player,
            input.movement.normalize_or_zero() * config.arena.player_speed * DT,
            0.26,
        );
        let fired = input.fire && self.fire_timer <= 0. && self.shots.len() < MAX_SHOTS;
        if fired {
            let weapon = self.weapon(config);
            self.shots.push(Shot {
                position: self.player,
                direction: self.facing,
                speed: weapon.projectile_speed,
                damage: weapon.damage,
                bounces: weapon.max_bounces,
                age: 0.,
                hostile: false,
            });
            self.fire_timer = 1. / weapon.shots_per_second;
        }
        for enemy in &mut self.enemies {
            let direction = (self.player - enemy.position).normalize_or_zero();
            match enemy.kind {
                EnemyKind::Chaser => {
                    let distance = config.enemies.chaser_speed * DT;
                    let initial = enemy.position;
                    move_body(
                        &config.arena,
                        &mut enemy.position,
                        direction * distance,
                        0.3,
                    );
                    if enemy.position.distance_squared(initial) < distance * distance * 0.04 {
                        // Slide around the blocker. Keep the same handedness at a dead end.
                        move_body(
                            &config.arena,
                            &mut enemy.position,
                            direction.perp() * distance,
                            0.3,
                        );
                    }
                    if enemy.position.distance_squared(self.player) < 0.57 * 0.57 {
                        hurt(
                            &mut self.health,
                            &mut self.invulnerability,
                            config.enemies.chaser_damage,
                            self.range,
                        );
                    }
                }
                EnemyKind::Turret => {
                    enemy.cooldown -= DT;
                    if !self.range && enemy.cooldown <= 0. && self.shots.len() < MAX_SHOTS {
                        self.shots.push(Shot {
                            position: enemy.position,
                            direction,
                            speed: config.enemies.turret_projectile_speed,
                            damage: config.enemies.turret_damage,
                            bounces: 0,
                            age: 0.,
                            hostile: true,
                        });
                        enemy.cooldown = config.enemies.turret_cooldown;
                    }
                }
            }
        }
        let mut index = 0;
        while index < self.shots.len() {
            let mut shot = self.shots[index];
            shot.age += DT;
            if shot.age < 3. && self.advance_shot(config, &mut shot) {
                self.shots[index] = shot;
                index += 1;
            } else {
                self.shots.swap_remove(index);
            }
        }
        if self.health <= 0. {
            self.phase = Phase::Loss;
        } else if !self.range && self.enemies.is_empty() {
            self.phase = if self.wave + 1 >= config.waves.len() {
                Phase::Win
            } else {
                Phase::Upgrade
            };
        }
        fired
    }

    fn impact(&mut self, config: &Config, position: Vec2, kind: ImpactKind) {
        self.sound = Some(kind);
        if self.impacts.len() == MAX_IMPACTS {
            return;
        }
        let color = if matches!(kind, ImpactKind::Bounce | ImpactKind::Wall) {
            config.effects.bounce_color
        } else {
            config.effects.hit_color
        };
        let lifetime = config.effects.duration
            * if kind == ImpactKind::Destroyed {
                2.
            } else {
                1.
            };
        self.impacts.push(Impact {
            position,
            kind,
            lifetime,
            remaining: lifetime,
            color,
        });
    }

    fn advance_shot(&mut self, config: &Config, shot: &mut Shot) -> bool {
        let mut remaining = shot.speed * DT;
        while remaining > SKIN {
            let wall = wall_hit(&config.arena, shot.position, shot.direction, remaining);
            let limit = wall.as_ref().map_or(remaining, |hit| hit.distance);
            if shot.hostile {
                if let Some(distance) =
                    circle_hit(shot.position, shot.direction, self.player, 0.28, limit)
                {
                    hurt(
                        &mut self.health,
                        &mut self.invulnerability,
                        shot.damage,
                        self.range,
                    );
                    self.impact(
                        config,
                        shot.position + shot.direction * distance,
                        ImpactKind::Hit,
                    );
                    return false;
                }
            } else if let Some((index, distance)) =
                target_hit(&self.enemies, shot.position, shot.direction, limit)
            {
                self.hits += 1;
                if !self.range {
                    self.enemies[index].health -= shot.damage;
                }
                let kind = if self.enemies[index].health <= 0. {
                    self.enemies.swap_remove(index);
                    self.kills += 1;
                    ImpactKind::Destroyed
                } else {
                    ImpactKind::Hit
                };
                self.impact(config, shot.position + shot.direction * distance, kind);
                return false;
            }
            let Some(hit) = wall else {
                shot.position += shot.direction * remaining;
                return true;
            };
            shot.position += shot.direction * hit.distance;
            if !hit.reflective || shot.bounces == 0 {
                self.impact(config, shot.position, ImpactKind::Wall);
                return false;
            }
            self.impact(config, shot.position, ImpactKind::Bounce);
            shot.bounces -= 1;
            shot.direction = reflect(shot.direction, hit.normal);
            shot.position += hit.normal * SKIN;
            remaining -= hit.distance + SKIN;
        }
        true
    }

    pub fn trace(&self, config: &Config) -> Trace {
        let mut trace = Trace {
            points: [Vec2::ZERO; TRACE_POINTS],
            len: 1,
            hits_target: false,
        };
        trace.points[0] = self.player;
        let mut position = self.player;
        let mut direction = self.facing;
        let mut budget = self.weapon(config).projectile_speed * 3.;
        for bounce in 0..=self.weapon(config).max_bounces {
            let wall = wall_hit(&config.arena, position, direction, budget);
            let distance = wall.as_ref().map_or(budget, |hit| hit.distance);
            let target = target_hit(&self.enemies, position, direction, distance);
            trace.points[trace.len] =
                position + direction * target.map_or(distance, |(_, distance)| distance);
            trace.len += 1;
            trace.hits_target = target.is_some();
            let Some(hit) = wall.filter(|hit| {
                hit.reflective && !trace.hits_target && bounce < self.weapon(config).max_bounces
            }) else {
                break;
            };
            position = trace.points[trace.len - 1] + hit.normal * SKIN;
            direction = reflect(direction, hit.normal);
            budget -= hit.distance + SKIN;
            if budget <= SKIN {
                break;
            }
        }
        trace
    }
}

pub struct Trace {
    points: [Vec2; TRACE_POINTS],
    len: usize,
    pub hits_target: bool,
}

impl Trace {
    pub fn points(&self) -> &[Vec2] {
        &self.points[..self.len]
    }
}

fn hurt(health: &mut f32, cooldown: &mut f32, damage: f32, invincible: bool) {
    if *cooldown > 0. {
        return;
    }
    if !invincible {
        *health = (*health - damage).max(0.);
    }
    *cooldown = 0.6;
}

fn inside(p: Vec2, wall: Wall, radius: f32) -> bool {
    wall.local_rotation()
        .rotate(p - wall.center)
        .abs()
        .cmplt(wall.half_size + Vec2::splat(radius))
        .all()
}

fn move_body(arena: &Arena, p: &mut Vec2, delta: Vec2, radius: f32) {
    let limit = arena.half_size - Vec2::splat(radius);
    for axis in [Vec2::X, Vec2::Y] {
        let next = (*p + axis * delta.dot(axis)).clamp(-limit, limit);
        if !arena.walls.iter().any(|wall| inside(next, *wall, radius)) {
            *p = next;
        }
    }
}

fn reflect(direction: Vec2, normal: Vec2) -> Vec2 {
    direction - 2. * direction.dot(normal) * normal
}

struct WallHit {
    distance: f32,
    normal: Vec2,
    reflective: bool,
}

fn wall_hit(arena: &Arena, from: Vec2, dir: Vec2, limit: f32) -> Option<WallHit> {
    let mut best = None;
    let mut nearest = limit;
    let mut consider = |distance, normal, reflective| {
        if distance >= 0. && distance <= nearest {
            nearest = distance;
            best = Some(WallHit {
                distance,
                normal,
                reflective,
            });
        }
    };
    for axis in [Vec2::X, Vec2::Y] {
        let component = dir.dot(axis);
        if component.abs() > 1e-6 {
            let sign = component.signum();
            let distance = (arena.half_size.dot(axis) * sign - from.dot(axis)) / component;
            consider(distance, -axis * sign, true);
        }
    }
    for wall in &arena.walls {
        let rotation = wall.local_rotation();
        let origin = rotation.rotate(from - wall.center);
        let direction = rotation.rotate(dir);
        let mut entry: f32 = 0.;
        let mut exit = limit;
        let mut normal = Vec2::ZERO;
        for axis in [Vec2::X, Vec2::Y] {
            let coordinate = origin.dot(axis);
            let component = direction.dot(axis);
            let half = wall.half_size.dot(axis);
            if component.abs() < 1e-6 {
                if coordinate.abs() > half {
                    exit = -1.;
                }
                continue;
            }
            let a = (-half - coordinate) / component;
            let b = (half - coordinate) / component;
            if a.min(b) >= entry {
                entry = a.min(b);
                normal = -axis * component.signum();
            }
            exit = exit.min(a.max(b));
        }
        if entry <= exit && normal != Vec2::ZERO {
            consider(
                entry,
                Vec2::new(rotation.x, -rotation.y).rotate(normal),
                wall.reflective,
            );
        }
    }
    best
}

fn circle_hit(from: Vec2, dir: Vec2, center: Vec2, radius: f32, limit: f32) -> Option<f32> {
    let offset = center - from;
    let projected = offset.dot(dir);
    let discriminant = projected * projected - offset.length_squared() + radius * radius;
    if discriminant < 0. || projected < -radius {
        return None;
    }
    let distance = (projected - discriminant.sqrt()).max(0.);
    (distance <= limit).then_some(distance)
}

fn target_hit(enemies: &[Enemy], from: Vec2, dir: Vec2, limit: f32) -> Option<(usize, f32)> {
    enemies
        .iter()
        .enumerate()
        .filter_map(|(index, enemy)| {
            circle_hit(from, dir, enemy.position, 0.38, limit).map(|distance| (index, distance))
        })
        .min_by(|a, b| a.1.total_cmp(&b.1))
}
