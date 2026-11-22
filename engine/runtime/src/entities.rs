use crate::{
    material::{Material, Materials},
    scene::{bounded, MeshKind},
};
use glam::{EulerRot, Mat4, Quat, Vec3};
use hecs::{Entity, World};
use serde::{Deserialize, Serialize};
use std::collections::BTreeMap;

#[derive(Clone, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct Transform {
    pub position: [f32; 3],
    #[serde(default)]
    pub scale: Scale,
    #[serde(default)]
    pub yaw_degrees: f32,
    #[serde(default, skip_serializing_if = "zero")]
    pub pitch_degrees: f32,
    #[serde(default, skip_serializing_if = "zero")]
    pub roll_degrees: f32,
}
fn zero(value: &f32) -> bool {
    *value == 0.
}

#[derive(Clone, Deserialize, Serialize)]
#[serde(untagged)]
pub enum Scale {
    Uniform(f32),
    Axes([f32; 3]),
}

impl Default for Scale {
    fn default() -> Self {
        Self::Uniform(1.)
    }
}

impl Scale {
    pub fn axes(&self) -> [f32; 3] {
        match self {
            Self::Uniform(value) => [*value; 3],
            Self::Axes(values) => *values,
        }
    }
}

impl Transform {
    fn valid(&self) -> bool {
        bounded(self.position, -1000., 1000.)
            && bounded(self.scale.axes(), 0.01, 100.)
            && bounded(
                [self.pitch_degrees, self.yaw_degrees, self.roll_degrees],
                -360.,
                360.,
            )
    }

    pub fn model(&self, rotation: f32) -> Mat4 {
        Mat4::from_scale_rotation_translation(
            Vec3::from_array(self.scale.axes()),
            Quat::from_euler(
                EulerRot::YXZ,
                self.yaw_degrees.to_radians() + rotation,
                self.pitch_degrees.to_radians(),
                self.roll_degrees.to_radians(),
            ),
            Vec3::from_array(self.position),
        )
    }
}

#[derive(Clone, Deserialize, Serialize)]
#[serde(untagged)]
pub enum MaterialSource {
    Named(String),
    Inline(Material),
}

#[derive(Clone, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct RenderSource {
    pub mesh: String,
    pub material: MaterialSource,
}

pub struct Renderable {
    pub mesh: MeshKind,
    pub material: Material,
}

#[derive(Clone, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct Spin {
    pub degrees_per_second: f32,
}

/// An absent override inherits the template, null removes it, and a component
/// object replaces it for the individual entity.
#[derive(Clone, Default, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct Components {
    #[serde(
        default,
        skip_serializing_if = "Option::is_none",
        deserialize_with = "override_value"
    )]
    pub transform: Option<Option<Transform>>,
    #[serde(
        default,
        skip_serializing_if = "Option::is_none",
        deserialize_with = "override_value"
    )]
    pub render: Option<Option<RenderSource>>,
    #[serde(
        default,
        skip_serializing_if = "Option::is_none",
        deserialize_with = "override_value"
    )]
    pub spin: Option<Option<Spin>>,
}

fn override_value<'de, T: Deserialize<'de>, D: serde::Deserializer<'de>>(
    d: D,
) -> Result<Option<Option<T>>, D::Error> {
    Option::<T>::deserialize(d).map(Some)
}

#[derive(Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct EntityDocument {
    pub id: String,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub archetype: Option<String>,
    #[serde(default)]
    pub components: Components,
}

pub struct EntityId(pub String);

pub struct Entities {
    pub world: World,
    // Render order and snapshots remain stable across hecs archetype layouts.
    pub order: Vec<Entity>,
}

