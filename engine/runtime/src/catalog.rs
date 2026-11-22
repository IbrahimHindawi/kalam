//! Project asset IDs resolve authored resources once at load time.
use serde::Deserialize;
use std::collections::BTreeMap;

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Catalog {
    version: u32,
    assets: BTreeMap<String, SkinnedMesh>,
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
pub struct SkinnedMesh {
    pub path: String,
    pub height: f32,
    pub forward: Forward,
    pub clips: BTreeMap<String, ClipSource>,
}

#[derive(Deserialize)]
#[serde(untagged)]
pub enum ClipSource {
    Embedded(String),
    Companion(CompanionClip),
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
pub struct CompanionClip {
    pub path: String,
    pub animation: Option<String>,
}

impl ClipSource {
    fn valid(&self) -> bool {
        match self {
            Self::Embedded(name) => !name.trim().is_empty(),
            Self::Companion(clip) => {
                valid_model_path(&clip.path)
                    && clip
                        .animation
                        .as_ref()
                        .is_none_or(|name| !name.trim().is_empty())
            }
        }
    }
}

#[derive(Clone, Copy, Deserialize)]
pub enum Forward {
    #[serde(rename = "+x")]
    PositiveX,
    #[serde(rename = "-x")]
    NegativeX,
    #[serde(rename = "+z")]
    PositiveZ,
    #[serde(rename = "-z")]
    NegativeZ,
}

pub(crate) fn valid_relative_path(path: &str) -> bool {
    path.split('/').all(|component| {
        !component.is_empty()
            && component != "."
            && component != ".."
            && component
                .bytes()
                .all(|b| b.is_ascii_alphanumeric() || matches!(b, b'_' | b'-' | b'.'))
    })
}

fn valid_model_path(path: &str) -> bool {
    (path.ends_with(".glb") || path.ends_with(".gltf")) && valid_relative_path(path)
}

impl Forward {
    pub fn yaw_offset(self) -> f32 {
        match self {
            Self::PositiveX => -std::f32::consts::FRAC_PI_2,
            Self::NegativeX => std::f32::consts::FRAC_PI_2,
            Self::PositiveZ => 0.,
            Self::NegativeZ => std::f32::consts::PI,
        }
    }
}

impl Catalog {
    pub fn parse(bytes: &[u8]) -> Result<Self, String> {
        let text = std::str::from_utf8(bytes).map_err(|error| error.to_string())?;
        let catalog: Self = toml::from_str(text).map_err(|error| error.to_string())?;
        if catalog.version != 1 {
            return Err(format!(
                "unsupported asset catalog version {}",
                catalog.version
            ));
        }
        for (id, asset) in &catalog.assets {
            if !crate::scene::valid_name(id)
                || !valid_model_path(&asset.path)
                || !asset.height.is_finite()
                || asset.height <= 0.
                || asset.clips.is_empty()
                || asset
                    .clips
                    .iter()
                    .any(|(slot, source)| !crate::scene::valid_name(slot) || !source.valid())
            {
                return Err(format!(
                    "asset {id:?}: invalid path, height, or clip aliases"
                ));
            }
        }
        Ok(catalog)
    }

    pub fn skin(&self, id: &str) -> Result<&SkinnedMesh, String> {
        self.assets
            .get(id)
            .ok_or_else(|| format!("unknown skinned asset {id:?}"))
    }
}
