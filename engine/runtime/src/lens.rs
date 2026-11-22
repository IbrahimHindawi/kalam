use crate::scene::Scene;
use serde::Deserialize;
use std::collections::HashSet;

const MAX_CONTROLS: usize = 6;

#[derive(Clone, Copy, Debug, Deserialize, Eq, Hash, PartialEq)]
pub enum Binding {
    #[serde(rename = "walk.speed")]
    WalkSpeed,
    #[serde(rename = "exposure")]
    Exposure,
}

impl Binding {
    pub fn range(self, scene: &Scene) -> (f32, f32) {
        match self {
            Self::WalkSpeed => scene.walk_speed_range,
            Self::Exposure => scene.exposure_range,
        }
    }

    pub fn read(self, scene: &Scene) -> Option<f32> {
        match self {
            Self::WalkSpeed => scene.walk.as_ref().map(|config| config.speed),
            Self::Exposure => Some(scene.exposure),
        }
    }

    pub fn write(self, scene: &mut Scene, value: f32) -> Result<(), &'static str> {
        let (lo, hi) = self.range(scene);
        if !(lo..=hi).contains(&value) {
            return Err("value is outside the scene's allowed range");
        }
        match self {
            Self::WalkSpeed => scene
                .walk
                .as_mut()
                .ok_or("scene has no walk configuration")?
                .set_speed(value),
            Self::Exposure => scene.set_exposure(value),
        }
    }
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Slider {
    pub label: String,
    pub bind: Binding,
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Descriptor {
    version: u32,
    pub title: String,
    pub sliders: Vec<Slider>,
}

impl Descriptor {
    pub fn parse(bytes: &[u8]) -> Result<Self, String> {
        let descriptor: Self = crate::inline_fields::decode(bytes)?;
        if descriptor.version != 1 {
            return Err("unsupported lens version".into());
        }
        if descriptor.sliders.is_empty() || descriptor.sliders.len() > MAX_CONTROLS {
            return Err(format!("expected 1..{MAX_CONTROLS} sliders"));
        }
        if !label_valid(&descriptor.title, 28) {
            return Err("title must be 1..28 printable ASCII characters".into());
        }
        let mut bindings = HashSet::new();
        for (index, control) in descriptor.sliders.iter().enumerate() {
            if !label_valid(&control.label, 24) || !bindings.insert(control.bind) {
                return Err(format!(
                    "sliders[{index}]: invalid label or duplicate binding"
                ));
            }
        }
        Ok(descriptor)
    }
}

fn label_valid(label: &str, max: usize) -> bool {
    !label.trim().is_empty()
        && label.len() <= max
        && label.bytes().all(|byte| (32..=126).contains(&byte))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn rejects_unknown_or_duplicate_bindings() {
        let data = include_str!("../../assets/lenses/inspect.json");
        assert_eq!(Descriptor::parse(data.as_bytes()).unwrap().sliders.len(), 2);
        assert!(Descriptor::parse(data.replace("exposure", "unknown").as_bytes()).is_err());
        assert!(Descriptor::parse(data.replace("exposure", "walk.speed").as_bytes()).is_err());
    }
}
