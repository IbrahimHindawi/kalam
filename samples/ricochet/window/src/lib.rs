use bumpalo::{collections::Vec as FrameVec, Bump};
use glam::{Mat4, Quat, Vec2, Vec3};
use kalam_host::game::{self as host, *};
use kalam_ricochet::{Config, EnemyKind, Game, ImpactKind, Input, Phase, Wall, SCENE, TICK_HZ};
use kalam_runtime::{
    catalog::{Catalog, ClipSource, SkinnedMesh},
    mesh::Geometry,
    scene::Camera,
    skin::{Pose, Skin},
};
use std::time::Instant;

const STEP: f32 = 1. / TICK_HZ as f32;
const HUD_HEIGHT: u32 = 90;
const BANK_POINT: f32 = -0.5;
const ASSET_CATALOG: &str = "samples/ricochet/res.toml";
const ARENA_DOCUMENT: usize = 0;
const WALL_CENTER_Y: f32 = 0.34;
const WALL_HALF_HEIGHT: f32 = 0.35;

struct Ricochet;
static APPLICATION: Ricochet = Ricochet;

impl host::Application for Ricochet {
    fn scene(&self) -> &str {
        SCENE
    }
    fn documents(&self) -> &[&str] {
        const PATHS: [&str; 5] = [
            kalam_ricochet::DOCUMENTS[ARENA_DOCUMENT].1,
            kalam_ricochet::DOCUMENTS[1].1,
            kalam_ricochet::DOCUMENTS[2].1,
            kalam_ricochet::DOCUMENTS[3].1,
            kalam_ricochet::DOCUMENTS[4].1,
        ];
        &PATHS
    }
    fn validate(&self, documents: &[u8]) -> Result<(), String> {
        load_config(Some(documents)).map(|_| ())
    }
    fn run(&self, smoke: bool) -> Result<(), String> {
        let mut assets = Assets::default();
        let result = run_game(smoke, &mut assets);
        assets.unload();
        result
    }
    fn smoke_document(&self) -> Option<(&str, &str)> {
        Some((
            kalam_ricochet::DOCUMENTS[1].1,
            r#"{
          "projectile_speed": {"value": 31, "type": "number", "min": 10, "max": 40},
          "max_bounces": {"type": "integer", "default": 1, "min": 0, "max": 3},
          "damage": 34,
          "shots_per_second": 4
        }"#,
        ))
    }
}

#[no_mangle]
pub extern "C" fn kh_register_game() -> *const std::ffi::c_char {
    match host::register(&APPLICATION) {
        Ok(()) => c"ricochet".as_ptr(),
        Err(_) => std::ptr::null(),
    }
}

fn load_config(overrides: Option<&[u8]>) -> Result<Config, String> {
    Config::load_overrides(overrides, |path| host::read_asset(path, 64 * 1024))
}

