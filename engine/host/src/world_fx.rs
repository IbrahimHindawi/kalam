//! Transparent unlit geometry rendered into the scene's HDR target.
use super::{Assets, Handle, Mesh, Texture};
use glam::{Mat4, Vec3};
use kalam_runtime::fx::{Blend, FxDraw, FxWorld, Kind, Layer};

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum FxBlend {
    #[default]
    Alpha,
    Additive,
}

fn flags(blend: FxBlend, on_top: bool) -> u32 {
    u32::from(blend == FxBlend::Additive) | (u32::from(on_top) << 1)
}

#[derive(Clone, Copy)]
pub struct FxQuad {
    /// World-space corners in top-left, top-right, bottom-right, bottom-left order.
    pub corners: [Vec3; 4],
    /// Normalized [left, top, right, bottom]. Invert either pair to flip a sprite.
    pub uv: [f32; 4],
    /// Straight linear RGB (including HDR emission) and opacity.
    pub color: [f32; 4],
    pub texture: Handle<Texture>,
    pub blend: FxBlend,
    pub render_on_top: bool,
    pub surface: FxQuadSurface,
}

#[derive(Clone, Copy)]
pub struct FxQuadSurface {
    pub ramp: Option<Handle<Texture>>,
    pub dissolve: Option<Handle<Texture>>,
    pub mask_uv: [f32; 4],
    pub ramp_strength: f32,
    pub dissolve_amount: f32,
    pub alpha_strength: f32,
    pub opacity_source: FxOpacity,
    pub cutout_threshold: f32,
    pub cutout_softness: f32,
}

impl Default for FxQuadSurface {
    fn default() -> Self {
        Self {
            ramp: None,
            dissolve: None,
            mask_uv: [0., 0., 1., 1.],
            ramp_strength: 0.,
            dissolve_amount: 0.,
            alpha_strength: 1.,
            opacity_source: FxOpacity::Texture,
            cutout_threshold: 0.,
            cutout_softness: 0.,
        }
    }
}

#[derive(Clone, Copy, Default)]
pub enum FxOpacity {
    #[default]
    Texture,
    BlackCutout,
    TextureAndBlackCutout,
}

impl FxOpacity {
    fn from_layer(layer: &Layer) -> Result<Self, String> {
        match layer.mesh_opacity_source.as_str() {
            "texture_alpha" => Ok(Self::Texture),
            "black_cutout" => Ok(Self::BlackCutout),
            "alpha_black_cutout" => Ok(Self::TextureAndBlackCutout),
            other => Err(format!("unsupported FX opacity source '{other}'")),
        }
    }
}

#[derive(Clone, Copy)]
pub struct FxMeshSurface {
    pub opacity: Option<Handle<Texture>>,
    pub ramp: Option<Handle<Texture>>,
    pub dissolve: Option<Handle<Texture>>,
    pub alpha_strength: f32,
    pub opacity_source: FxOpacity,
    pub cutout_threshold: f32,
    pub cutout_softness: f32,
    pub uv_offset: [f32; 2],
    pub flip_mask_uv: [bool; 2],
    pub ramp_strength: f32,
    pub phase: f32,
    pub hold_dissolve: bool,
    pub dissolve_start: f32,
    pub dissolve_softness: f32,
    pub wave_wipe: bool,
    pub wave_frequency: f32,
    pub wave_speed: f32,
    pub wave_along_v: bool,
    pub wave_width: f32,
    pub wave_softness: f32,
    pub wave_reverse: bool,
}

impl Default for FxMeshSurface {
    fn default() -> Self {
        Self {
            opacity: None,
            ramp: None,
            dissolve: None,
            alpha_strength: 1.,
            opacity_source: FxOpacity::Texture,
            cutout_threshold: 0.,
            cutout_softness: 0.,
            uv_offset: [0.; 2],
            flip_mask_uv: [false; 2],
            ramp_strength: 0.,
            phase: 0.,
            hold_dissolve: false,
            dissolve_start: 0.,
            dissolve_softness: 0.05,
            wave_wipe: false,
            wave_frequency: 1.,
            wave_speed: 1.,
            wave_along_v: false,
            wave_width: 0.1,
            wave_softness: 0.05,
            wave_reverse: false,
        }
    }
}

