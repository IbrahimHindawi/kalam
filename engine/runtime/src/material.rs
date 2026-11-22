use serde::{Deserialize, Serialize};
use std::collections::HashMap;

#[derive(Deserialize, Serialize, Clone, Debug)]
#[serde(deny_unknown_fields)]
pub struct Material {
    pub version: u32,
    pub base_color: [f32; 4],
    pub metallic: f32,
    pub roughness: f32,
    #[serde(default)]
    pub base_texture: Option<String>,
    #[serde(default)]
    pub metallic_roughness_texture: Option<String>,
}

impl Material {
    pub fn validate(&self, exists: impl Fn(&str) -> bool) -> Result<(), String> {
        if self.version != 1 {
            return Err(format!("version: unsupported {}", self.version));
        }
        for (field, value) in [("metallic", self.metallic), ("roughness", self.roughness)] {
            if !(0.0..=1.0).contains(&value) {
                return Err(format!("{field}: expected 0..1"));
            }
        }
        if self.base_color.iter().any(|x| !(0.0..=1.0).contains(x)) {
            return Err("base_color: expected four channels in 0..1".into());
        }
        for (field, path) in [
            ("base_texture", &self.base_texture),
            (
                "metallic_roughness_texture",
                &self.metallic_roughness_texture,
            ),
        ] {
            if let Some(path) = path {
                if !exists(path) {
                    return Err(format!("{field}: unknown asset {path}"));
                }
            }
        }
        Ok(())
    }
}

#[derive(Default)]
pub struct Materials(HashMap<String, Material>);

impl Materials {
    pub fn replace(
        &mut self,
        id: &str,
        json: &[u8],
        exists: impl Fn(&str) -> bool,
    ) -> Result<(), String> {
        let next: Material = crate::inline_fields::decode(json)?;
        next.validate(exists)?;
        self.0.insert(id.to_owned(), next);
        Ok(())
    }
    pub fn get(&self, id: &str) -> Option<&Material> {
        self.0.get(id)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn failed_replacements_keep_live_revision() {
        let original = include_str!("../../assets/materials/reference.json");
        let mut store = Materials::default();
        store
            .replace("wall", original.as_bytes(), |_| true)
            .unwrap();
        for (replacement, field) in [
            (original.replace("0.42", "2.0"), "roughness"),
            (
                original.replace("fixture/neutral", "bad"),
                "metallic_roughness_texture",
            ),
        ] {
            assert!(store
                .replace("wall", replacement.as_bytes(), |p| p
                    .starts_with("fixture/"))
                .unwrap_err()
                .contains(field));
            assert_eq!(store.get("wall").unwrap().roughness, 0.42);
        }
        store
            .replace("wall", original.replace("0.42", "0.75").as_bytes(), |_| {
                true
            })
            .unwrap();
        assert_eq!(store.get("wall").unwrap().roughness, 0.75);
    }
}