fn run_game(smoke: bool, assets: &mut Assets) -> Result<(), String> {
    let mut cpu = load_scene(SCENE, None)?;
    let mut config = load_config(None)?;
    let mut game = Game::new(&config);
    let mut editor_camera = cpu.scene.camera.clone();
    let mut selected_wall = None;
    let mut graphics = upload(assets, &cpu)?;
    let cube_geometry = cube_geometry();
    let mut cube = assets.mesh(&cube_geometry)?;
    let catalog = Catalog::parse(&host::read_asset(ASSET_CATALOG, 64 * 1024)?)
        .map_err(|error| format!("{ASSET_CATALOG}: {error}"))?;
    let repair_spec = catalog.skin("repair_bot")?;
    let chaser_spec = catalog.skin("chaser_bot")?;
    let mut bot = AnimatedBot::load(repair_spec, assets)?;
    let mut chaser = AnimatedMesh::load(chaser_spec, assets)?;
    let chaser_walk = chaser.clip("walk")?;
    let mut scratch = Bump::new();
    let mut accumulator = 0.;
    let mut pending_fire = false;
    let mut play_revision = 0;
    let mut hud = None;
    let mut frames = 0;
    let profile = smoke && std::env::var_os("KALAM_RICOCHET_PROFILE").is_some();
    let crowd_count = if profile {
        match std::env::var("KALAM_SKIN_CROWD") {
            Ok(value) => value
                .parse::<usize>()
                .ok()
                .filter(|&count| count <= 64)
                .ok_or("KALAM_SKIN_CROWD must be between 0 and 64")?,
            Err(std::env::VarError::NotPresent) => 0,
            Err(error) => return Err(error.to_string()),
        }
    } else {
        0
    };
    let mut crowd = chaser.instances(assets, chaser.instance.buffer, crowd_count)?;
    if smoke {
        eprintln!(
            "Ricochet skinning: {} player, {} chaser, {crowd_count} independent crowd poses",
            bot.rig.instance.backend(),
            chaser.instance.backend()
        );
    }
    let mut frame_times = Vec::with_capacity(if profile { 240 } else { 0 });
    let mut skin_times = Vec::with_capacity(frame_times.capacity());
    let sounds = Sounds::new();
    if smoke {
        verify_wall_picking(&config)?;
        game.reset(&config, true);
        let input = Input {
            aim: bank_point(&config),
            ..Input::default()
        };
        game.tick(
            &config,
            Input {
                fire: true,
                ..input
            },
        );
        for _ in 0..75 {
            game.tick(&config, input);
        }
        if game.hits != 1 {
            return Err("smoke: reference bank shot missed".into());
        }
        game.reset(&config, true);
        game.tick(&config, input);
    }
    if profile {
        let busiest = config
            .waves
            .iter()
            .enumerate()
            .max_by_key(|(_, wave)| wave.chasers + wave.turrets)
            .map_or(0, |(index, _)| index);
        config.waves.rotate_left(busiest);
        game.reset(&config, false);
    }
    loop {
        let frame_start = profile.then(Instant::now);
        let Frame {
            status,
            dt,
            width: w,
            height: h,
            mouse_delta,
        } = host::next_frame()?;
        match status {
            FrameStatus::Closed if smoke => {
                return Err("window closed before sample smoke completed".into())
            }
            FrameStatus::Closed => return Ok(()),
            FrameStatus::Ready => (),
            FrameStatus::Paused => {
                accumulator = 0.;
                continue;
            }
            FrameStatus::DeviceRecreated => {
                assets.forget_lost_device();
                graphics = upload(assets, &cpu)?;
                cube = assets.mesh(&cube_geometry)?;
                bot.rig.instance = SkinInstance::new(&bot.rig.skin, assets, None)?;
                chaser.instance = SkinInstance::new(&chaser.skin, assets, None)?;
                crowd = chaser.instances(assets, chaser.instance.buffer, crowd_count)?;
                hud = None;
                continue;
            }
            FrameStatus::Reload => {
                let documents = host::take_document_edits();
                let next = load_config(documents.as_deref()).and_then(|config| {
                    load_scene(SCENE, documents.as_deref()).map(|scene| (config, scene))
                });
                match next {
                    Ok((next, scene)) => {
                        let reset = config.arena.half_size != next.arena.half_size
                            || config.arena.player_spawn != next.arena.player_spawn
                            || config.arena.range_target != next.arena.range_target
                            || config.arena.walls != next.arena.walls;
                        if scene.meshes != cpu.meshes {
                            let mut replacement = Assets::default();
                            let loaded = upload(&mut replacement, &scene).and_then(|graphics| {
                                let cube = replacement.mesh(&cube_geometry)?;
                                let player =
                                    SkinInstance::new(&bot.rig.skin, &mut replacement, None)?;
                                let enemy =
                                    SkinInstance::new(&chaser.skin, &mut replacement, None)?;
                                let crowd = chaser.instances(
                                    &mut replacement,
                                    enemy.buffer,
                                    crowd_count,
                                )?;
                                Ok((graphics, cube, player, enemy, crowd))
                            });
                            let (next_graphics, next_cube, next_bot, next_chaser, next_crowd) =
                                match loaded {
                                    Ok(loaded) => loaded,
                                    Err(error) => {
                                        replacement.unload();
                                        eprintln!("Arena reload: {error}");
                                        continue;
                                    }
                                };
                            assets.unload();
                            *assets = replacement;
                            graphics = next_graphics;
                            cube = next_cube;
                            bot.rig.instance = next_bot;
                            chaser.instance = next_chaser;
                            crowd = next_crowd;
                            hud = None;
                        }
                        if smoke && documents.is_some() && next.weapon.projectile_speed == 31. {
                            if next.weapon.max_bounces != 1 {
                                return Err("smoke: inline bounce default was not applied".into());
                            }
                            game.reset(&next, true);
                            let input = Input {
                                aim: bank_point(&next),
                                ..Input::default()
                            };
                            game.tick(
                                &next,
                                Input {
                                    fire: true,
                                    ..input
                                },
                            );
                            for _ in 0..52 {
                                game.tick(&next, input);
                            }
                            if game.hits != 1 {
                                return Err(
                                    "smoke: live weapon edit did not reach the next shot".into()
                                );
                            }
                            eprintln!(
                                "Ricochet smoke: inline weapon edit and default applied to the next shot"
                            );
                        }
                        if cpu.scene.camera != scene.scene.camera {
                            editor_camera = scene.scene.camera.clone();
                        }
                        if config.arena.walls.len() != next.arena.walls.len() {
                            selected_wall = None;
                        }
                        config = next;
                        cpu = scene;
                        if reset {
                            game.reset(&config, game.range);
                        }
                    }
                    Err(error) => eprintln!("Arena reload: {error}"),
                }
            }
        }
        if w == 0 || h == 0 {
            continue;
        }
        let play = host::play_session();
        if play_revision != play.revision {
            play_revision = play.revision;
            game = Game::new(&config);
            bot.time = 0.;
            bot.tick = 0;
            bot.moving = false;
        }
        if play.state != PlayState::Playing {
            accumulator = 0.;
            pending_fire = false;
        }
        let controls = host::input(['w', 's', 'a', 'd'], ['t', 'x', '1', '2']);
        let rect = controls.viewport;
        let screen = Vec2::new(w as f32, h as f32);
        let editing = play.state != PlayState::Playing;
        let editor = host::editor_input();
        let authored_camera = &cpu.scene.camera;
        let active_camera = if editing {
            editor.fly_camera(&mut editor_camera, mouse_delta, dt);
            &editor_camera
        } else {
            authored_camera
        };
        let (mut eye, mut view, mut projection) =
            camera(active_camera, authored_camera, &config, screen, rect);
        if editor.deselect
            || (selected_wall.is_some() && !host::project_document_is_open(ARENA_DOCUMENT))
        {
            selected_wall = None;
        }
        if let Some(cursor) = editor.pick.filter(|_| editing) {
            let ray = host::viewport_ray((projection * view).inverse(), cursor, screen, eye);
            let entity = host::pick_entity(
                &cpu,
                eye,
                ray,
                game.tick as f32 * STEP,
                (editor_camera.distance * 2.).max(100.),
            );
            selected_wall = pick_wall(&config.arena.walls, eye, ray)
                .filter(|&(_, distance)| entity.as_ref().is_none_or(|(_, hit)| distance < *hit))
                .map(|(index, _)| index);
            if selected_wall.is_some() {
                host::open_project_document(ARENA_DOCUMENT);
            } else {
                host::select_entity(entity.as_ref().map_or("", |(id, _)| id));
            }
        }
        if editing && editor.frame_selection {
            let bounds = selected_wall
                .and_then(|index| config.arena.walls.get(index))
                .map(wall_bounds)
                .or_else(|| {
                    host::entity_bounds(&cpu, &host::selected_entity(), game.tick as f32 * STEP)
                });
            if let Some(bounds) = bounds {
                let lens = arena_lens(&editor_camera, authored_camera, &config, rect);
                host::frame_camera(&mut editor_camera, bounds, lens);
                (eye, view, projection) =
                    camera(&editor_camera, authored_camera, &config, screen, rect);
            }
        }
        let previous_player = game.player;
        let previous_tick = game.tick;
        if profile {
            if frames % 120 == 0 {
                game.reset(&config, false);
            }
            let aim = game
                .enemies
                .first()
                .map_or(Vec2::ZERO, |enemy| enemy.position);
            for _ in 0..2 {
                game.tick(
                    &config,
                    Input {
                        movement: Vec2::from_angle(frames as f32 * 0.075),
                        aim,
                        fire: true,
                    },
                );
            }
        } else if !smoke && play.state == PlayState::Playing {
            if controls.pressed[0] {
                game.reset(&config, !game.range);
            }
            if controls.pressed[1] {
                game.reset(&config, game.range);
            }
            if controls.pressed[2] || controls.pressed[3] {
                game.upgrade(&config, controls.pressed[2]);
            }
            let movement = Vec2::from_array(controls.movement);
            let aim = if controls.aim {
                ground_point(
                    (projection * view).inverse(),
                    Vec2::from_array(controls.cursor),
                    screen,
                )
                .unwrap_or(game.player + game.facing)
            } else {
                game.player + game.facing
            };
            pending_fire |= controls.primary;
            accumulator += dt.min(0.1);
            let mut muzzle = false;
            while accumulator >= STEP {
                muzzle |= game.tick(
                    &config,
                    Input {
                        movement,
                        aim,
                        fire: pending_fire || controls.primary,
                    },
                );
                pending_fire = false;
                accumulator -= STEP;
            }
            if muzzle {
                sounds.play(0);
            }
            if let Some(effect) = game.sound.take() {
                sounds.play(match effect {
                    ImpactKind::Bounce => 1,
                    ImpactKind::Wall => 2,
                    ImpactKind::Hit => 3,
                    ImpactKind::Destroyed => 4,
                });
            }
        }
        scratch.reset();
        let smoke_wall = smoke && !profile && frames == 2 && !config.arena.walls.is_empty();
        if smoke_wall {
            host::open_project_document(ARENA_DOCUMENT);
        }
        let show_wall_tools =
            editing || (smoke_wall && host::project_document_is_open(ARENA_DOCUMENT));
        let outlined_wall = if smoke_wall && show_wall_tools {
            Some(0)
        } else {
            selected_wall.filter(|_| editing)
        };
        let selected = host::selected_entity();
        let mut list = draws(
            &cpu.scene,
            &graphics,
            &scratch,
            game.tick as f32 * STEP,
            &selected,
        );
        if play.state == PlayState::Stopped {
            arena_draws(
                &mut list,
                &graphics,
                cube,
                &config,
                &game,
                true,
                selected_wall,
            );
            render_view(&list, assets, &cpu.scene, eye, view, projection)?;
            wall_gizmo(&config, selected_wall, projection * view);
            host::present()?;
            continue;
        }
        let moving = game.phase == Phase::Play
            && if game.tick == previous_tick {
                bot.moving
            } else {
                game.tick > previous_tick && game.player.distance_squared(previous_player) > 1e-6
            };
        let skin_start = profile.then(Instant::now);
        let player = bot.draw(assets, &graphics, &game, moving)?;
        list.push(player);
        let mut chasers = game
            .enemies
            .iter()
            .filter(|enemy| enemy.kind == EnemyKind::Chaser)
            .peekable();
        if chasers.peek().is_some() {
            // The crowd shares a walk pose and vertex buffer. Positions and headings are per actor.
            chaser
                .instance
                .sample(&chaser.skin, assets, chaser_walk, game.tick as f32 * STEP)?;
            for enemy in chasers {
                list.push(chaser.draw(
                    &chaser.instance,
                    &graphics,
                    enemy.position,
                    (game.player - enemy.position).normalize_or_zero(),
                    [0.80, 0.21, 0.14, 1.],
                ));
            }
        }
        for (index, instance) in crowd.iter_mut().enumerate() {
            let time = game.tick as f32 * STEP + index as f32 * 0.137;
            instance.sample(&chaser.skin, assets, chaser_walk, time)?;
            let grid = Vec2::new((index % 8) as f32, (index / 8) as f32);
            let position = (grid / 3.5 - Vec2::ONE) * config.arena.half_size * 0.8;
            list.push(chaser.draw(
                instance,
                &graphics,
                position,
                Vec2::Y,
                [0.39, 0.55, 0.85, 1.],
            ));
        }
        let skin_time = skin_start.map(|start| start.elapsed().as_secs_f64() * 1000.);
        arena_draws(
            &mut list,
            &graphics,
            cube,
            &config,
            &game,
            false,
            outlined_wall,
        );
        render_view(&list, assets, &cpu.scene, eye, view, projection)?;
        if show_wall_tools {
            wall_gizmo(&config, outlined_wall, projection * view);
        }
        draw_hud(&mut hud, assets, &config, &game, rect)?;
        let presented = host::present()?;
        if smoke && presented {
            frames += 1;
            if let Some(start) = frame_start {
                if frames > 30 {
                    frame_times.push(start.elapsed().as_secs_f64() * 1000.);
                    skin_times.extend(skin_time);
                }
                if frame_times.len() == 240 {
                    frame_times.sort_by(f64::total_cmp);
                    skin_times.sort_by(f64::total_cmp);
                    eprintln!(
                        "Ricochet frame profile: 240 frames, median {:.3} ms, p95 {:.3} ms (host wall time)",
                        frame_times[120], frame_times[228]
                    );
                    eprintln!("Ricochet animation and draw preparation: median {:.3} ms, p95 {:.3} ms (host)",
                        skin_times[120], skin_times[228]);
                    return Ok(());
                }
            } else if frames == 3 {
                eprintln!("Ricochet smoke: bank shot hit, sample submitted three frames");
                return Ok(());
            }
        }
    }
}

