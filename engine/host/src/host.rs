use crate::panel::{Panel, WIDTH as PANEL_WIDTH};
use crate::resources::{Handle, Mesh, SkinPalette, Table, Texture};
use crate::viewport::{
    camera_offset, fly_editor_camera, frame_camera, viewport_projection, viewport_ray,
    viewport_size,
};
use bumpalo::{collections::Vec as FrameVec, Bump};
use glam::{Mat4, Vec2, Vec3};
use kalam_runtime::content::CpuScene;
use kalam_runtime::entities::{EntityId, Renderable, Spin, Transform};
use kalam_runtime::lens::Descriptor;
use kalam_runtime::mesh::Geometry;
use kalam_runtime::scene::{Camera, MeshKind, Scene};
use kalam_runtime::skin::Skin;
use kalam_runtime::walk::Walker;
use std::collections::{BTreeMap, HashMap};
use std::ffi::{CStr, CString};
use std::sync::Mutex;

use crate::game;
#[path = "world_fx.rs"]
mod world_fx;
pub use world_fx::{FxBlend, FxMesh, FxMeshSurface, FxOpacity, FxQuad, FxQuadSurface, WorldFx};

pub(crate) static EDITOR_DOCUMENTS: Mutex<Option<Vec<u8>>> = Mutex::new(None);

extern "C" {
    fn kh_lens_input(x: *mut f32, y: *mut f32) -> u32;
    fn kh_lens_draw(order: u16, texture: u32, x: u16, y: u16, w: u16, h: u16) -> i32;
    fn kh_game_overlay(order: u16, texture: u32, x: u16, y: u16, w: u16, h: u16) -> i32;
    fn kh_msdf_text(
        texture: u32,
        glyphs: *const kalam_runtime::msdf::GlyphQuad,
        count: u32,
        unit_range_x: f32,
        unit_range_y: f32,
        x: u16,
        y: u16,
        width: u16,
        height: u16,
        scale_x: f32,
        scale_y: f32,
    ) -> i32;
    pub(crate) fn kh_selected(dst: *mut u8, capacity: usize) -> usize;
    fn kh_walk_keys() -> u32;
    pub(crate) fn kh_editor_camera_input(pick_x: *mut f32, pick_y: *mut f32) -> u32;
    pub(crate) fn kh_editor_picked(id: *const u8, length: usize);
    fn kh_editor_gizmo(id: *const std::ffi::c_char, transform: *const f32, projection: *const f32);
    pub(crate) fn kh_next(
        dt: *mut f32,
        dx: *mut f32,
        dy: *mut f32,
        w: *mut u32,
        h: *mut u32,
    ) -> i32;
    fn kh_mesh(vertices: *const f32, n: u32, indices: *const u16, ni: u32) -> u32;
    fn kh_dynamic_mesh(vertices: *const f32, n: u32, indices: *const u16, ni: u32) -> u32;
    fn kh_gpu_skinning() -> bool;
    fn kh_skin_mesh(vertices: *const f32, n: u32, indices: *const u16, ni: u32, joints: u16)
        -> u32;
    fn kh_skin_palette(joints: u16) -> u32;
    fn kh_upload_palette(handle: u32, values: *const f32, joints: u16) -> i32;
    fn kh_update_mesh(id: u32, vertices: *const f32, count: u32) -> i32;
    fn kh_texture(pixels: *const u8, w: u32, h: u32, srgb: i32) -> u32;
    fn kh_drop_mesh(h: u32);
    fn kh_drop_texture(h: u32);
    fn kh_begin_geometry(order: u16, shadow: i32, view: *const f32, proj: *const f32, clear: u32);
    fn kh_draw_shadow(order: u16, mesh: u32, model: *const f32, palette: u32) -> i32;
    fn kh_draw_lit(
        order: u16,
        mesh: u32,
        model: *const f32,
        base: *const f32,
        material: *const f32,
        light_vp: *const f32,
        flags: *const f32,
        sun: *const f32,
        color: *const f32,
        eye: *const f32,
        tex: u32,
        mr: u32,
        palette: u32,
        normal: u32,
    ) -> i32;
    fn kh_live_resources() -> i32;
    fn kh_tonemap(order: u16, exposure: f32, show_outline: i32);
    fn kh_overlay(order: u16, tint: *const f32);
    pub(crate) fn kh_present() -> i32;
    fn kh_asset(path: *const std::ffi::c_char, dst: *mut u8, capacity: i32) -> i32;
}

pub fn gpu_skinning_available() -> bool {
    unsafe { kh_gpu_skinning() }
}

