use super::*;
use serde_json::{json, Value};

fn asset(effects: Value) -> Arc<FxAsset> {
    let file = json!({"version":1,"name":"test","duration":1.0,"effects":effects});
    Arc::new(FxAsset::parse(&serde_json::to_vec(&file).unwrap()).unwrap())
}
fn quads(fx: &FxWorld) -> Vec<([Vec3; 4], [f32; 4], [f32; 4])> {
    let mut list = Vec::new();
    fx.visit(Vec3::X, Vec3::Y, |draw| {
        if let FxDraw::Quad {
            corners, uv, color, ..
        } = draw
        {
            list.push((corners, uv, color));
        }
    });
    list
}

#[test]
fn burst_steps_through_four_atlas_cells_and_outlives_its_emitter() {
    let data = br#"{"version":1,"name":"impact","standalone":true,"start_frame":1,"end_frame":12,
        "effects":[{"name":"flash","type":"particle","texture_flip_u":1,"render_on_top":true,
        "particle_emit_count":0,"particle_burst_count":1,"particle_sheet_columns":2,"particle_sheet_rows":2,
        "particle_sheet_frame_count":4,"particle_life_min":0.4,"particle_life_max":0.4,
        "particle_color_end":[1,1,1,1],"particle_size_start":[1.5,1.5],"particle_size_end":[1.5,1.5]}]}"#;
    let asset = Arc::new(FxAsset::parse(data).unwrap());
    assert!((asset.seconds() - 0.2).abs() < 0.00001);
    let mut world = FxWorld::default();
    let id = world
        .spawn(asset, FxAnchor::point(Vec3::ZERO), 0, false)
        .unwrap();
    world.step(0.01);
    let values = quads(&world);
    assert_eq!(values.len(), 1);
    assert_eq!(values[0].1, [0.5, 0.0, 0.0, 0.5]);
    world.step(0.2);
    assert!(world.alive(id));
    assert!(!world.set_anchor(id, FxAnchor::point(Vec3::X)));
    assert_eq!(quads(&world)[0].1, [0.5, 0.5, 0.0, 1.0]);
    world.step(0.21);
    assert!(!world.alive(id));
}

#[test]
fn windows_fire_between_updates_and_loops_do_not_repeat_full_window_bursts() {
    let data = asset(json!([
        {"type":"particle","particle_emit_count":0,"particle_burst_count":1,
         "particle_life_min":4,"particle_life_max":4,"emission_window_start_normalized":0.2,"emission_window_end_normalized":0.21},
        {"type":"particle","particle_emit_count":0,"particle_burst_count":1,"particle_life_min":4,"particle_life_max":4}
    ]));
    let mut world = FxWorld::default();
    let id = world
        .spawn(data, FxAnchor::point(Vec3::X), 99, true)
        .unwrap();
    world.step(0.15);
    assert_eq!(quads(&world).len(), 1);
    world.step(1.2);
    assert_eq!(quads(&world).len(), 3);
    world.release(id);
    world.step(0.5);
    assert_eq!(quads(&world).len(), 3);
    world.step(4.0);
    assert!(!world.alive(id));
}

#[test]
fn seeded_travel_and_color_are_independent_of_frame_subdivision() {
    let data = asset(
        json!([{"type":"particle","particle_burst_count":3,"emit_interval":0.08,
        "particle_life_min":0.9,"particle_life_max":0.9,"particle_acceleration":[0,-4,0]}]),
    );
    let sample = |steps: usize| {
        let mut world = FxWorld::default();
        let id = world
            .spawn(data.clone(), FxAnchor::point(Vec3::ZERO), 1234, false)
            .unwrap();
        for n in 1..=steps {
            world.set_anchor(id, FxAnchor::point(Vec3::X * (n as f32 / steps as f32)));
            world.step(0.25 / steps as f32);
        }
        quads(&world)
    };
    let a = sample(1);
    let b = sample(4);
    assert_eq!(a.len(), 7);
    assert_eq!(a.len(), b.len());
    for ((ca, ua, c1), (cb, ub, c2)) in a.into_iter().zip(b) {
        assert_eq!(ua, ub);
        assert!(ca.iter().zip(cb).all(|(a, b)| a.distance(b) < 0.00001));
        assert!(c1.iter().zip(c2).all(|(a, b)| (a - b).abs() < 0.00001));
    }
}

#[test]
fn two_socket_edges_sweep_and_mesh_uses_current_attachment_pose() {
    let data = asset(
        json!([{"type":"ribbon","emit_interval":0.01,"sample_life":0.2,"max_sample_gap":0.2},
        {"type":"mesh","mesh_translate":[0,0,1],"mesh_scale":[2,2,2]}]),
    );
    let mut world = FxWorld::default();
    let id = world
        .spawn(data, FxAnchor::segment(Vec3::ZERO, Vec3::Y), 0, false)
        .unwrap();
    let moving = FxAnchor::segment(Vec3::X, Vec3::X + Vec3::Y);
    world.set_anchor(id, moving);
    world.set_layer_anchor(id, 1, Some(FxAnchor::segment(Vec3::Y, Vec3::Y + Vec3::X)));
    world.step(0.1);
    let ribbon = quads(&world);
    assert!(ribbon.len() >= 8);
    assert!(ribbon[0].0[0].distance(Vec3::ZERO) < 1e-5);
    assert!(ribbon.last().unwrap().0[1].x > 0.8);
    world.visit(Vec3::X, Vec3::Y, |draw| {
        if let FxDraw::Mesh { model, .. } = draw {
            assert!(
                model
                    .transform_point3(Vec3::ZERO)
                    .distance(Vec3::new(1.0, 1.0, 0.0))
                    < 1e-5
            );
        }
    });
    world.stop(id);
    assert!(quads(&world).is_empty());
}

#[test]
fn bounded_pools_keep_capacity_and_old_handles_never_control_new_sources() {
    let a = asset(
        json!([{"type":"particle","particle_burst_count":128,"particle_emit_count":128,
        "emit_interval":0.001,"particle_life_min":1,"particle_life_max":1}]),
    );
    let mut world = FxWorld::default();
    let old = world
        .spawn(a.clone(), FxAnchor::point(Vec3::ZERO), 0, true)
        .unwrap();
    world.step(0.5);
    assert_eq!(world.particles.len(), MAX_PARTICLES);
    assert_eq!(world.particles.capacity(), MAX_PARTICLES);
    world.clear();
    let new = world
        .spawn(a, FxAnchor::point(Vec3::ZERO), 0, false)
        .unwrap();
    assert_ne!(old, new);
    world.stop(old);
    assert!(world.alive(new));
}

#[test]
fn rejects_invalid_ranges_and_sheet_layouts_before_playback() {
    for layer in [
        json!({"type":"particle","particle_sheet_columns":0}),
        json!({"type":"ribbon","emit_interval":0}),
        json!({"type":"particle","particle_life_min":3,"particle_life_max":2}),
        json!({"type":"particle","particle_sheet_frame_count":2}),
        json!({"type":"mesh","emission_window_start_normalized":0.9,"emission_window_end_normalized":0.3}),
        json!({"type":"particle","render_on_top":2}),
    ] {
        let file = json!({"version":1,"name":"invalid","effects":[layer]});
        assert!(FxAsset::parse(file.to_string().as_bytes()).is_err());
    }
}