struct AnimatedBot {
    rig: AnimatedMesh,
    idle: usize,
    walk: usize,
    time: f32,
    tick: u64,
    moving: bool,
}

impl AnimatedBot {
    fn load(spec: &SkinnedMesh, assets: &mut Assets) -> Result<Self, String> {
        let rig = AnimatedMesh::load(spec, assets)?;
        Ok(Self {
            idle: rig.clip("idle")?,
            walk: rig.clip("walk")?,
            rig,
            time: 0.,
            tick: 0,
            moving: false,
        })
    }

    fn draw(
        &mut self,
        assets: &Assets,
        graphics: &RenderAssets,
        game: &Game,
        moving: bool,
    ) -> Result<Draw, String> {
        if self.moving != moving || game.tick < self.tick {
            self.time = 0.;
        } else {
            self.time += (game.tick - self.tick) as f32 * STEP;
        }
        self.tick = game.tick;
        self.moving = moving;
        let clip = if self.moving { self.walk } else { self.idle };
        self.rig
            .instance
            .sample(&self.rig.skin, assets, clip, self.time)?;
        Ok(self.rig.draw(
            &self.rig.instance,
            graphics,
            game.player,
            game.facing,
            [0.13, 0.78, 0.57, 1.],
        ))
    }
}

struct AnimatedMesh {
    skin: Skin,
    instance: SkinInstance,
    normalize: Mat4,
    yaw_offset: f32,
    clips: std::collections::BTreeMap<String, usize>,
}

