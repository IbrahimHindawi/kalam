use kalam_runtime::skin::Skin;

#[test]
fn generated_asset_has_two_clips_and_deforms_positions_and_normals() {
    let skin = Skin::decode(include_bytes!(
        "../../../engine/assets/samples/ricochet/repair-bot.glb"
    ))
    .unwrap();
    let walk = skin.clip("walk").unwrap();
    assert!(skin.clip("idle").is_some());
    let mut pose = skin.pose();
    skin.sample(Some(walk), 0.0, &mut pose).unwrap();
    let start = pose.vertices.clone();
    skin.sample(Some(walk), 0.4, &mut pose).unwrap();
    assert!(pose
        .vertices
        .iter()
        .zip(start)
        .any(|(a, b)| { (a[0] - b[0]).abs() > 1e-4 && (a[3] - b[3]).abs() > 1e-4 }));
    assert!(pose.vertices.iter().flatten().all(|v| v.is_finite()));
}

#[test]
fn generated_chaser_has_a_moving_walk_pose() {
    let bytes = include_bytes!("../../../engine/assets/samples/ricochet/chaser-bot.glb");
    let skin = Skin::decode(bytes).unwrap();
    let walk = skin.clip("walk").unwrap();
    let mut pose = skin.pose();
    skin.sample(Some(walk), 0., &mut pose).unwrap();
    let start = pose.vertices.clone();
    skin.sample(Some(walk), 0.3, &mut pose).unwrap();
    assert!(pose
        .vertices
        .iter()
        .zip(start)
        .any(|(a, b)| (a[1] - b[1]).abs() > 1e-4));
    assert!(pose.vertices.iter().flatten().all(|v| v.is_finite()));
}