/// Rigid, unlit mesh effect; shared mesh and texture handles are owned by Assets.
#[derive(Clone, Copy)]
pub struct FxMesh {
    pub mesh: Handle<Mesh>,
    pub model: Mat4,
    pub uv: [f32; 4],
    pub texture: Handle<Texture>,
    pub color: [f32; 4],
    pub blend: FxBlend,
    pub render_on_top: bool,
    pub surface: FxMeshSurface,
}

#[derive(Clone, Copy)]
enum Item {
    Quad(usize),
    Mesh(usize),
}

struct Submission {
    item: Item,
    depth: f32,
    texture: u32,
    flags: u32,
    quad_maps: [u32; 2],
    quad_opacity: [f32; 4],
}

/// Reuse across frames. Quads and meshes share back-to-front ordering. An
/// authored "on top" layer draws after depth-tested layers in the same HDR pass.
#[derive(Default)]
pub struct WorldFx {
    pub quads: Vec<FxQuad>,
    pub meshes: Vec<FxMesh>,
    sorted: Vec<Submission>,
    vertices: Vec<FxVertex>,
    /// Quads shed during the last render when bgfx's transient pool filled.
    pub dropped_quads: usize,
}

#[repr(C)]
struct FxVertex {
    position: [f32; 3],
    color: [f32; 4],
    uv: [f32; 2],
    mask: [f32; 4],
}

extern "C" {
    fn kh_begin_fx(order: u16, view: *const f32, proj: *const f32);
    fn kh_fx_quads(
        order: u16,
        maps: *const u32,
        flags: u32,
        opacity: *const f32,
        data: *const FxVertex,
        count: u32,
    ) -> i32;
    fn kh_fx_mesh(
        order: u16,
        mesh: u32,
        flags: u32,
        model: *const f32,
        maps: *const u32,
        params: *const [f32; 4],
    ) -> i32;
}

impl WorldFx {
    pub fn clear(&mut self) {
        self.quads.clear();
        self.meshes.clear();
    }