#[derive(Default)]
pub struct Assets {
    meshes: Table<Mesh>,
    textures: Table<Texture>,
    palettes: Table<SkinPalette>,
}
impl Assets {
    pub fn skinned_mesh(&mut self, skin: &Skin) -> Result<Handle<Mesh>, String> {
        let vertices = skin.gpu_vertices();
        let indices = &skin.geometry.indices;
        if vertices.len() > 65_536 || indices.len() > u32::MAX as usize / 2 {
            return Err("skinned mesh exceeds the renderer limits".into());
        }
        self.meshes.insert(unsafe {
            kh_skin_mesh(
                vertices.as_ptr().cast(),
                vertices.len() as u32,
                indices.as_ptr(),
                indices.len() as u32,
                skin.joint_count(),
            )
        })
    }
    pub fn skin_palette(&mut self, joints: u16) -> Result<Handle<SkinPalette>, String> {
        self.palettes.insert(unsafe { kh_skin_palette(joints) })
    }
    pub fn upload_palette(
        &self,
        handle: Handle<SkinPalette>,
        rows: &[[f32; 4]],
    ) -> Result<(), String> {
        let id = self.palettes.get(handle).ok_or("invalid skin palette")?;
        if rows.is_empty()
            || !rows.len().is_multiple_of(6)
            || rows.len() > 256 * 6
            || unsafe { kh_upload_palette(id, rows.as_ptr().cast(), (rows.len() / 6) as u16) } == 0
        {
            return Err("skin palette upload does not match its joint count".into());
        }
        Ok(())
    }
    pub fn mesh(&mut self, g: &Geometry) -> Result<Handle<Mesh>, String> {
        self.meshes.insert(unsafe {
            kh_mesh(
                g.vertices.as_ptr().cast(),
                g.vertices.len() as u32,
                g.indices.as_ptr(),
                g.indices.len() as u32,
            )
        })
    }
    pub fn dynamic_mesh(&mut self, g: &Geometry) -> Result<Handle<Mesh>, String> {
        if g.vertices.len() > 65_536 || g.indices.len() > u32::MAX as usize / 2 {
            return Err("dynamic mesh exceeds the renderer limits".into());
        }
        self.meshes.insert(unsafe {
            kh_dynamic_mesh(
                g.vertices.as_ptr().cast(),
                g.vertices.len() as u32,
                g.indices.as_ptr(),
                g.indices.len() as u32,
            )
        })
    }
    pub fn update_mesh(&self, handle: Handle<Mesh>, vertices: &[[f32; 8]]) -> Result<(), String> {
        let id = self.meshes.get(handle).ok_or("invalid dynamic mesh")?;
        if vertices.len() > 65_536
            || unsafe { kh_update_mesh(id, vertices.as_ptr().cast(), vertices.len() as u32) } == 0
        {
            return Err("dynamic mesh update does not match its vertex buffer".into());
        }
        Ok(())
    }
    pub fn texture(
        &mut self,
        p: &[u8],
        w: u32,
        h: u32,
        srgb: bool,
    ) -> Result<Handle<Texture>, String> {
        assert_eq!(p.len(), (w * h * 4) as usize);
        self.textures
            .insert(unsafe { kh_texture(p.as_ptr(), w, h, srgb as i32) })
    }
    pub fn unload(&mut self) {
        self.meshes.clear(|h| unsafe { kh_drop_mesh(h) });
        self.textures.clear(|h| unsafe { kh_drop_texture(h) });
        self.palettes.clear(|h| unsafe { kh_drop_texture(h) });
    }
    pub fn forget_lost_device(&mut self) {
        self.meshes.clear(|_| {});
        self.textures.clear(|_| {});
        self.palettes.clear(|_| {});
    }
    pub fn drop_texture(&mut self, texture: Handle<Texture>) {
        self.textures
            .remove(texture, |handle| unsafe { kh_drop_texture(handle) });
    }
    pub fn drop_skin_palette(&mut self, palette: Handle<SkinPalette>) {
        self.palettes
            .remove(palette, |handle| unsafe { kh_drop_texture(handle) });
    }
    pub fn drop_mesh(&mut self, mesh: Handle<Mesh>) {
        self.meshes
            .remove(mesh, |handle| unsafe { kh_drop_mesh(handle) });
    }
    pub fn overlay(&self, handle: Handle<Texture>, rect: [u16; 4]) -> Result<(), String> {
        let texture = self.textures.get(handle).ok_or("invalid overlay texture")?;
        let [x, y, width, height] = rect;
        if width != 0
            && height != 0
            && unsafe { kh_lens_draw(5, texture, x, y, width, height) } == 0
        {
            return Err("overlay submission failed".into());
        }
        Ok(())
    }

    /// Submit a translucent panel to one of the two game UI layers.
    pub fn overlay_panel(&self, handle: Handle<Texture>, rect: [u16; 4], right: bool) -> Result<(), String> {
        let texture = self.textures.get(handle).ok_or("invalid overlay texture")?;
        let [x, y, width, height] = rect;
        if width != 0 && height != 0
            && unsafe { kh_game_overlay(5 + u16::from(right), texture, x, y, width, height) } == 0
        {
            return Err("overlay submission failed".into());
        }
        Ok(())
    }