impl AnimatedMesh {
    fn load(spec: &SkinnedMesh, assets: &mut Assets) -> Result<Self, String> {
        let path = &spec.path;
        let mut skin =
            Skin::load(path, host::read_asset).map_err(|error| format!("{path}: {error}"))?;
        let mut clips = std::collections::BTreeMap::new();
        for (slot, source) in &spec.clips {
            let name = match source {
                ClipSource::Embedded(name) => name.clone(),
                ClipSource::Companion(clip) => {
                    let alias = format!("companion:{slot}");
                    skin.add_clip(
                        &alias,
                        &clip.path,
                        clip.animation.as_deref(),
                        host::read_asset,
                    )
                    .map_err(|error| format!("{}: {error}", clip.path))?;
                    alias
                }
            };
            let index = skin
                .clip(&name)
                .ok_or_else(|| format!("{path}: missing animation {name:?} for {slot:?}"))?;
            clips.insert(slot.clone(), index);
        }
        let mut pose = skin.pose();
        skin.sample(None, 0., &mut pose)?;
        let mut min = Vec3::splat(f32::INFINITY);
        let mut max = Vec3::splat(f32::NEG_INFINITY);
        for vertex in &pose.vertices {
            let point = pose
                .mesh_transform
                .transform_point3(Vec3::new(vertex[0], vertex[1], vertex[2]));
            min = min.min(point);
            max = max.max(point);
        }
        let height = max.y - min.y;
        if !height.is_finite() || height <= 1e-4 {
            return Err(format!("{path}: invalid skinned bounds"));
        }
        let center = Vec3::new((min.x + max.x) * 0.5, min.y, (min.z + max.z) * 0.5);
        let normalize =
            Mat4::from_scale(Vec3::splat(spec.height / height)) * Mat4::from_translation(-center);
        let instance = SkinInstance::new(&skin, assets, None)?;
        Ok(Self {
            skin,
            instance,
            normalize,
            yaw_offset: spec.forward.yaw_offset(),
            clips,
        })
    }