impl Entities {
    pub fn load(
        documents: Vec<(String, EntityDocument)>,
        archetypes: &BTreeMap<String, Components>,
        meshes: &BTreeMap<String, String>,
        materials: &Materials,
        texture_exists: &impl Fn(&str) -> bool,
    ) -> Result<Self, String> {
        let mut world = World::new();
        let mut order = Vec::with_capacity(documents.len());
        let empty = Components::default();
        for (path, document) in documents {
            let resolve = |error: &str| format!("{path}: {error}");
            let template = if let Some(name) = &document.archetype {
                archetypes
                    .get(name)
                    .ok_or_else(|| resolve(&format!("unknown archetype {name}")))?
            } else {
                &empty
            };
            let components = document.components;
            let transform = components
                .transform
                .or_else(|| template.transform.clone())
                .flatten();
            let render = components
                .render
                .or_else(|| template.render.clone())
                .flatten();
            let spin = components.spin.or_else(|| template.spin.clone()).flatten();
            if transform
                .as_ref()
                .is_some_and(|transform| !transform.valid())
            {
                return Err(resolve("invalid transform"));
            }
            if spin
                .as_ref()
                .is_some_and(|spin| !(-720.0..=720.).contains(&spin.degrees_per_second))
            {
                return Err(resolve("spin.degrees_per_second: expected -720..720"));
            }
            let renderable = render
                .map(|render| -> Result<Renderable, String> {
                    let mesh = match render.mesh.as_str() {
                        "floor" => MeshKind::Floor,
                        "sphere" => MeshKind::Sphere,
                        id if meshes.contains_key(id) => MeshKind::Imported(id.to_owned()),
                        id => return Err(resolve(&format!("render.mesh: unknown mesh {id}"))),
                    };
                    let material = match render.material {
                        MaterialSource::Named(id) => {
                            materials.get(&id).cloned().ok_or_else(|| {
                                resolve(&format!("render.material: unknown material {id}"))
                            })?
                        }
                        MaterialSource::Inline(material) => material,
                    };
                    material
                        .validate(texture_exists)
                        .map_err(|e| resolve(&format!("render.material.{e}")))?;
                    Ok(Renderable { mesh, material })
                })
                .transpose()?;
            let entity = world.spawn((EntityId(document.id),));
            if let Some(component) = transform {
                world.insert_one(entity, component).unwrap();
            }
            if let Some(component) = renderable {
                world.insert_one(entity, component).unwrap();
            }
            if let Some(component) = spin {
                world.insert_one(entity, component).unwrap();
            }
            order.push(entity);
        }
        Ok(Self { world, order })
    }

    pub fn ids(&self) -> impl Iterator<Item = String> + '_ {
        self.order
            .iter()
            .map(|&entity| self.world.get::<&EntityId>(entity).unwrap().0.clone())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::content;

    #[test]
    fn scalar_and_axis_scales_preserve_authored_transforms() {
        let original: Transform =
            serde_json::from_str(r#"{"position":[1,2,3],"scale":2,"yaw_degrees":90}"#).unwrap();
        assert!(original.valid());
        assert!(original
            .model(0.)
            .transform_point3(Vec3::X)
            .abs_diff_eq(Vec3::new(1., 2., 1.), 1e-5));
        let axes: Transform = crate::inline_fields::decode(
            br#"{"position":[0,0,0],"scale":{"type":"vec3","value":[2,3,4],"default":[1,1,1],"min":0.01,"max":100},"pitch_degrees":90,"roll_degrees":90}"#,
        )
        .unwrap();
        assert!(axes.valid());
        assert!(axes
            .model(0.)
            .transform_vector3(Vec3::X)
            .abs_diff_eq(Vec3::Z * 2., 1e-5));
        assert!(axes
            .model(0.)
            .transform_vector3(Vec3::Y)
            .abs_diff_eq(Vec3::NEG_X * 3., 1e-5));
    }

    #[test]
    fn archetype_overrides_round_trip_and_remove_only_one_entity_component() {
        let mut entity: EntityDocument = crate::inline_fields::decode(include_bytes!(
            "../../assets/scenes/entities/side-prop.json"
        ))
        .unwrap();
        entity.archetype = Some("spinning-prop".into());
        entity.components.spin = Some(None);
        let round_trip = serde_json::to_value(entity).unwrap();
        assert!(round_trip["components"]["spin"].is_null());
        assert_eq!(round_trip["archetype"], "spinning-prop");
        let mut manifest: serde_json::Value =
            serde_json::from_slice(include_bytes!("../../assets/scenes/entities.json")).unwrap();
        manifest["archetypes"] = serde_json::json!({
            "spinning-prop": {"spin": {"degrees_per_second": 41.7}}
        });
        let scene = content::load_scene("entities", |path, _| {
            match path {
                "scenes/entities.json" => return Ok(serde_json::to_vec(&manifest).unwrap()),
                "scenes/entities/side-prop.json" => return Ok(serde_json::to_vec(&round_trip).unwrap()),
                "scenes/entities/centerpiece.json" => return Ok(br#"{"id":"centerpiece","archetype":"spinning-prop","components":{"render":null}}"#.to_vec()),
                _ => {}
            }
            std::fs::read(std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("../assets").join(path))
                .map_err(|error| format!("{path}: {error}"))
        }).unwrap().scene;
        let centerpiece = scene.entities.order[1];
        let side = scene.entities.order[2];
        assert!(scene
            .entities
            .world
            .get::<&Renderable>(centerpiece)
            .is_err());
        assert!(scene.entities.world.get::<&Spin>(centerpiece).is_ok());
        assert!(scene.entities.world.get::<&Renderable>(side).is_ok());
        assert!(scene.entities.world.get::<&Spin>(side).is_err());
    }
}