    /// Draw cached logical glyph quads into a drawable-pixel rectangle, above
    /// the game overlays. The atlas must be uploaded as a linear texture.
    pub fn msdf_text(
        &self,
        atlas: Handle<Texture>,
        font: &kalam_runtime::msdf::Font,
        glyphs: &[kalam_runtime::msdf::GlyphQuad],
        design_size: [u32; 2],
        target_rect: [u16; 4],
    ) -> Result<(), String> {
        let [x, y, w, h] = target_rect;
        if glyphs.is_empty() || w == 0 || h == 0 {
            return Ok(());
        }
        if design_size.contains(&0) {
            return Err("MSDF panel has zero logical extent".into());
        }
        let texture = self
            .textures
            .get(atlas)
            .ok_or("invalid MSDF atlas texture")?;
        let [rx, ry] = font.unit_range();
        let sx = f32::from(w) / design_size[0] as f32;
        let sy = f32::from(h) / design_size[1] as f32;
        // Four vertices per glyph, indexed locally with u16 indices.
        for batch in glyphs.chunks(16384) {
            let ok = unsafe {
                kh_msdf_text(
                    texture,
                    batch.as_ptr(),
                    batch.len() as u32,
                    rx,
                    ry,
                    x,
                    y,
                    w,
                    h,
                    sx,
                    sy,
                )
            };
            if ok == 0 {
                return Err("MSDF glyph submission failed".into());
            }
        }
        Ok(())
    }
}
pub struct RenderAssets {
    pub sphere: Handle<Mesh>,
    floor: Handle<Mesh>,
    pub textures: HashMap<&'static str, Handle<Texture>>,
    imported: BTreeMap<String, Handle<Mesh>>,
}
pub fn upload(assets: &mut Assets, cpu: &CpuScene) -> Result<RenderAssets, String> {
    let sphere = assets.mesh(&kalam_runtime::mesh::sphere())?;
    let floor = assets.mesh(&kalam_runtime::mesh::floor())?;
    let textures = HashMap::from([
        ("fixture/white", assets.texture(&[255; 4], 1, 1, true)?),
        (
            "fixture/asymmetric",
            assets.texture(
                &[
                    220, 60, 10, 255, 20, 220, 60, 255, 40, 40, 220, 255, 255, 225, 200, 255,
                ],
                2,
                2,
                true,
            )?,
        ),
        ("fixture/neutral", assets.texture(&[255; 4], 1, 1, false)?),
    ]);
    let imported = cpu
        .meshes
        .iter()
        .map(|(id, geometry)| assets.mesh(geometry).map(|handle| (id.clone(), handle)))
        .collect::<Result<_, _>>()?;
    Ok(RenderAssets {
        sphere,
        floor,
        textures,
        imported,
    })
}

pub(crate) fn read_document(path: &std::ffi::CStr, capacity: usize) -> Result<Vec<u8>, String> {
    let mut data = vec![0; capacity];
    let count = unsafe { kh_asset(path.as_ptr(), data.as_mut_ptr(), data.len() as i32) };
    if count <= 0 || count as usize > data.len() {
        return Err(format!(
            "{}: missing, empty or exceeds {} KiB",
            path.to_string_lossy(),
            capacity / 1024
        ));
    }
    data.truncate(count as usize);
    Ok(data)
}

pub fn load_scene(name: &str, overrides: Option<&[u8]>) -> Result<CpuScene, String> {
    let read = |path: &str, capacity| {
        let path = CString::new(path).map_err(|error| error.to_string())?;
        read_document(&path, capacity)
    };
    match overrides {
        Some(overrides) => kalam_runtime::content::load_scene_with_overrides(name, overrides, read),
        None => kalam_runtime::content::load_scene(name, read),
    }
}

#[no_mangle]
pub unsafe extern "C" fn kalam_preview_documents(document: *const u8, length: usize) {
    let bytes = unsafe { std::slice::from_raw_parts(document, length) };
    *EDITOR_DOCUMENTS.lock().unwrap() = Some(bytes.to_vec());
}

#[no_mangle]
pub extern "C" fn kalam_project_document(
    scene: *const std::ffi::c_char,
    index: usize,
    path: *mut std::ffi::c_char,
    capacity: usize,
) -> usize {
    project_from_c(scene)
        .and_then(|game| game.documents().get(index))
        .map_or(0, |document| copy_to_c(document, path, capacity))
}

fn project_from_c(scene: *const std::ffi::c_char) -> Option<&'static dyn game::Application> {
    if scene.is_null() {
        return None;
    }
    game::application(unsafe { CStr::from_ptr(scene) }.to_str().ok()?)
}

fn copy_to_c(text: &str, output: *mut std::ffi::c_char, capacity: usize) -> usize {
    if output.is_null() || text.len() >= capacity {
        return 0;
    }
    unsafe {
        std::ptr::copy_nonoverlapping(text.as_ptr(), output.cast(), text.len());
        *output.add(text.len()) = 0;
    }
    text.len()
}

#[no_mangle]
pub extern "C" fn kalam_project_smoke_document(
    scene: *const std::ffi::c_char,
    path: *mut std::ffi::c_char,
    path_capacity: usize,
    text: *mut std::ffi::c_char,
    text_capacity: usize,
) -> usize {
    let Some((document, body)) = project_from_c(scene).and_then(game::Application::smoke_document)
    else {
        return 0;
    };
    if copy_to_c(document, path, path_capacity) == 0 {
        return 0;
    }
    copy_to_c(body, text, text_capacity)
}