    fn clip(&self, slot: &str) -> Result<usize, String> {
        self.clips
            .get(slot)
            .copied()
            .ok_or_else(|| format!("skinned asset is missing clip alias {slot:?}"))
    }

    fn instances(
        &self,
        assets: &mut Assets,
        mesh: Handle<Mesh>,
        count: usize,
    ) -> Result<Vec<SkinInstance>, String> {
        (0..count)
            .map(|_| SkinInstance::new(&self.skin, assets, Some(mesh)))
            .collect()
    }

    fn draw(
        &self,
        instance: &SkinInstance,
        graphics: &RenderAssets,
        position: Vec2,
        direction: Vec2,
        color: [f32; 4],
    ) -> Draw {
        let yaw = direction.x.atan2(direction.y) + self.yaw_offset;
        let actor =
            Mat4::from_rotation_translation(Quat::from_rotation_y(yaw), world(position, 0.03));
        Draw {
            mesh: instance.buffer,
            model: actor * self.normalize * instance.pose.mesh_transform,
            base: color,
            metallic: 0.45,
            roughness: 0.38,
            texture: graphics.textures["fixture/white"],
            mr: graphics.textures["fixture/neutral"],
            normal: None,
            emissive: 0.,
            selected: false,
            palette: instance.gpu.as_ref().map(|(palette, _)| *palette),
        }
    }
}

struct SkinInstance {
    buffer: Handle<Mesh>,
    pose: Pose,
    gpu: Option<(Handle<SkinPalette>, Vec<[f32; 4]>)>,
    uploaded: bool,
}

impl SkinInstance {
    fn new(skin: &Skin, assets: &mut Assets, shared: Option<Handle<Mesh>>) -> Result<Self, String> {
        let (buffer, gpu) =
            if gpu_skinning_available() && std::env::var_os("KALAM_CPU_SKINNING").is_none() {
                let buffer = match shared {
                    Some(buffer) => buffer,
                    None => assets.skinned_mesh(skin)?,
                };
                let palette = assets.skin_palette(skin.joint_count())?;
                (
                    buffer,
                    Some((palette, vec![[0.; 4]; usize::from(skin.joint_count()) * 6])),
                )
            } else {
                (assets.dynamic_mesh(&skin.geometry)?, None)
            };
        Ok(Self {
            buffer,
            gpu,
            pose: skin.palette_pose(),
            uploaded: false,
        })
    }

    fn backend(&self) -> &str {
        if self.gpu.is_some() {
            "GPU"
        } else {
            "CPU"
        }
    }

    fn sample(
        &mut self,
        skin: &Skin,
        assets: &Assets,
        clip: usize,
        time: f32,
    ) -> Result<(), String> {
        if let Some((palette, rows)) = &mut self.gpu {
            if skin.sample_palette(Some(clip), time, &mut self.pose)? || !self.uploaded {
                self.pose.write_gpu_palette(rows)?;
                assets.upload_palette(*palette, rows)?;
            }
        } else if skin.sample(Some(clip), time, &mut self.pose)? || !self.uploaded {
            assets.update_mesh(self.buffer, &self.pose.vertices)?;
        }
        self.uploaded = true;
        Ok(())
    }
}

fn bank_point(config: &Config) -> Vec2 {
    Vec2::new(BANK_POINT, -config.arena.half_size.y)
}

fn camera(
    camera: &Camera,
    authored: &Camera,
    config: &Config,
    screen: Vec2,
    rect: [f32; 4],
) -> (Vec3, Mat4, Mat4) {
    let target = Vec3::from_array(camera.target);
    let eye = target + camera_offset(camera);
    let view = Mat4::look_at_rh(eye, target, Vec3::Y);
    let lens = arena_lens(camera, authored, config, rect);
    (eye, view, host::viewport_projection(lens, screen, rect))
}