    /// Append runtime instances using the project's asset catalog. Two passes through
    /// this API (game and editor preview) can share one sorted transparent submission.
    pub fn extend_world(
        &mut self,
        world: &FxWorld,
        view: Mat4,
        texture: impl Fn(&str) -> Option<Handle<Texture>>,
        mesh: impl Fn(&str) -> Option<Handle<Mesh>>,
    ) -> Result<(), String> {
        let right = view.row(0).truncate();
        let up = view.row(1).truncate();
        let start = (self.quads.len(), self.meshes.len());
        let tex = |name: &str| texture(name).ok_or_else(|| format!("unknown FX texture '{name}'"));
        let blend = |layer: &Layer| match layer.blend {
            Blend::Alpha => FxBlend::Alpha,
            Blend::Additive => FxBlend::Additive,
        };
        let mut result = Ok(());
        world.visit(right, up, |draw| {
            if result.is_err() {
                return;
            }
            result = (|| {
                match draw {
                    FxDraw::Quad {
                        layer,
                        corners,
                        uv,
                        color,
                        dissolve,
                    } => {
                        let mut mask_uv = uv;
                        if layer.texture_flip_u {
                            if layer.kind == Kind::Ribbon {
                                mask_uv[0] = 1. - mask_uv[0];
                                mask_uv[2] = 1. - mask_uv[2];
                            } else {
                                mask_uv.swap(0, 2);
                            }
                        }
                        if layer.texture_flip_v {
                            mask_uv.swap(1, 3);
                        }
                        self.quads.push(FxQuad {
                            corners,
                            uv,
                            color,
                            texture: tex(&layer.texture)?,
                            blend: blend(layer),
                            render_on_top: layer.render_on_top,
                            surface: FxQuadSurface {
                                ramp: (layer.ramp_strength > 0.)
                                    .then(|| tex(&layer.ramp_texture))
                                    .transpose()?,
                                dissolve: (layer.kind == Kind::Ribbon
                                    && !layer.dissolve_texture.is_empty())
                                .then(|| tex(&layer.dissolve_texture))
                                .transpose()?,
                                mask_uv,
                                ramp_strength: layer.ramp_strength,
                                dissolve_amount: dissolve,
                                alpha_strength: layer.mesh_texture_alpha_strength,
                                opacity_source: FxOpacity::from_layer(layer)?,
                                cutout_threshold: layer.mesh_cutout_threshold,
                                cutout_softness: layer.mesh_cutout_softness,
                            },
                        });
                    }
                    FxDraw::Mesh {
                        layer,
                        model,
                        color,
                        progress,
                        seconds,
                    } => {
                        let mut uv = [0., 0., 1., 1.];
                        if layer.texture_flip_u {
                            uv.swap(0, 2);
                        }
                        if layer.texture_flip_v {
                            uv.swap(1, 3);
                        }
                        self.meshes.push(FxMesh {
                            mesh: mesh(&layer.mesh)
                                .ok_or_else(|| format!("unknown FX mesh '{}'", layer.mesh))?,
                            model,
                            uv,
                            color,
                            texture: tex(&layer.texture)?,
                            blend: blend(layer),
                            render_on_top: layer.render_on_top,
                            surface: FxMeshSurface {
                                opacity: (!layer.opacity_texture.is_empty())
                                    .then(|| tex(&layer.opacity_texture))
                                    .transpose()?,
                                ramp: (layer.ramp_strength > 0.)
                                    .then(|| tex(&layer.ramp_texture))
                                    .transpose()?,
                                dissolve: layer
                                    .mesh_dissolve_active
                                    .then(|| tex(&layer.dissolve_texture))
                                    .transpose()?,
                                alpha_strength: layer.mesh_texture_alpha_strength,
                                opacity_source: FxOpacity::from_layer(layer)?,
                                cutout_threshold: layer.mesh_cutout_threshold,
                                cutout_softness: layer.mesh_cutout_softness,
                                uv_offset: [0., 2. * seconds * layer.mesh_scroll_speed],
                                flip_mask_uv: [
                                    layer.opacity_texture_flip_u,
                                    layer.opacity_texture_flip_v,
                                ],
                                ramp_strength: layer.ramp_strength,
                                phase: progress,
                                hold_dissolve: layer.mesh_dissolve_active,
                                dissolve_start: layer.mesh_dissolve_start,
                                dissolve_softness: layer.mesh_dissolve_softness,
                                wave_wipe: layer.wave_wipe_active,
                                wave_frequency: layer.wave_wipe_frequency,
                                wave_speed: layer.wave_wipe_speed,
                                wave_along_v: layer.wave_wipe_direction == "v_band_from_u",
                                wave_width: layer.wave_wipe_width,
                                wave_softness: layer.wave_wipe_softness,
                                wave_reverse: layer.wave_wipe_reverse,
                            },
                        });
                    }
                }
                Ok(())
            })();
        });
        if result.is_err() {
            self.quads.truncate(start.0);
            self.meshes.truncate(start.1);
        }
        result
    }