#[no_mangle]
pub unsafe extern "C" fn kalam_validate_documents(
    name: *const std::ffi::c_char,
    document: *const u8,
    length: usize,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    let validate = || -> Result<(), String> {
        let name = unsafe { CStr::from_ptr(name) }
            .to_str()
            .map_err(|e| e.to_string())?;
        let bytes = unsafe { std::slice::from_raw_parts(document, length) };
        load_scene(name, Some(bytes))?;
        if let Some(application) = game::application(name) {
            application.validate(bytes)?;
        }
        Ok(())
    };
    match validate() {
        Ok(()) => 1,
        Err(error) => {
            if capacity > 0 {
                let count = error.len().min(capacity - 1);
                unsafe {
                    std::ptr::copy_nonoverlapping(error.as_ptr(), message, count);
                    *message.add(count) = 0;
                }
            }
            0
        }
    }
}

pub struct Draw {
    pub mesh: Handle<Mesh>,
    pub model: Mat4,
    pub base: [f32; 4],
    pub metallic: f32,
    pub roughness: f32,
    pub texture: Handle<Texture>,
    pub mr: Handle<Texture>,
    pub normal: Option<Handle<Texture>>,
    pub emissive: f32,
    pub selected: bool,
    pub palette: Option<Handle<SkinPalette>>,
}
pub fn draws<'a>(
    scene: &Scene,
    graphics: &RenderAssets,
    arena: &'a Bump,
    elapsed: f32,
    selected: &str,
) -> FrameVec<'a, Draw> {
    let mut list = FrameVec::with_capacity_in(scene.entities.order.len(), arena);
    let mut query = scene
        .entities
        .world
        .query::<(&EntityId, &Transform, &Renderable, Option<&Spin>)>();
    let view = query.view();
    for &entity in &scene.entities.order {
        let Some((id, transform, item, spin)) = view.get(entity) else {
            continue;
        };
        let material = &item.material;
        list.push(Draw {
            mesh: match &item.mesh {
                MeshKind::Sphere => graphics.sphere,
                MeshKind::Floor => graphics.floor,
                MeshKind::Imported(id) => graphics.imported[id],
            },
            model: entity_model(transform, spin, elapsed),
            base: material.base_color,
            metallic: material.metallic,
            roughness: material.roughness,
            texture: graphics.textures[material.base_texture.as_deref().unwrap_or("fixture/white")],
            mr: graphics.textures[material
                .metallic_roughness_texture
                .as_deref()
                .unwrap_or("fixture/neutral")],
            normal: None,
            emissive: 0.,
            selected: id.0 == selected,
            palette: None,
        });
    }
    list
}

fn entity_model(transform: &Transform, spin: Option<&Spin>, elapsed: f32) -> Mat4 {
    let rotation = spin.map_or(0., |spin| {
        (spin.degrees_per_second * elapsed)
            .rem_euclid(360.)
            .to_radians()
    });
    transform.model(rotation)
}

pub fn pick_entity(
    cpu: &CpuScene,
    eye: Vec3,
    direction: Vec3,
    elapsed: f32,
    max_distance: f32,
) -> Option<(String, f32)> {
    let sphere = kalam_runtime::mesh::sphere();
    let floor = kalam_runtime::mesh::floor();
    let mut nearest = max_distance;
    let mut picked = String::new();
    let mut query = cpu
        .scene
        .entities
        .world
        .query::<(&EntityId, &Transform, &Renderable, Option<&Spin>)>();
    let view = query.view();
    for &entity in &cpu.scene.entities.order {
        let Some((id, transform, renderable, spin)) = view.get(entity) else {
            continue;
        };
        let geometry = match &renderable.mesh {
            MeshKind::Sphere => &sphere,
            MeshKind::Floor => &floor,
            MeshKind::Imported(id) => &cpu.meshes[id],
        };
        let inverse = entity_model(transform, spin, elapsed).inverse();
        let origin = inverse.transform_point3(eye);
        // Preserve the ray parameter in world units so scaled meshes compare directly.
        let ray = inverse.transform_vector3(direction);
        let point = |index: u16| {
            let v = geometry.vertices[usize::from(index)];
            Vec3::new(v[0], v[1], v[2])
        };
        for triangle in geometry.indices.chunks_exact(3) {
            if let Some(distance) = ray_triangle(
                origin,
                ray,
                point(triangle[0]),
                point(triangle[1]),
                point(triangle[2]),
            ) {
                if distance >= 0.1 && distance < nearest {
                    nearest = distance;
                    picked.clone_from(&id.0);
                }
            }
        }
    }
    (!picked.is_empty()).then_some((picked, nearest))
}