fn arena_lens(camera: &Camera, authored: &Camera, config: &Config, rect: [f32; 4]) -> Mat4 {
    let viewport = host::viewport_size(rect);
    let aspect = viewport.x / viewport.y;
    let authored_target = Vec3::from_array(authored.target);
    let authored_view = Mat4::look_at_rh(
        authored_target + camera_offset(authored),
        authored_target,
        Vec3::Y,
    );
    let far = (camera.distance * 2.).max(100.);
    let mut projection = Mat4::perspective_rh(camera.fov_y_degrees.to_radians(), aspect, 0.1, far);
    let extent = config.arena.half_size + Vec2::splat(0.55);
    let fit = [-1., 1.]
        .into_iter()
        .flat_map(|x| [-1., 1.].map(move |y| Vec3::new(x * extent.x, 0., y * extent.y)))
        .map(|point| {
            (projection * authored_view)
                .project_point3(point)
                .abs()
                .truncate()
                .max_element()
        })
        .fold(1., f32::max);
    projection.x_axis /= fit;
    projection.y_axis /= fit;
    projection
}

fn wall_bounds(wall: &Wall) -> [Vec3; 2] {
    let axis = wall.local_rotation().abs();
    let other_axis = Vec2::new(axis.y, axis.x);
    let extent = world(
        axis * wall.half_size.x + other_axis * wall.half_size.y,
        WALL_HALF_HEIGHT,
    );
    let center = world(wall.center, WALL_CENTER_Y);
    [center - extent, center + extent]
}

fn wall_gizmo(config: &Config, selected: Option<usize>, view_projection: Mat4) {
    if let Some((index, wall)) =
        selected.and_then(|index| config.arena.walls.get(index).map(|wall| (index, wall)))
    {
        host::planar_gizmo(
            ARENA_DOCUMENT,
            c"walls",
            index,
            world(wall.center, WALL_CENTER_Y),
            wall.half_size,
            wall.yaw_degrees,
            view_projection,
        );
    }
}

fn pick_wall(walls: &[Wall], eye: Vec3, ray: Vec3) -> Option<(usize, f32)> {
    walls
        .iter()
        .enumerate()
        .filter_map(|(index, wall)| {
            let rotation = wall.local_rotation();
            let origin = world(
                rotation.rotate(Vec2::new(eye.x, eye.z) - wall.center),
                eye.y - WALL_CENTER_Y,
            );
            let ray = world(rotation.rotate(Vec2::new(ray.x, ray.z)), ray.y);
            let half = world(wall.half_size, WALL_HALF_HEIGHT);
            let mut nearest = 0.1_f32;
            let mut farthest = 100_f32;
            for axis in 0..3 {
                let origin = origin[axis];
                if ray[axis].abs() < 1e-6 {
                    if origin.abs() > half[axis] {
                        return None;
                    }
                    continue;
                }
                let a = (-half[axis] - origin) / ray[axis];
                let b = (half[axis] - origin) / ray[axis];
                nearest = nearest.max(a.min(b));
                farthest = farthest.min(a.max(b));
            }
            (nearest <= farthest).then_some((index, nearest))
        })
        .min_by(|a, b| a.1.total_cmp(&b.1))
}

fn verify_wall_picking(config: &Config) -> Result<(), String> {
    let walls = [
        Wall {
            center: Vec2::new(-1.5, 0.5),
            half_size: Vec2::new(0.7, 0.2),
            reflective: true,
            yaw_degrees: 35.,
        },
        Wall {
            center: Vec2::new(1.5, -0.5),
            half_size: Vec2::splat(0.5),
            reflective: false,
            yaw_degrees: 0.,
        },
    ];
    let authored = Camera {
        target: [0.; 3],
        distance: 16.,
        yaw: 0.,
        pitch: 1.18,
        fov_y_degrees: 44.,
    };
    let moved = Camera {
        target: [0.8, 0.1, 1.2],
        yaw: 0.4,
        ..authored.clone()
    };
    let screen = Vec2::new(1280., 800.);
    let (eye, view, projection) =
        camera(&moved, &authored, config, screen, [190., 26., 560., 774.]);
    let view_projection = projection * view;
    for (expected, wall) in walls.iter().enumerate() {
        let point = view_projection.project_point3(world(wall.center, WALL_CENTER_Y));
        let cursor = (point.truncate() * Vec2::new(0.5, -0.5) + Vec2::splat(0.5)) * screen;
        let ray = host::viewport_ray(view_projection.inverse(), cursor, screen, eye);
        if pick_wall(&walls, eye, ray).map(|(index, _)| index) != Some(expected) {
            return Err(
                "smoke: wall picking did not follow the moved camera and docked viewport".into(),
            );
        }
    }
    Ok(())
}

fn ground_point(inverse: Mat4, cursor: Vec2, screen: Vec2) -> Option<Vec2> {
    let ndc = (cursor / screen) * Vec2::new(2., -2.) + Vec2::new(-1., 1.);
    let near = inverse.project_point3(ndc.extend(0.));
    let far = inverse.project_point3(ndc.extend(1.));
    let ray = far - near;
    if ray.y >= -1e-5 {
        return None;
    }
    let on_floor = near - ray * (near.y / ray.y);
    Some(Vec2::new(on_floor.x, on_floor.z))
}