    pub(super) fn render(
        &mut self,
        order: u16,
        assets: &Assets,
        view: Mat4,
        proj: Mat4,
    ) -> Result<(), String> {
        self.dropped_quads = 0;
        self.sorted.clear();
        if self.quads.is_empty() && self.meshes.is_empty() {
            return Ok(());
        }
        let depth = |p: Vec3| -view.transform_point3(p).z;
        let texture = |h| assets.textures.get(h).ok_or("stale world FX texture");
        for (i, quad) in self.quads.iter().enumerate() {
            let id = texture(quad.texture)?;
            let material = &quad.surface;
            self.sorted.push(Submission {
                quad_maps: [
                    material.ramp.map_or(Ok(id), texture)?,
                    material.dissolve.map_or(Ok(id), texture)?,
                ],
                quad_opacity: [
                    material.alpha_strength,
                    material.opacity_source as u8 as f32,
                    material.cutout_threshold,
                    material.cutout_softness,
                ],
                item: Item::Quad(i),
                depth: depth(quad.corners.iter().sum::<Vec3>() * 0.25),
                texture: id,
                flags: flags(quad.blend, quad.render_on_top),
            });
        }
        for (i, mesh) in self.meshes.iter().enumerate() {
            self.sorted.push(Submission {
                item: Item::Mesh(i),
                quad_maps: [0; 2],
                quad_opacity: [0.; 4],
                depth: depth(mesh.model.w_axis.truncate()),
                texture: texture(mesh.texture)?,
                flags: flags(mesh.blend, mesh.render_on_top),
            });
        }
        // Use the draw index as a deterministic tie-break at equal depths, including
        // coplanar stamps. Unstable sort then needs no temporary allocation.
        let key = |s: &Submission| match s.item {
            Item::Quad(i) => (0, i),
            Item::Mesh(i) => (1, i),
        };
        self.sorted.sort_unstable_by(|a, b| {
            (a.flags >> 1)
                .cmp(&(b.flags >> 1))
                .then_with(|| b.depth.total_cmp(&a.depth))
                .then_with(|| key(a).cmp(&key(b)))
        });
        unsafe { kh_begin_fx(order, view.as_ref().as_ptr(), proj.as_ref().as_ptr()) };
        let mut i = 0;
        while i < self.sorted.len() {
            let entry = &self.sorted[i];
            if let Item::Mesh(idx) = entry.item {
                let fx = &self.meshes[idx];
                let s = &fx.surface;
                let mesh = assets.meshes.get(fx.mesh).ok_or("stale world FX mesh")?;
                let map = |h: Option<Handle<Texture>>| h.map_or(Ok(entry.texture), texture);
                let maps = [
                    entry.texture,
                    map(s.opacity)?,
                    map(s.ramp)?,
                    map(s.dissolve)?,
                ];
                let params = [
                    fx.color,
                    fx.uv,
                    [
                        s.uv_offset[0],
                        s.uv_offset[1],
                        f32::from(s.flip_mask_uv[0]),
                        f32::from(s.flip_mask_uv[1]),
                    ],
                    [
                        s.alpha_strength,
                        s.opacity_source as u8 as f32,
                        s.cutout_threshold,
                        s.cutout_softness,
                    ],
                    [
                        f32::from(s.opacity.is_some()),
                        if s.ramp.is_some() {
                            s.ramp_strength
                        } else {
                            0.
                        },
                        f32::from(s.dissolve.is_some()),
                        s.phase,
                    ],
                    [
                        f32::from(s.hold_dissolve),
                        s.dissolve_start,
                        s.dissolve_softness,
                        0.,
                    ],
                    [
                        f32::from(s.wave_wipe),
                        s.wave_frequency,
                        s.wave_speed,
                        f32::from(s.wave_along_v),
                    ],
                    [s.wave_width, s.wave_softness, f32::from(s.wave_reverse), 0.],
                ];
                let ok = unsafe {
                    kh_fx_mesh(
                        order,
                        mesh,
                        entry.flags,
                        fx.model.as_ref().as_ptr(),
                        maps.as_ptr(),
                        params.as_ptr(),
                    )
                };
                if ok == 0 {
                    return Err("world FX requires a valid rigid mesh".into());
                }
                i += 1;
                continue;
            }

            // Only merge consecutive compatible submissions. Regrouping by texture
            // would allow distant smoke to blend over closer flashes.
            let start = i;
            self.vertices.clear();
            while i < self.sorted.len() && i - start < 4096 {
                let next = &self.sorted[i];
                let Item::Quad(idx) = next.item else { break };
                if i != start
                    && (next.texture != entry.texture
                        || next.flags != entry.flags
                        || next.quad_maps != entry.quad_maps
                        || next.quad_opacity != entry.quad_opacity)
                {
                    break;
                }
                let quad = &self.quads[idx];
                let [u0, v0, u1, v1] = quad.uv;
                let surface = &quad.surface;
                let [a, b, c, d] = surface.mask_uv;
                let ramp = if surface.ramp.is_some() {
                    surface.ramp_strength
                } else {
                    0.
                };
                let dissolve = if surface.dissolve.is_some() {
                    surface.dissolve_amount
                } else {
                    0.
                };
                for ((position, uv), mask_uv) in quad
                    .corners
                    .into_iter()
                    .zip([[u0, v0], [u1, v0], [u1, v1], [u0, v1]])
                    .zip([[a, b], [c, b], [c, d], [a, d]])
                {
                    self.vertices.push(FxVertex {
                        position: position.to_array(),
                        color: quad.color,
                        uv,
                        mask: [mask_uv[0], mask_uv[1], ramp, dissolve],
                    });
                }
                i += 1;
            }
            let count = i - start;
            let accepted = unsafe {
                kh_fx_quads(
                    order,
                    [entry.texture, entry.quad_maps[0], entry.quad_maps[1]].as_ptr(),
                    entry.flags,
                    entry.quad_opacity.as_ptr(),
                    self.vertices.as_ptr(),
                    count as u32,
                )
            };
            if accepted < 0 {
                return Err("world FX quad submission has an invalid texture".into());
            }
            self.dropped_quads += count - accepted as usize;
        }
        Ok(())
    }
}