pub fn entity_bounds(cpu: &CpuScene, selected: &str, elapsed: f32) -> Option<[Vec3; 2]> {
    let mut query = cpu
        .scene
        .entities
        .world
        .query::<(&EntityId, &Transform, &Renderable, Option<&Spin>)>();
    let (_, (_, transform, item, spin)) =
        query.iter().find(|(_, (id, _, _, _))| id.0 == selected)?;
    let model = entity_model(transform, spin, elapsed);
    let primitive;
    let geometry = match &item.mesh {
        MeshKind::Sphere => {
            primitive = kalam_runtime::mesh::sphere();
            &primitive
        }
        MeshKind::Floor => {
            primitive = kalam_runtime::mesh::floor();
            &primitive
        }
        MeshKind::Imported(id) => cpu.meshes.get(id)?,
    };
    let mut vertices = geometry
        .vertices
        .iter()
        .map(|v| model.transform_point3(Vec3::from_slice(&v[..3])));
    let first = vertices.next()?;
    let bounds = vertices.fold([first; 2], |[min, max], point| {
        [min.min(point), max.max(point)]
    });
    bounds
        .iter()
        .all(|point| point.is_finite())
        .then_some(bounds)
}

fn ray_triangle(origin: Vec3, ray: Vec3, a: Vec3, b: Vec3, c: Vec3) -> Option<f32> {
    let ab = b - a;
    let ac = c - a;
    let perpendicular = ray.cross(ac);
    let determinant = ab.dot(perpendicular);
    if determinant.abs() < 1e-8 {
        return None;
    }
    let relative = origin - a;
    let u = relative.dot(perpendicular) / determinant;
    let cross = relative.cross(ab);
    let v = ray.dot(cross) / determinant;
    (u >= 0. && v >= 0. && u + v <= 1.).then(|| ac.dot(cross) / determinant)
}

fn move_editor_camera(camera: &mut Camera, keys: u32, dt: f32) {
    // Bit positions match the KALAM_CAMERA constants in embedded.h.
    let axis = |positive: u32, negative: u32| {
        f32::from(keys & (1 << positive) != 0) - f32::from(keys & (1 << negative) != 0)
    };
    let previous_offset = camera_offset(camera);
    camera.yaw = (camera.yaw + axis(6, 9) * 1.5 * dt).rem_euclid(std::f32::consts::TAU);
    camera.pitch = (camera.pitch + axis(7, 8) * 1.5 * dt).clamp(-1.5, 1.5);
    let (sin, cos) = camera.yaw.sin_cos();
    let motion = Vec3::new(cos, 0., -sin) * axis(3, 0)
        + Vec3::new(-sin, 0., -cos) * axis(2, 1)
        + Vec3::Y * axis(4, 5);
    camera.target = (Vec3::from_array(camera.target) + previous_offset - camera_offset(camera)
        + motion.normalize_or_zero() * 5. * dt)
        .to_array();
}

fn camera_view(scene: &Scene, camera: &Camera, walker: Option<&Walker>) -> (Vec3, Vec3) {
    let target = Vec3::from_array(camera.target);
    walker
        .zip(scene.walk.as_ref())
        .map(|(player, config)| player.view(config))
        .unwrap_or((target + camera_offset(camera), target))
}

pub fn render_view(
    list: &[Draw],
    assets: &Assets,
    scene: &Scene,
    eye: Vec3,
    view: Mat4,
    proj: Mat4,
) -> Result<(), String> {
    render_view_at(list, assets, scene, eye, view, proj, Vec3::ZERO)
}

pub fn render_view_at(
    list: &[Draw],
    assets: &Assets,
    scene: &Scene,
    eye: Vec3,
    view: Mat4,
    proj: Mat4,
    shadow_center: Vec3,
) -> Result<(), String> {
    render_view_at_fx(list, assets, scene, eye, view, proj, shadow_center, &mut WorldFx::default())
}

