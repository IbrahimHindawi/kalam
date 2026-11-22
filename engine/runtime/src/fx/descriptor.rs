use serde::{Deserialize, Deserializer, Serialize};

pub const MAX_LAYERS: usize = 16;

#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum Kind {
    Particle,
    Ribbon,
    Mesh,
    Beam,
    Decal,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum Blend {
    Alpha,
    Additive,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum Orientation {
    Billboard,
    Ground,
}

fn flag<'de, D: Deserializer<'de>>(d: D) -> Result<bool, D::Error> {
    #[derive(Deserialize)]
    #[serde(untagged)]
    enum Flag {
        Bool(bool),
        Number(u8),
    }
    match Flag::deserialize(d)? {
        Flag::Bool(v) => Ok(v),
        Flag::Number(0) => Ok(false),
        Flag::Number(1) => Ok(true),
        Flag::Number(_) => Err(serde::de::Error::custom("expected boolean or 0/1")),
    }
}

#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(default)]
pub struct Layer {
    pub version: u32,
    pub name: String,
    #[serde(rename = "type")]
    pub kind: Kind,
    pub blend: Blend,
    pub orientation: Orientation,
    pub timing_source: String,
    pub attach_kind: String,
    pub attach_a: String,
    pub attach_b: String,
    pub attach_b_offset: [f32; 3],
    pub texture: String,
    pub opacity_texture: String,
    pub ramp_texture: String,
    pub ramp_strength: f32,
    #[serde(deserialize_with = "flag")]
    pub texture_flip_u: bool,
    #[serde(deserialize_with = "flag")]
    pub texture_flip_v: bool,
    #[serde(deserialize_with = "flag")]
    pub opacity_texture_flip_u: bool,
    #[serde(deserialize_with = "flag")]
    pub opacity_texture_flip_v: bool,
    #[serde(deserialize_with = "flag")]
    pub render_on_top: bool,
    pub mesh: String,
    pub mesh_material: String,
    pub mesh_translate: [f32; 3],
    pub mesh_rotate_deg: [f32; 3],
    pub mesh_scale: [f32; 3],
    pub mesh_scroll_speed: f32,
    pub mesh_opacity_source: String,
    pub mesh_cutout_threshold: f32,
    pub mesh_cutout_softness: f32,
    pub mesh_texture_alpha_strength: f32,
    pub dissolve_texture: String,
    #[serde(deserialize_with = "flag")]
    pub mesh_dissolve_active: bool,
    pub mesh_dissolve_start: f32,
    pub mesh_dissolve_softness: f32,
    #[serde(deserialize_with = "flag")]
    pub wave_wipe_active: bool,
    pub wave_wipe_direction: String,
    #[serde(deserialize_with = "flag")]
    pub wave_wipe_reverse: bool,
    pub wave_wipe_frequency: f32,
    pub wave_wipe_speed: f32,
    pub wave_wipe_width: f32,
    pub wave_wipe_softness: f32,
    pub emission_window_start_normalized: f32,
    pub emission_window_end_normalized: f32,
    pub sample_life: f32,
    pub emit_interval: f32,
    pub max_sample_gap: f32,
    pub particle_emit_count: u32,
    pub particle_burst_count: u32,
    pub particle_sheet_columns: u32,
    pub particle_sheet_rows: u32,
    pub particle_sheet_frame: i32,
    pub particle_sheet_frame_count: u32,
    pub particle_life_min: f32,
    pub particle_life_max: f32,
    pub particle_size_start: [f32; 2],
    pub particle_size_end: [f32; 2],
    pub particle_size_random: f32,
    pub particle_spawn_radius: f32,
    pub particle_velocity_min: [f32; 3],
    pub particle_velocity_max: [f32; 3],
    pub particle_acceleration: [f32; 3],
    pub particle_rotation_min_deg: f32,
    pub particle_rotation_max_deg: f32,
    pub particle_spin_min_deg: f32,
    pub particle_spin_max_deg: f32,
    pub color: [f32; 4],
    pub particle_color_end: [f32; 4],
}

impl Default for Layer {
    fn default() -> Self {
        Self {
            version: 1,
            name: String::new(),
            kind: Kind::Particle,
            blend: Blend::Alpha,
            orientation: Orientation::Billboard,
            timing_source: "instance".into(),
            attach_kind: "none".into(),
            attach_a: String::new(),
            attach_b: String::new(),
            attach_b_offset: [-1.0, 0.0, 0.0],
            texture: "res_tex_white".into(),
            opacity_texture: "res_tex_white".into(),
            ramp_texture: "res_tex_white".into(),
            ramp_strength: 0.0,
            texture_flip_u: false,
            texture_flip_v: false,
            opacity_texture_flip_u: false,
            opacity_texture_flip_v: false,
            render_on_top: false,
            mesh: "res_mesh_sprite".into(),
            mesh_material: "fx_mesh".into(),
            mesh_translate: [0.0; 3],
            mesh_rotate_deg: [0.0; 3],
            mesh_scale: [1.0; 3],
            mesh_scroll_speed: 1.0,
            mesh_opacity_source: "texture_alpha".into(),
            mesh_cutout_threshold: 0.02,
            mesh_cutout_softness: 0.05,
            mesh_texture_alpha_strength: 1.0,
            dissolve_texture: "res_tex_white".into(),
            mesh_dissolve_active: false,
            mesh_dissolve_start: 0.4,
            mesh_dissolve_softness: 0.05,
            wave_wipe_active: false,
            wave_wipe_direction: "u_band_from_v".into(),
            wave_wipe_reverse: false,
            wave_wipe_frequency: 6.0,
            wave_wipe_speed: 2.0,
            wave_wipe_width: 0.15,
            wave_wipe_softness: 0.05,
            emission_window_start_normalized: 0.0,
            emission_window_end_normalized: 1.0,
            sample_life: 0.1,
            emit_interval: 0.015,
            max_sample_gap: 0.1,
            particle_emit_count: 1,
            particle_burst_count: 0,
            particle_sheet_columns: 1,
            particle_sheet_rows: 1,
            particle_sheet_frame: -1,
            particle_sheet_frame_count: 0,
            particle_life_min: 0.35,
            particle_life_max: 0.55,
            particle_size_start: [0.2; 2],
            particle_size_end: [0.7; 2],
            particle_size_random: 0.2,
            particle_spawn_radius: 0.05,
            particle_velocity_min: [-0.1, 0.2, -0.1],
            particle_velocity_max: [0.1, 0.6, 0.1],
            particle_acceleration: [0.0; 3],
            particle_rotation_min_deg: 0.0,
            particle_rotation_max_deg: 360.0,
            particle_spin_min_deg: -90.0,
            particle_spin_max_deg: 90.0,
            color: [1.0; 4],
            particle_color_end: [1.0, 1.0, 1.0, 0.0],
        }
    }
}

fn frame_one() -> u32 {
    1
}
fn frame_last() -> u32 {
    21
}

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct FxAsset {
    pub version: u32,
    pub name: String,
    #[serde(default, deserialize_with = "flag")]
    pub standalone: bool,
    #[serde(default = "frame_one")]
    pub start_frame: u32,
    #[serde(default = "frame_last")]
    pub end_frame: u32,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub duration: Option<f32>,
    pub effects: Vec<Layer>,
}

impl FxAsset {
    pub fn parse(bytes: &[u8]) -> Result<Self, String> {
        if bytes.len() > 1024 * 1024 {
            return Err("FX descriptor exceeds 1 MiB".into());
        }
        let asset: Self = serde_json::from_slice(bytes).map_err(|e| e.to_string())?;
        asset.validate()?;
        Ok(asset)
    }

    pub fn seconds(&self) -> f32 {
        self.duration.unwrap_or_else(|| {
            (self.end_frame.saturating_sub(self.start_frame) as f32 + 1.0) / 60.0
        })
    }

    pub fn validate(&self) -> Result<(), String> {
        if self.version != 1 {
            return Err(format!("unsupported FX version {}", self.version));
        }
        if self.name.trim().is_empty() {
            return Err("missing FX name".into());
        }
        if self.end_frame < self.start_frame || !(0.001..=3600.0).contains(&self.seconds()) {
            return Err(format!("{}: invalid FX duration or frame range", self.name));
        }
        if self.effects.len() > MAX_LAYERS {
            return Err(format!(
                "{}: at most {MAX_LAYERS} effects are supported",
                self.name
            ));
        }
        for (i, layer) in self.effects.iter().enumerate() {
            layer
                .validate()
                .map_err(|msg| format!("{}.effects[{i}] ({}): {msg}", self.name, layer.name))?;
        }
        Ok(())
    }
}

impl Layer {
    fn validate(&self) -> Result<(), &'static str> {
        if self.version != 1 {
            return Err("unsupported layer version");
        }
        // JSON numbers are parsed at double precision; a large valid number
        // can still overflow while being converted to the game's GPU floats.
        let scalars = [
            self.max_sample_gap,
            self.particle_spawn_radius,
            self.particle_size_random,
            self.particle_rotation_min_deg,
            self.particle_rotation_max_deg,
            self.particle_spin_min_deg,
            self.particle_spin_max_deg,
            self.ramp_strength,
            self.mesh_scroll_speed,
            self.mesh_cutout_threshold,
            self.mesh_cutout_softness,
            self.mesh_texture_alpha_strength,
            self.mesh_dissolve_start,
            self.mesh_dissolve_softness,
            self.wave_wipe_frequency,
            self.wave_wipe_speed,
            self.wave_wipe_width,
            self.wave_wipe_softness,
        ];
        let vectors: &[&[f32]] = &[
            &self.attach_b_offset,
            &self.mesh_translate,
            &self.mesh_rotate_deg,
            &self.mesh_scale,
            &self.particle_size_start,
            &self.particle_size_end,
            &self.particle_velocity_min,
            &self.particle_velocity_max,
            &self.particle_acceleration,
            &self.color,
            &self.particle_color_end,
        ];
        if !scalars.into_iter().all(f32::is_finite)
            || vectors.iter().any(|v| !v.iter().all(|p| p.is_finite()))
        {
            return Err("material, transform and particle parameters must be finite");
        }
        if !(0.0..=1.0).contains(&self.emission_window_start_normalized)
            || !(self.emission_window_start_normalized..=1.0)
                .contains(&self.emission_window_end_normalized)
        {
            return Err("invalid normalized emission window");
        }
        if !(0.001..=3600.0).contains(&self.emit_interval)
            || !(0.001..=60.0).contains(&self.sample_life)
            || self.max_sample_gap < 0.0
        {
            return Err("invalid emission interval, ribbon lifetime or sample gap");
        }
        if self.particle_emit_count > 128 || self.particle_burst_count > 128 {
            return Err("particle count exceeds 128 per emission");
        }
        if !(1..=16).contains(&self.particle_sheet_columns)
            || !(1..=16).contains(&self.particle_sheet_rows)
        {
            return Err("spritesheet rows and columns must be in 1..=16");
        }
        let cells = self.particle_sheet_rows * self.particle_sheet_columns;
        if !(-1..cells as i32).contains(&self.particle_sheet_frame)
            || self.particle_sheet_frame_count > cells
        {
            return Err("spritesheet frame outside atlas");
        }
        if !(0.001..=60.0).contains(&self.particle_life_min)
            || !(self.particle_life_min..=60.0).contains(&self.particle_life_max)
        {
            return Err("invalid particle lifetime range");
        }
        if self
            .particle_size_start
            .into_iter()
            .chain(self.particle_size_end)
            .any(|v| v < 0.0)
            || self.particle_spawn_radius < 0.0
            || self.particle_size_random < 0.0
        {
            return Err("particle sizes, randomness and radius must be nonnegative");
        }
        Ok(())
    }
}
