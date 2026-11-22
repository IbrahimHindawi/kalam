use crate::{material::Materials, mesh::Geometry, scene::Scene};
use std::collections::{BTreeMap, HashMap};

pub struct CpuScene {
    pub scene: Scene,
    pub meshes: BTreeMap<String, Geometry>,
}

pub fn load_scene_with_overrides(
    name: &str,
    overrides_json: &[u8],
    mut read: impl FnMut(&str, usize) -> Result<Vec<u8>, String>,
) -> Result<CpuScene, String> {
    let overrides: HashMap<String, String> =
        serde_json::from_slice(overrides_json).map_err(|error| error.to_string())?;
    load_scene(name, |path, limit| {
        if let Some(contents) = overrides.get(path) {
            if contents.len() > limit {
                return Err(format!("{path}: exceeds {} KiB", limit / 1024));
            }
            return Ok(contents.as_bytes().to_vec());
        }
        read(path, limit)
    })
}

pub fn load_scene(
    name: &str,
    mut read: impl FnMut(&str, usize) -> Result<Vec<u8>, String>,
) -> Result<CpuScene, String> {
    if !crate::scene::valid_name(name) {
        return Err("invalid scene name".into());
    }
    let textures = |id: &str| {
        matches!(
            id,
            "fixture/white" | "fixture/asymmetric" | "fixture/neutral"
        )
    };
    let mut materials = Materials::default();
    let material = read("materials/reference.json", 64 * 1024)?;
    materials
        .replace("reference", &material, textures)
        .map_err(|error| format!("materials/reference.json: {error}"))?;
    let path = format!("scenes/{name}.json");
    let contents = read(&path, 256 * 1024)?;
    let scene = Scene::parse(name, &contents, &materials, textures, &mut read)
        .map_err(|error| format!("{path}: {error}"))?;
    let mut meshes = BTreeMap::new();
    for (id, file) in &scene.meshes {
        // Scene validation permits a filename only, so it cannot escape meshes/.
        let path = format!("meshes/{file}");
        let data = read(&path, 8 * 1024 * 1024)?;
        let mesh = crate::glb::decode(&data).map_err(|error| format!("meshes/{file}: {error}"))?;
        meshes.insert(id.clone(), mesh);
    }
    Ok(CpuScene { scene, meshes })
}