pub fn render_view_at_fx(
    list: &[Draw],
    assets: &Assets,
    scene: &Scene,
    eye: Vec3,
    view: Mat4,
    proj: Mat4,
    shadow_center: Vec3,
    fx: &mut WorldFx,
) -> Result<(), String> {
    let mut selection = [0; 64];
    let length = unsafe { kh_selected(selection.as_mut_ptr(), selection.len()) };
    let selected = std::str::from_utf8(&selection[..length]).unwrap_or_default();
    let target = (!selected.is_empty())
        .then(|| {
            scene
                .entities
                .world
                .query::<(&EntityId, &Transform)>()
                .iter()
                .find(|(_, (id, _))| id.0 == selected)
                .map(|(_, (_, transform))| {
                    let [x, y, z] = transform.position;
                    let [sx, sy, sz] = transform.scale.axes();
                    [
                        x,
                        y,
                        z,
                        sx,
                        sy,
                        sz,
                        transform.pitch_degrees,
                        transform.yaw_degrees,
                        transform.roll_degrees,
                    ]
                })
        })
        .flatten();
    if let Some((transform, id)) = target.zip(CString::new(selected).ok()) {
        unsafe {
            kh_editor_gizmo(
                id.as_ptr(),
                transform.as_ptr(),
                game::gizmo_projection(proj * view).as_ptr(),
            )
        };
    } else {
        unsafe { kh_editor_gizmo(std::ptr::null(), std::ptr::null(), std::ptr::null()) };
    }
    let sun = Vec3::from_array(scene.sun.direction).normalize();
    let light_up = if sun.y.abs() > 0.99 { Vec3::Z } else { Vec3::Y };
    let light_view = Mat4::look_at_rh(shadow_center + sun * 16., shadow_center, light_up);
    let light_proj = Mat4::orthographic_rh(-8., 8., -8., 8., 0.1, 32.);
    let light_vp = light_proj * light_view;
    // bgfx schedules view IDs in increasing order. Keep dependencies here with the scene.
    const SHADOW: u16 = 0;
    const OPAQUE: u16 = 1;
    const TRANSPARENT: u16 = 2;
    const TONEMAP: u16 = 3;
    let palette = |item: &Draw| -> Result<u32, String> {
        item.palette.map_or(Ok(0), |handle| {
            assets
                .palettes
                .get(handle)
                .ok_or_else(|| "invalid draw skin palette".into())
        })
    };
    // Native code copies matrices and uniforms at submission; no frame pointers escape.
    unsafe {
        kh_begin_geometry(
            SHADOW,
            1,
            light_view.as_ref().as_ptr(),
            light_proj.as_ref().as_ptr(),
            0xffff_ffff,
        )
    };
    for item in list {
        let mesh = assets.meshes.get(item.mesh).ok_or("invalid shadow mesh")?;
        if unsafe { kh_draw_shadow(SHADOW, mesh, item.model.as_ref().as_ptr(), palette(item)?) }
            == 0
        {
            return Err("invalid native shadow mesh".into());
        }
    }
    unsafe {
        kh_begin_geometry(
            OPAQUE,
            0,
            view.as_ref().as_ptr(),
            proj.as_ref().as_ptr(),
            0x1016_2000,
        )
    };
    let mut show_outline = false;
    for item in list {
        let tex = |handle| {
            assets
                .textures
                .get(handle)
                .ok_or("invalid material texture")
        };
        let mesh = assets.meshes.get(item.mesh).ok_or("invalid mesh")?;
        let ok = unsafe {
            kh_draw_lit(
                OPAQUE,
                mesh,
                item.model.as_ref().as_ptr(),
                item.base.as_ptr(),
                [item.metallic, item.roughness, 4., item.emissive].as_ptr(),
                light_vp.as_ref().as_ptr(),
                [-0.5, 0.003, u8::from(item.selected) as f32, 0.].as_ptr(),
                sun.extend(0.).as_ref().as_ptr(),
                Vec3::from_array(scene.sun.radiance)
                    .extend(1.)
                    .as_ref()
                    .as_ptr(),
                eye.extend(1.).as_ref().as_ptr(),
                tex(item.texture)?,
                tex(item.mr)?,
                palette(item)?,
                item.normal.map(tex).transpose()?.unwrap_or(0),
            )
        };
        if ok == 0 {
            return Err("invalid native draw handle".into());
        }
        show_outline |= item.selected;
    }
    fx.render(TRANSPARENT, assets, view, proj)?;
    unsafe { kh_tonemap(TONEMAP, scene.exposure, i32::from(show_outline)) };
    Ok(())
}

fn open_panel() -> Result<Panel, String> {
    let document = read_document(c"lenses/inspect.json", 16 * 1024)?;
    Descriptor::parse(&document)
        .map(Panel::new)
        .map_err(|error| format!("lenses/inspect.json: {error}"))
}

fn release_panel_texture(texture: &mut u32) {
    if *texture != 0 {
        unsafe { kh_drop_texture(*texture) };
        *texture = 0;
    }
}

