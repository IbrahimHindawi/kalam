use crate::entities::{Components, Entities, EntityDocument};
use crate::inline_fields;
use crate::material::Materials;
use crate::walk::WalkConfig;
use glam::Vec3;
use serde::Deserialize;
use std::collections::{BTreeMap, HashSet};

const MAX_ENTITIES: usize = 512;
const MAX_MESH_ASSETS: usize = 32;

pub enum MeshKind {
    Floor,
    Sphere,
    Imported(String),
}

#[derive(Clone, Deserialize, PartialEq)]
#[serde(deny_unknown_fields)]
pub struct Camera {
    pub target: [f32; 3],
    pub distance: f32,
    pub yaw: f32,
    pub pitch: f32,
    pub fov_y_degrees: f32,
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Sun {
    pub direction: [f32; 3],
    pub radiance: [f32; 3],
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct SceneDocument {
    version: u32,
    camera: Camera,
    sun: Sun,
    exposure: f32,
    #[serde(default)]
    meshes: BTreeMap<String, String>,
    entities: Vec<String>,
    #[serde(default)]
    archetypes: BTreeMap<String, Components>,
    walk: Option<WalkConfig>,
}

pub struct Scene {
    pub camera: Camera,
    pub sun: Sun,
    pub exposure: f32,
    pub entities: Entities,
    pub meshes: BTreeMap<String, String>,
    pub walk: Option<WalkConfig>,
    pub exposure_range: (f32, f32),
    pub walk_speed_range: (f32, f32),
}

impl Scene {
    pub const EXPOSURE_RANGE: (f32, f32) = (0.01, 32.);

    pub fn set_exposure(&mut self, value: f32) -> Result<(), &'static str> {
        Self::validate_exposure(value)?;
        if !(self.exposure_range.0..=self.exposure_range.1).contains(&value) {
            return Err("exposure: outside the authored range");
        }
        self.exposure = value;
        Ok(())
    }

    fn validate_exposure(value: f32) -> Result<(), &'static str> {
        let (min, max) = Self::EXPOSURE_RANGE;
        (min..=max)
            .contains(&value)
            .then_some(())
            .ok_or("exposure: expected 0.01..32")
    }

    pub(crate) fn parse(
        name: &str,
        json: &[u8],
        materials: &Materials,
        texture_exists: impl Fn(&str) -> bool,
        mut read: impl FnMut(&str, usize) -> Result<Vec<u8>, String>,
    ) -> Result<Self, String> {
        let (doc, (exposure_range, walk_speed_range)): (SceneDocument, _) =
            inline_fields::decode_with(json, |raw| {
                (
                    inline_fields::number_range(raw, "/exposure", Self::EXPOSURE_RANGE),
                    inline_fields::number_range(raw, "/walk/speed", WalkConfig::SPEED_RANGE),
                )
            })?;
        if doc.version != 1 {
            return Err(format!("version: unsupported {}", doc.version));
        }
        if !bounded(doc.camera.target, -1000., 1000.)
            || !(0.25..=50.).contains(&doc.camera.distance)
            || !(-360.0..=360.0).contains(&doc.camera.yaw)
            || !(0.1..=1.5).contains(&doc.camera.pitch)
            || !(15.0..=100.0).contains(&doc.camera.fov_y_degrees)
        {
            return Err("camera: invalid target, distance, orbit angles or field of view".into());
        }
        let sun = Vec3::from_array(doc.sun.direction);
        if !bounded(doc.sun.direction, -1000., 1000.) || sun.length_squared() < 1e-8 {
            return Err("sun.direction: expected a finite nonzero direction".into());
        }
        if !bounded(doc.sun.radiance, 0., 64.) {
            return Err("sun.radiance: expected three values in 0..64".into());
        }
        Self::validate_exposure(doc.exposure)?;

        if let Some(walk) = &doc.walk {
            walk.validate()?;
        }
        if doc.meshes.len() > MAX_MESH_ASSETS {
            return Err(format!("meshes: maximum {MAX_MESH_ASSETS} assets"));
        }
        for (id, file) in &doc.meshes {
            if !valid_name(id)
                || matches!(id.as_str(), "floor" | "sphere")
                || !file.strip_suffix(".glb").is_some_and(valid_name)
            {
                return Err(format!(
                    "meshes.{id}: expected a unique mesh ID and a .glb filename"
                ));
            }
        }
        if doc.entities.len() > MAX_ENTITIES {
            return Err(format!("entities: maximum {MAX_ENTITIES}"));
        }
        let mut ids = HashSet::new();
        let mut documents = Vec::with_capacity(doc.entities.len());
        for id in &doc.entities {
            if !valid_name(id) || !ids.insert(id.to_ascii_lowercase()) {
                return Err(format!("entities: invalid or duplicate ID {id:?}"));
            }
            let path = format!("scenes/{name}/{id}.json");
            let bytes = read(&path, 256 * 1024)?;
            let entity: EntityDocument =
                inline_fields::decode(&bytes).map_err(|error| format!("{path}: {error}"))?;
            if entity.id != *id {
                return Err(format!("{path}: id must match the manifest entry {id:?}"));
            }
            documents.push((path, entity));
        }
        let entities = Entities::load(
            documents,
            &doc.archetypes,
            &doc.meshes,
            materials,
            &texture_exists,
        )?;
        Ok(Self {
            camera: doc.camera,
            exposure: doc.exposure,
            sun: doc.sun,
            entities,
            meshes: doc.meshes,
            walk: doc.walk,
            exposure_range,
            walk_speed_range,
        })
    }
}

