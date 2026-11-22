use glam::Vec2;
use kalam_ricochet::{Config, Game, ImpactKind, Input};
use std::alloc::{GlobalAlloc, Layout, System};
use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering::Relaxed};
use std::{path::PathBuf, time::Instant};

struct ProfileAllocator;
static RECORD: AtomicBool = AtomicBool::new(false);
static ALLOCATIONS: AtomicUsize = AtomicUsize::new(0);

// The benchmark process uses System normally. Count requests only while a
// measured tick is running, including requests to grow an existing allocation.
unsafe impl GlobalAlloc for ProfileAllocator {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        if RECORD.load(Relaxed) {
            ALLOCATIONS.fetch_add(1, Relaxed);
        }
        unsafe { System.alloc(layout) }
    }

    unsafe fn dealloc(&self, pointer: *mut u8, layout: Layout) {
        unsafe { System.dealloc(pointer, layout) }
    }

    unsafe fn realloc(&self, pointer: *mut u8, layout: Layout, size: usize) -> *mut u8 {
        if RECORD.load(Relaxed) {
            ALLOCATIONS.fetch_add(1, Relaxed);
        }
        unsafe { System.realloc(pointer, layout, size) }
    }
}

#[global_allocator]
static ALLOCATOR: ProfileAllocator = ProfileAllocator;

fn main() -> Result<(), String> {
    let root = std::env::args_os()
        .nth(1)
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from("engine/assets"));
    let mut config =
        Config::load(|path| std::fs::read(root.join(path)).map_err(|e| format!("{path}: {e}")))?;
    let aim = Vec2::new(-0.5, -config.arena.half_size.y);
    let mut game = Game::new(&config);
    game.reset(&config, true);
    let input = Input {
        aim,
        ..Input::default()
    };
    game.tick(
        &config,
        Input {
            fire: true,
            ..input
        },
    );
    let mut bounce_tick = None;
    let hit_tick = (0..360).find_map(|_| {
        game.tick(&config, input);
        if game
            .impacts
            .iter()
            .any(|impact| impact.kind == ImpactKind::Bounce)
        {
            bounce_tick.get_or_insert(game.tick);
        }
        (game.hits == 1).then_some(game.tick)
    });
    let (Some(bounce), Some(hit)) = (bounce_tick, hit_tick) else {
        return Err("reference ricochet did not bounce and hit".into());
    };
    if bounce >= hit {
        return Err("target hit occurred before the bank shot".into());
    }
    let busiest = config
        .waves
        .iter()
        .enumerate()
        .max_by_key(|(_, wave)| wave.chasers + wave.turrets)
        .map(|(index, _)| index)
        .unwrap_or(0);
    config.waves.rotate_left(busiest);
    game.reset(&config, false);
    let actors = game.enemies.len() + 1;
    let mut samples = Vec::with_capacity(10_000);
    for index in 0..10_000 {
        if index % 240 == 0 {
            game.reset(&config, false);
        }
        let begin = Instant::now();
        RECORD.store(true, Relaxed);
        game.tick(
            &config,
            Input {
                movement: Vec2::from_angle(index as f32 * 0.025),
                fire: true,
                ..input
            },
        );
        RECORD.store(false, Relaxed);
        samples.push(begin.elapsed().as_nanos() as u64);
    }
    samples.sort_unstable();
    println!(
        "{}",
        serde_json::json!({
            "bounce_tick": bounce,
            "hit_tick": hit,
            "tick_hz": kalam_ricochet::TICK_HZ,
            "profile": { "samples": samples.len(), "actors_at_start": actors,
                "median_ns": samples[5000], "p95_ns": samples[9500],
                "tick_allocations": ALLOCATIONS.load(Relaxed) }
        })
    );
    Ok(())
}