fn cube_geometry() -> Geometry {
    let mut vertices = Vec::with_capacity(24);
    let mut indices = Vec::with_capacity(36);
    for normal in [
        Vec3::X,
        Vec3::NEG_X,
        Vec3::Y,
        Vec3::NEG_Y,
        Vec3::Z,
        Vec3::NEG_Z,
    ] {
        let right = if normal.y.abs() > 0.5 {
            Vec3::X
        } else {
            Vec3::Y
        };
        let up = normal.cross(right);
        let base = vertices.len() as u16;
        for [u, v] in [[0., 0.], [1., 0.], [1., 1.], [0., 1.]] {
            let pos = normal * 0.5 + right * (u - 0.5) + up * (v - 0.5);
            vertices.push([pos.x, pos.y, pos.z, normal.x, normal.y, normal.z, u, v]);
        }
        indices.extend([base, base + 1, base + 2, base, base + 2, base + 3]);
    }
    Geometry { vertices, indices }
}

fn world(position: Vec2, y: f32) -> Vec3 {
    Vec3::new(position.x, y, position.y)
}

fn arena_draws(
    list: &mut FrameVec<'_, Draw>,
    graphics: &RenderAssets,
    cube: Handle<Mesh>,
    config: &Config,
    game: &Game,
    edit_mode: bool,
    selected_wall: Option<usize>,
) {
    let white = graphics.textures["fixture/white"];
    let neutral = graphics.textures["fixture/neutral"];
    let mut shape = |mesh, position, scale, yaw, tint: [f32; 3], selected| {
        list.push(Draw {
            mesh,
            model: Mat4::from_scale_rotation_translation(
                scale,
                glam::Quat::from_rotation_y(yaw),
                position,
            ),
            base: [tint[0], tint[1], tint[2], 1.],
            metallic: 0.12,
            roughness: 0.66,
            texture: white,
            mr: neutral,
            normal: None,
            emissive: 0.,
            selected,
            palette: None,
        });
    };
    let half = config.arena.half_size;
    let steel = [0.22, 0.49, 0.53];
    let cover = [0.27, 0.20, 0.15];
    let seam = [0.10, 0.13, 0.15];
    shape(
        cube,
        Vec3::new(0., -0.28, 0.),
        Vec3::new(half.x * 2. + 0.5, 0.5, half.y * 2. + 0.5),
        0.,
        [0.064, 0.083, 0.10],
        false,
    );
    for x in -(half.x.floor() as i16)..=half.x.floor() as i16 {
        shape(
            cube,
            Vec3::new(f32::from(x), -0.025, 0.),
            Vec3::new(0.012, 0.01, half.y * 2.),
            0.,
            seam,
            false,
        );
    }
    for y in -(half.y.floor() as i16)..=half.y.floor() as i16 {
        shape(
            cube,
            Vec3::new(0., -0.025, f32::from(y)),
            Vec3::new(half.x * 2., 0.01, 0.012),
            0.,
            seam,
            false,
        );
    }
    for sign in [-1., 1.] {
        shape(
            cube,
            Vec3::new(sign * (half.x + 0.09), 0.28, 0.),
            Vec3::new(0.18, 0.6, half.y * 2. + 0.36),
            0.,
            steel,
            false,
        );
        shape(
            cube,
            Vec3::new(0., 0.28, sign * (half.y + 0.09)),
            Vec3::new(half.x * 2., 0.6, 0.18),
            0.,
            steel,
            false,
        );
    }
    for (index, wall) in config.arena.walls.iter().enumerate() {
        shape(
            cube,
            world(wall.center, WALL_CENTER_Y),
            world(wall.half_size * 2., WALL_HALF_HEIGHT * 2.),
            wall.yaw_degrees.to_radians(),
            if wall.reflective { steel } else { cover },
            selected_wall == Some(index),
        );
    }
    if edit_mode {
        return;
    }
    for enemy in game
        .enemies
        .iter()
        .filter(|enemy| enemy.kind == EnemyKind::Turret)
    {
        let direction = (game.player - enemy.position).normalize_or_zero();
        let yaw = direction.x.atan2(direction.y);
        let (size, tint) = (Vec3::new(0.69, 0.43, 0.69), [0.91, 0.49, 0.12]);
        shape(cube, world(enemy.position, 0.23), size, yaw, tint, false);
        shape(
            cube,
            world(enemy.position + direction * 0.23, 0.43),
            Vec3::new(0.16, 0.13, 0.39),
            yaw,
            tint,
            false,
        );
    }
    for shot in &game.shots {
        shape(
            graphics.sphere,
            world(shot.position, 0.24),
            Vec3::splat(0.095),
            0.,
            if shot.hostile {
                [1., 0.27, 0.08]
            } else {
                [0.28, 1., 0.8]
            },
            false,
        );
    }
    for effect in &game.impacts {
        let progress = 1. - effect.remaining / effect.lifetime;
        let radius = (0.12 + progress * 0.23)
            * if effect.kind == ImpactKind::Destroyed {
                2.
            } else {
                1.
            };
        for angle in [
            0.,
            std::f32::consts::TAU / 3.,
            2. * std::f32::consts::TAU / 3.,
        ] {
            let p = effect.position + Vec2::from_angle(angle + progress) * radius;
            let color = effect.color.map(|v| v * (1. - 0.45 * progress));
            shape(
                cube,
                world(p, 0.2 + progress * 0.19),
                Vec3::splat(0.1 * (1. - 0.5 * progress)),
                angle,
                color,
                false,
            );
        }
    }
    if game.range {
        let trace = game.trace(config);
        let tint = if trace.hits_target {
            [0.18, 0.94, 0.51]
        } else {
            [0.27, 0.65, 0.76]
        };
        for segment in trace.points().windows(2) {
            let delta = segment[1] - segment[0];
            if delta.length_squared() < 0.001 {
                continue;
            }
            shape(
                cube,
                world((segment[0] + segment[1]) * 0.5, 0.055),
                Vec3::new(0.018, 0.015, delta.length()),
                delta.x.atan2(delta.y),
                tint,
                false,
            );
        }
        let marker = bank_point(config);
        shape(
            cube,
            world(marker + Vec2::Y * 0.06, 0.08),
            Vec3::new(0.36, 0.08, 0.12),
            0.,
            [0.85, 0.66, 0.29],
            false,
        );
    }
}