pub fn valid_name(name: &str) -> bool {
    !name.is_empty()
        && name.len() <= 64
        && name
            .bytes()
            .all(|byte| byte.is_ascii_alphanumeric() || matches!(byte, b'_' | b'-'))
}

pub(crate) fn bounded(values: [f32; 3], min: f32, max: f32) -> bool {
    values.iter().all(|value| (min..=max).contains(value))
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::entities::Transform;

    const FIXTURE: &str = include_str!("../../assets/scenes/reference.json");
    fn parse(json: &str, overrides: &[(&str, Vec<u8>)]) -> Result<Scene, String> {
        let mut materials = Materials::default();
        materials.replace(
            "reference",
            include_bytes!("../../assets/materials/reference.json"),
            |_| true,
        )?;
        Scene::parse(
            "reference",
            json.as_bytes(),
            &materials,
            |id| matches!(id, "fixture/asymmetric" | "fixture/neutral"),
            |path, _| {
                if let Some((_, bytes)) = overrides.iter().find(|(file, _)| *file == path) {
                    return Ok(bytes.clone());
                }
                std::fs::read(
                    std::path::Path::new(env!("CARGO_MANIFEST_DIR"))
                        .join("../assets")
                        .join(path),
                )
                .map_err(|error| format!("{path}: {error}"))
            },
        )
    }

    #[test]
    fn packaged_scene_resolves_the_reference_material() {
        let scene = parse(FIXTURE, &[]).unwrap();
        assert_eq!(scene.entities.order.len(), 17);
        let entity = scene.entities.order[16];
        assert_eq!(
            scene
                .entities
                .world
                .get::<&crate::entities::EntityId>(entity)
                .unwrap()
                .0,
            "textured-sphere"
        );
        let transform = scene.entities.world.get::<&Transform>(entity).unwrap();
        assert_eq!(
            transform.model(0.).transform_point3(Vec3::ZERO),
            Vec3::new(0., 1., 3.5)
        );
        let renderable = scene
            .entities
            .world
            .get::<&crate::entities::Renderable>(entity)
            .unwrap();
        assert_eq!(
            renderable.material.base_texture.as_deref(),
            Some("fixture/asymmetric")
        );
    }

    #[test]
    fn invalid_edits_fail_before_the_renderer_uses_the_scene() {
        let fixture: serde_json::Value = serde_json::from_str(FIXTURE).unwrap();
        for (pointer, replacement, field) in [
            ("/entities/1", serde_json::json!("ground"), "duplicate ID"),
            ("/entities/1", serde_json::json!("../ground"), "invalid"),
            (
                "/entities/1",
                serde_json::json!("missing"),
                "scenes/reference/missing.json",
            ),
            ("/exposure", serde_json::json!(0), "exposure"),
        ] {
            let mut json = fixture.clone();
            *json.pointer_mut(pointer).unwrap() = replacement;
            let error = parse(&json.to_string(), &[])
                .err()
                .expect("invalid scene accepted");
            assert!(error.contains(field), "{pointer}: {error}");
        }
        let file = "scenes/reference/textured-sphere.json";
        let original: serde_json::Value = serde_json::from_str(include_str!(
            "../../assets/scenes/reference/textured-sphere.json"
        ))
        .unwrap();
        for (pointer, replacement, field) in [
            ("/id", serde_json::json!("ground"), "id must match"),
            (
                "/components/render/material",
                serde_json::json!("missing"),
                "unknown material",
            ),
            (
                "/components/transform",
                serde_json::json!({"position":[0, 1, 3.5], "scale":0}),
                "transform",
            ),
        ] {
            let mut entity = original.clone();
            *entity.pointer_mut(pointer).unwrap() = replacement;
            let error = parse(FIXTURE, &[(file, serde_json::to_vec(&entity).unwrap())])
                .err()
                .expect("invalid entity accepted");
            assert!(
                error.contains(file) && error.contains(field),
                "{pointer}: {error}"
            );
        }
    }

    #[test]
    fn native_and_scripted_lens_edits_obey_authored_bounds() {
        let mut manifest: serde_json::Value = serde_json::from_str(FIXTURE).unwrap();
        manifest["exposure"] = serde_json::json!({
            "type": "number", "default": 1.5, "min": 1.25, "max": 2
        });
        let mut scene = parse(&manifest.to_string(), &[]).unwrap();
        let binding = crate::lens::Binding::Exposure;
        assert_eq!(scene.exposure, 1.5);
        assert_eq!(binding.range(&scene), (1.25, 2.));
        assert!(binding.write(&mut scene, 3.).is_err());
        binding.write(&mut scene, 2.).unwrap();
        assert_eq!(scene.exposure, 2.);
    }
}
