use glam::Vec2;
use kalam_ricochet::{Config, Game, ImpactKind, Input, Phase, Wave, DOCUMENTS};

fn source(path: &str) -> Result<Vec<u8>, String> {
    let root = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("../../engine/assets");
    std::fs::read(root.join(path)).map_err(|e| e.to_string())
}

fn shot(game: &mut Game, config: &Config, aim: Vec2, ticks: u64) -> Option<u64> {
    let start = game.hits;
    let input = Input {
        aim,
        ..Input::default()
    };
    assert!(game.tick(
        config,
        Input {
            fire: true,
            ..input
        }
    ));
    let start_tick = game.tick;
    (1..ticks).find_map(|_| {
        game.tick(config, input);
        (game.hits > start).then_some(game.tick - start_tick + 1)
    })
}

#[test]
fn cover_blocks_a_direct_shot_and_the_bank_is_reproducible() {
    let config = Config::load(source).unwrap();
    let mut game = Game::new(&config);
    game.reset(&config, true);
    assert_eq!(
        shot(&mut game, &config, config.arena.range_target, 120),
        None
    );
    game.reset(&config, true);
    let aim = Vec2::new(-0.5, -config.arena.half_size.y);
    game.tick(
        &config,
        Input {
            aim,
            ..Input::default()
        },
    );
    assert!(game.trace(&config).hits_target);
    assert_eq!(shot(&mut game, &config, aim, 120), Some(71));
}

#[test]
fn a_live_bounce_limit_applies_to_the_next_shot() {
    let config = Config::load(source).unwrap();
    let mut game = Game::new(&config);
    game.reset(&config, true);
    let path = DOCUMENTS[1].1;
    let json = r#"{"projectile_speed":140,"max_bounces":0,"damage":34,"shots_per_second":4}"#;
    let overrides = serde_json::to_vec(&std::collections::BTreeMap::from([(path, json)])).unwrap();
    let mut next = Config::load_overrides(Some(&overrides), source).unwrap();
    let aim = Vec2::new(-0.5, -config.arena.half_size.y);
    assert_eq!(shot(&mut game, &next, aim, 120), None);
    assert!(!game.trace(&next).hits_target);
    next.weapon.max_bounces = 1;
    let hit = shot(&mut game, &next, aim, 40).expect("fast shot crossed the entire bank path");
    assert!(hit < 15);
    assert!(game
        .impacts
        .iter()
        .any(|impact| impact.kind == ImpactKind::Hit));
}

#[test]
fn authored_limits_can_change_but_capacity_is_checked() {
    let path = DOCUMENTS[1].1;
    let mut weapon = serde_json::json!({
        "projectile_speed": { "type": "number", "default": 200, "min": 1, "max": 250 },
        "max_bounces": { "type": "integer", "value": 4, "min": 0, "max": 8 },
        "damage": 34,
        "shots_per_second": 4
    });
    let load = |weapon: &serde_json::Value| {
        let overrides = serde_json::to_vec(&std::collections::BTreeMap::from([(
            path,
            weapon.to_string(),
        )]))
        .unwrap();
        Config::load_overrides(Some(&overrides), source)
    };
    assert_eq!(load(&weapon).unwrap().weapon.projectile_speed, 200.);
    weapon["max_bounces"]["max"] = 3.into();
    assert!(load(&weapon)
        .err()
        .unwrap()
        .contains("$.max_bounces: value is outside"));
    weapon["max_bounces"]["max"] = 10.into();
    weapon["max_bounces"]["value"] = 9.into();
    assert!(load(&weapon)
        .err()
        .unwrap()
        .contains("max_bounces: exceeds the runtime limit"));
}

#[test]
fn arena_fields_allow_layout_edits_and_rotated_cover_reflects_shots() {
    let path = DOCUMENTS[0].1;
    let mut arena: serde_json::Value = serde_json::from_slice(&source(path).unwrap()).unwrap();
    arena["player_spawn"]["value"] = serde_json::json!([-2., 0.]);
    arena["range_target"]["value"] = serde_json::json!([8., 0.]);
    arena["walls"] = serde_json::json!([{
        "center": [0., 0.],
        "half_size": [0.2, 1.],
        "reflective": true,
        "yaw_degrees": 45.
    }]);
    let overrides = serde_json::to_vec(&std::collections::BTreeMap::from([(
        path,
        arena.to_string(),
    )]))
    .unwrap();
    let config = Config::load_overrides(Some(&overrides), source).unwrap();
    assert_eq!(config.arena.range_target, Vec2::new(8., 0.));
    let mut game = Game::new(&config);
    game.reset(&config, true);
    game.tick(
        &config,
        Input {
            aim: Vec2::X,
            ..Input::default()
        },
    );
    let trace = game.trace(&config);
    let points = trace.points();
    assert!(points.len() >= 3);
    let reflected = (points[2] - points[1]).normalize();
    assert!(reflected.abs_diff_eq(Vec2::Y, 1e-3), "{reflected:?}");
}

#[test]
fn an_upgrade_persists_across_waves_until_restart() {
    let mut config = Config::load(source).unwrap();
    config.waves = (0..3)
        .map(|_| Wave {
            chasers: 0,
            turrets: 1,
        })
        .collect();
    config.weapon.damage = 100.;
    let aim = Vec2::new(-0.5, -config.arena.half_size.y);
    let mut game = Game::new(&config);
    for wave in 0..3 {
        assert_eq!(game.wave, wave);
        assert!(shot(&mut game, &config, aim, 120).is_some());
        if wave < 2 {
            assert!(game.phase == Phase::Upgrade);
            game.upgrade(&config, wave == 0);
        }
    }
    assert!(game.phase == Phase::Win);
    assert_eq!(
        game.weapon(&config).max_bounces,
        config.weapon.max_bounces + 1
    );
    assert!(game.weapon(&config).projectile_speed > config.weapon.projectile_speed);
    game.reset(&config, false);
    assert_eq!(game.weapon(&config).max_bounces, config.weapon.max_bounces);
    assert_eq!(
        game.weapon(&config).projectile_speed,
        config.weapon.projectile_speed
    );
}