#[derive(PartialEq, Eq)]
struct HudState {
    range: bool,
    phase: Phase,
    health: u32,
    wave: usize,
    wave_count: usize,
    count: usize,
    hits: u32,
    bounces: u8,
}

fn draw_hud(
    cached: &mut Option<(HudState, Handle<Texture>)>,
    assets: &mut Assets,
    config: &Config,
    game: &Game,
    rect: [f32; 4],
) -> Result<(), String> {
    let state = HudState {
        range: game.range,
        phase: game.phase,
        health: game.health as u32,
        wave: game.wave + 1,
        wave_count: config.waves.len(),
        count: game.enemies.len(),
        hits: if game.range { game.hits } else { 0 },
        bounces: game.weapon(config).max_bounces,
    };
    if cached.as_ref().is_none_or(|(old, _)| old != &state) {
        let mut image = HudImage(vec![255; (HUD_WIDTH * HUD_HEIGHT * 4) as usize]);
        image.fill(0, 0, HUD_WIDTH, HUD_HEIGHT, [17, 24, 34, 248]);
        image.fill(0, 0, HUD_WIDTH, 3, [60, 191, 169, 255]);
        let (title, instruction) = if state.range {
            (
                format!("TEST RANGE    HITS {}", state.hits),
                "Aim at gold wall marker for a bank.",
            )
        } else {
            match state.phase {
                Phase::Play => (
                    format!("SCRAPYARD     WAVE {}/{}", state.wave, state.wave_count),
                    "WASD move   Mouse aim   LMB fire",
                ),
                Phase::Upgrade => ("WAVE CLEARED".into(), "1 Extra bounce   2 Faster shots"),
                Phase::Win => (
                    "YARD CLEARED".into(),
                    "All waves cleared. Press X to replay.",
                ),
                Phase::Loss => ("BOT DISABLED".into(), "Press X to restart the run."),
            }
        };
        image.text(12, 10, &title, [137, 240, 219, 255]);
        image.text(
            12,
            31,
            &format!(
                "HP {}    DRONES {}    BOUNCES {}",
                state.health, state.count, state.bounces
            ),
            [234, 238, 245, 255],
        );
        image.text(12, 51, instruction, [234, 238, 245, 255]);
        image.text(
            12,
            69,
            "T Toggle test range   X Reset",
            [166, 182, 199, 255],
        );
        let texture = assets.texture(&image.0, HUD_WIDTH, HUD_HEIGHT, false)?;
        if let Some((_, previous)) = cached.replace((state, texture)) {
            assets.drop_texture(previous);
        }
    }
    let texture = cached
        .as_ref()
        .map(|(_, handle)| *handle)
        .ok_or("arena HUD texture missing")?;
    let width = rect[2].min(HUD_WIDTH as f32);
    let height = width * HUD_HEIGHT as f32 / HUD_WIDTH as f32;
    let x = rect[0] + (rect[2] - width) * 0.5;
    let y = (rect[1] + rect[3] - height - 12.).max(rect[1]);
    assets.overlay(texture, [x as u16, y as u16, width as u16, height as u16])
}

struct Sounds([Vec<f32>; 5]);

impl Sounds {
    fn new() -> Self {
        Self(
            [
                (500., 240., 0.055),
                (1150., 840., 0.055),
                (220., 90., 0.06),
                (720., 310., 0.07),
                (360., 65., 0.15),
            ]
            .map(|(start, end, duration)| {
                let count = (duration * 24_000.) as usize;
                (0..count)
                    .map(|i| {
                        let t = i as f32 / 24_000.;
                        let frequency_slope = (end - start) / duration;
                        let phase = std::f32::consts::TAU * t * (start + 0.5 * frequency_slope * t);
                        let envelope = (t * 350.).min(1.) * (1. - t / duration).max(0.).powi(2);
                        phase.sin() * envelope * 0.14
                    })
                    .collect()
            }),
        )
    }

    fn play(&self, index: usize) {
        let sound = &self.0[index];
        host::play_audio(sound);
    }
}