fn run(smoke: i32, name: &str, show_lens: bool) -> Result<(), String> {
    if let Some(application) = game::application(name) {
        unsafe { game::kh_game_handles_escape(application.handles_escape()) };
        let result = application.run(smoke != 0);
        unsafe { game::kh_game_handles_escape(false) };
        return result;
    }
    let mut assets = Assets::default();
    let baseline = unsafe { kh_live_resources() };
    let mut panel_texture = 0;
    let result = (|| {
        let mut cpu = load_scene(name, None)?;
        let mut graphics = upload(&mut assets, &cpu)?;
        let mut camera = cpu.scene.camera.clone();
        let mut editor_camera = false;
        let mut walker = if smoke == 0 {
            cpu.scene.walk.as_ref().map(Walker::new)
        } else {
            None
        };
        let mut scratch = Bump::new();
        let mut panel = if show_lens { Some(open_panel()?) } else { None };
        let mut frames = 0;
        let mut elapsed = 0.0;
        let mut play_revision = 0;
        loop {
            let (mut dt, mut dx, mut dy, mut w, mut h) = (0., 0., 0., 0, 0);
            match unsafe { kh_next(&mut dt, &mut dx, &mut dy, &mut w, &mut h) } {
                0 if smoke != 0 && frames < 3 => {
                    return Err("window closed before smoke completed".into())
                }
                0 => break,
                1 => (),
                2 => continue,
                3 => {
                    panel_texture = 0;
                    assets.meshes.clear(|_| {});
                    assets.textures.clear(|_| {});
                    graphics = upload(&mut assets, &cpu)?;
                    continue;
                }
                4 => {
                    let overrides = EDITOR_DOCUMENTS.lock().unwrap().take();
                    let mut pending = Assets::default();
                    let candidate = load_scene(name, overrides.as_deref()).and_then(|next| {
                        let gpu = if next.meshes != cpu.meshes {
                            Some(upload(&mut pending, &next)?)
                        } else {
                            None
                        };
                        Ok((next, gpu))
                    });
                    match candidate {
                        Ok((next, gpu)) => {
                            if overrides.is_none() || next.scene.camera != cpu.scene.camera {
                                camera = next.scene.camera.clone();
                                editor_camera = overrides.is_some();
                            }
                            if smoke == 0 {
                                walker = next.scene.walk.as_ref().map(|config| {
                                    let spawn_changed = cpu
                                        .scene
                                        .walk
                                        .as_ref()
                                        .is_some_and(|old| old.spawn_xz != config.spawn_xz);
                                    let mut player = if spawn_changed {
                                        Walker::new(config)
                                    } else {
                                        walker.take().unwrap_or_else(|| Walker::new(config))
                                    };
                                    player.constrain(config);
                                    player
                                });
                            }
                            cpu = next;
                            release_panel_texture(&mut panel_texture);
                            if let Some(gpu) = gpu {
                                assets.unload();
                                assets = pending;
                                graphics = gpu;
                            }
                        }
                        Err(error) => {
                            pending.unload();
                            eprintln!("Scene reload: {error}");
                        }
                    }
                    continue;
                }
                _ => return Err("platform event or device error".into()),
            }
            if w == 0 || h == 0 {
                continue;
            }
            let play = game::play_session();
            let simulating = play.state == game::PlayState::Playing;
            if play_revision != play.revision {
                play_revision = play.revision;
                elapsed = 0.;
                if let Some((player, config)) = walker.as_mut().zip(cpu.scene.walk.as_ref()) {
                    *player = Walker::new(config);
                }
            }
            let (mut pick_x, mut pick_y) = (0., 0.);
            let editor_input = unsafe { kh_editor_camera_input(&mut pick_x, &mut pick_y) };
            let keys = editor_input & 0x3ff;
            let editor_focus = editor_input & (1 << 31) != 0;
            let pick_requested = editor_input & (1 << 30) != 0;
            let camera_drag = editor_input & (1 << 29) != 0;
            let cursor_pick = editor_input & (1 << 28) != 0;
            let (mut x, mut y) = (0., 0.);
            let input = unsafe { kh_lens_input(&mut x, &mut y) };
            if input & 1 != 0 && smoke == 0 {
                if panel.take().is_some() {
                    release_panel_texture(&mut panel_texture);
                } else {
                    match open_panel() {
                        Ok(next) => panel = Some(next),
                        Err(error) => eprintln!("Scene controls: {error}"),
                    }
                }
            }
            let rect = panel
                .as_ref()
                .and_then(|panel| panel.rect(&cpu.scene, w, h));
            if let Some((panel, rect)) = panel.as_mut().zip(rect) {
                let dirty = smoke == 0
                    && panel.input(&mut cpu.scene, rect, x, y, input & 4 != 0, input & 2 != 0);
                if dirty || panel_texture == 0 {
                    let pixels = panel.pixels(&cpu.scene);
                    let next = unsafe {
                        kh_texture(pixels.as_ptr(), PANEL_WIDTH, panel.height(&cpu.scene), 0)
                    };
                    if next != 0 {
                        release_panel_texture(&mut panel_texture);
                        panel_texture = next;
                    } else {
                        // Retry texture creation on a later frame.
                        release_panel_texture(&mut panel_texture);
                    }
                }
            } else if panel.is_none() {
                if (keys != 0 || camera_drag) && !editor_camera {
                    if let Some((player, config)) = walker
                        .as_ref()
                        .zip(cpu.scene.walk.as_ref())
                        .filter(|_| play.state != game::PlayState::Stopped)
                    {
                        let (eye, target) = player.view(config);
                        let offset = (eye - target).normalize();
                        camera.yaw = offset.x.atan2(offset.z);
                        camera.pitch = offset.y.asin();
                        camera.target = (eye - offset * camera.distance).to_array();
                    }
                    editor_camera = true;
                }
                if editor_camera {
                    if camera_drag {
                        fly_editor_camera(&mut camera, keys, dx, dy, dt);
                    } else {
                        if keys != 0 {
                            move_editor_camera(&mut camera, keys, dt);
                        }
                        camera.yaw -= dx * 0.008;
                        camera.pitch = (camera.pitch + dy * 0.008).clamp(-1.5, 1.5);
                    }
                } else if let Some((player, config)) = walker.as_mut().zip(cpu.scene.walk.as_ref())
                {
                    if simulating {
                        player.update(config, dt, unsafe { kh_walk_keys() }, dx, dy);
                    }
                } else if smoke == 0 {
                    camera.yaw -= dx * 0.008;
                    camera.pitch = (camera.pitch + dy * 0.008).clamp(0.1, 1.50);
                }
            }
            if smoke == 0 && simulating {
                elapsed += dt;
            }
            let active_walker = walker
                .as_ref()
                .filter(|_| !editor_camera && play.state != game::PlayState::Stopped);
            let screen = Vec2::new(w as f32, h as f32);
            let viewport_rect = game::viewport_rect();
            let size = viewport_size(viewport_rect);
            let lens = |camera: &Camera| {
                let far = (camera.distance * 2.).max(60.);
                (
                    Mat4::perspective_rh(
                        camera.fov_y_degrees.to_radians(),
                        size.x / size.y,
                        0.1,
                        far,
                    ),
                    far,
                )
            };
            let (mut eye, target) = camera_view(&cpu.scene, &camera, active_walker);
            let mut view = Mat4::look_at_rh(eye, target, Vec3::Y);
            let (optics, far) = lens(&camera);
            let mut projection = viewport_projection(optics, screen, viewport_rect);
            if pick_requested && panel.is_none() {
                let ray = if cursor_pick {
                    viewport_ray(
                        (projection * view).inverse(),
                        Vec2::new(pick_x, pick_y),
                        screen,
                        eye,
                    )
                } else {
                    (target - eye).normalize()
                };
                let hit = pick_entity(&cpu, eye, ray, elapsed, far);
                game::select_entity(hit.as_ref().map_or("", |(id, _)| id));
            }
            let mut selection = [0; 64];
            let length = unsafe { kh_selected(selection.as_mut_ptr(), selection.len()) };
            let selected = std::str::from_utf8(&selection[..length]).unwrap_or_default();
            if editor_input & (1 << 26) != 0
                && !simulating
                && panel.is_none()
                && !selected.is_empty()
            {
                if let Some(bounds) = entity_bounds(&cpu, selected, elapsed) {
                    if active_walker.is_some() {
                        let back = (eye - target).normalize();
                        camera.yaw = back.x.atan2(back.z);
                        camera.pitch = back.y.asin().clamp(-1.5, 1.5);
                    }
                    frame_camera(&mut camera, bounds, optics);
                    editor_camera = true;
                    let target = Vec3::from_array(camera.target);
                    eye = target + camera_offset(&camera);
                    view = Mat4::look_at_rh(eye, target, Vec3::Y);
                    projection = viewport_projection(lens(&camera).0, screen, viewport_rect);
                }
            }
            scratch.reset();
            render_view(
                &draws(&cpu.scene, &graphics, &scratch, elapsed, selected),
                &assets,
                &cpu.scene,
                eye,
                view,
                projection,
            )?;
            unsafe { kh_overlay(4, [1.; 4].as_ptr()) };
            if let Some(rect) = rect.filter(|_| panel_texture != 0) {
                if unsafe {
                    kh_lens_draw(
                        5,
                        panel_texture,
                        rect.x as u16,
                        rect.y as u16,
                        rect.w as u16,
                        rect.h as u16,
                    )
                } == 0
                {
                    return Err("invalid native panel texture".into());
                }
            }
            if editor_focus && panel.is_none() && w >= 16 && h >= 16 {
                let white = assets
                    .textures
                    .get(graphics.textures["fixture/white"])
                    .ok_or("invalid crosshair texture")?;
                let (x, y) = (w as u16 / 2, h as u16 / 2);
                unsafe {
                    kh_lens_draw(6, white, x - 6, y, 13, 1);
                    kh_lens_draw(7, white, x, y - 6, 1, 13);
                }
            }
            let presented = unsafe { kh_present() };
            if presented < 0 {
                return Err("GPU frame submission failed".into());
            }
            if smoke != 0 && presented != 0 {
                frames += 1;
                if frames < 3 {
                    assets.unload();
                    if unsafe { kh_live_resources() } != baseline + i32::from(panel_texture != 0) {
                        return Err("scene unload leaked GPU handles".into());
                    }
                    graphics = upload(&mut assets, &cpu)?;
                }
                if frames == 3 {
                    break;
                }
            }
        }
        Ok(())
    })();
    release_panel_texture(&mut panel_texture);
    assets.unload();
    result
}
#[no_mangle]
pub unsafe extern "C" fn kh_run(smoke: i32, scene: *const std::ffi::c_char, lens: i32) -> i32 {
    // SDL's argument storage remains valid for the entire engine run.
    let name = unsafe { CStr::from_ptr(scene) }
        .to_str()
        .ok()
        .filter(|name| kalam_runtime::scene::valid_name(name));
    let Some(name) = name else {
        eprintln!("Kalam: scene name must contain only letters, digits, underscores or hyphens");
        return 1;
    };
    match run(smoke, name, lens != 0) {
        Ok(()) => 0,
        Err(e) => {
            eprintln!("Kalam: {e}");
            1
        }
    }
}
