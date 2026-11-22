use glam::{Mat4, Vec2, Vec3, Vec4};
use kalam_runtime::scene::Camera;

pub fn camera_offset(camera: &Camera) -> Vec3 {
    camera.distance
        * Vec3::new(
            camera.yaw.sin() * camera.pitch.cos(),
            camera.pitch.sin(),
            camera.yaw.cos() * camera.pitch.cos(),
        )
}

pub fn viewport_ray(inverse: Mat4, cursor: Vec2, screen: Vec2, eye: Vec3) -> Vec3 {
    let ndc = cursor * Vec2::new(2., -2.) / screen + Vec2::new(-1., 1.);
    (inverse.project_point3(ndc.extend(1.)) - eye).normalize_or_zero()
}

pub fn viewport_size(rect: [f32; 4]) -> Vec2 {
    Vec2::new(rect[2].max(1.), rect[3].max(1.))
}

/// Project into the docked viewport while rendering to the full window.
pub fn viewport_projection(projection: Mat4, screen: Vec2, rect: [f32; 4]) -> Mat4 {
    let size = viewport_size(rect);
    let scale = size / screen;
    let center = (Vec2::new(rect[0], rect[1]) * 2. + size) / screen - Vec2::ONE;
    Mat4::from_cols(
        Vec4::new(scale.x, 0., 0., 0.),
        Vec4::new(0., scale.y, 0., 0.),
        Vec4::Z,
        Vec4::new(center.x, -center.y, 0., 1.),
    ) * projection
}

/// Fit the world bounds with padding using the current viewport's optics.
pub fn frame_camera(camera: &mut Camera, [min, max]: [Vec3; 2], projection: Mat4) {
    let center = min * 0.5 + max * 0.5;
    let back = camera_offset(camera).normalize();
    let right = Vec3::Y.cross(back).normalize();
    let up = back.cross(right);
    let mut distance = 0.35_f32;
    for x in [min.x, max.x] {
        for y in [min.y, max.y] {
            for z in [min.z, max.z] {
                let corner = Vec3::new(x, y, z) - center;
                let depth = corner.dot(back);
                let h = corner.dot(right).abs() * projection.x_axis.x;
                let v = corner.dot(up).abs() * projection.y_axis.y;
                distance = distance.max(depth + (h.max(v) * 1.15).max(0.2));
            }
        }
    }
    camera.target = center.to_array();
    camera.distance = distance;
}

pub(super) fn fly_editor_camera(camera: &mut Camera, keys: u32, dx: f32, dy: f32, dt: f32) {
    const MOUSE_LOOK_RADIANS_PER_COUNT: f32 = 0.0025;
    let eye = Vec3::from_array(camera.target) + camera_offset(camera);
    camera.yaw = (camera.yaw - dx * MOUSE_LOOK_RADIANS_PER_COUNT).rem_euclid(std::f32::consts::TAU);
    camera.pitch = (camera.pitch + dy * MOUSE_LOOK_RADIANS_PER_COUNT).clamp(-1.5, 1.5);
    let offset = camera_offset(camera);
    let forward = -offset.normalize();
    let right = forward.cross(Vec3::Y).normalize();
    let sideways = f32::from(keys & 8 != 0) - f32::from(keys & 1 != 0);
    let depth = f32::from(keys & 4 != 0) - f32::from(keys & 2 != 0);
    let lift = f32::from(keys & 16 != 0) - f32::from(keys & 32 != 0);
    let up = right.cross(forward);
    let movement = (right * sideways + forward * depth + up * lift).normalize_or_zero() * 5. * dt;
    camera.target = (eye + movement - offset).to_array();
}

#[cfg(test)]
mod viewport_tests {
    use super::*;
    use glam::Mat4;

    fn camera() -> Camera {
        Camera {
            target: [0.; 3],
            distance: 10.,
            yaw: 0.2,
            pitch: 0.4,
            fov_y_degrees: 50.,
        }
    }

    #[test]
    fn cursor_ray_projects_back_to_click() {
        let camera = camera();
        let eye = camera_offset(&camera);
        let view = Mat4::look_at_rh(eye, Vec3::ZERO, Vec3::Y);
        let projection = Mat4::perspective_rh(camera.fov_y_degrees.to_radians(), 1.6, 0.1, 60.);
        let direction = viewport_ray(
            (projection * view).inverse(),
            Vec2::new(800., 200.),
            Vec2::new(1280., 800.),
            eye,
        );
        let projected = (projection * view).project_point3(eye + direction * 4.);
        assert!((projected.x - 0.25).abs() < 1e-5);
        assert!((projected.y - 0.5).abs() < 1e-5);
    }

    #[test]
    fn frame_fits_a_box_in_a_narrow_off_center_viewport() {
        let mut camera = camera();
        let bounds = [Vec3::new(-3., 1., 8.), Vec3::new(4., 4., 10.)];
        let screen = Vec2::new(1280., 800.);
        let rect = [280., 30., 260., 720.];
        let size = viewport_size(rect);
        let projection = Mat4::perspective_rh(50_f32.to_radians(), size.x / size.y, 0.1, 100.);
        frame_camera(&mut camera, bounds, projection);
        let center = Vec3::from_array(camera.target);
        let eye = center + camera_offset(&camera);
        let vp =
            viewport_projection(projection, screen, rect) * Mat4::look_at_rh(eye, center, Vec3::Y);
        let pixel = |point| {
            (vp.project_point3(point).truncate() * Vec2::new(0.5, -0.5) + Vec2::splat(0.5)) * screen
        };
        let origin = Vec2::new(rect[0], rect[1]);
        assert!(pixel(center).abs_diff_eq(origin + size * 0.5, 1e-3));
        for x in [bounds[0].x, bounds[1].x] {
            for y in [bounds[0].y, bounds[1].y] {
                for z in [bounds[0].z, bounds[1].z] {
                    let point = pixel(Vec3::new(x, y, z));
                    assert!(point.cmpgt(origin).all() && point.cmplt(origin + size).all());
                }
            }
        }
    }

    #[test]
    fn mouse_look_pivots_in_place_and_w_flies_forward() {
        let mut camera = camera();
        let eye = camera_offset(&camera);
        fly_editor_camera(&mut camera, 0, 32., -16., 0.1);
        let offset = camera_offset(&camera);
        assert!((Vec3::from_array(camera.target) + offset - eye).length() < 1e-5);
        assert!((camera.yaw - 0.12).abs() < 1e-5);
        assert!((camera.pitch - 0.36).abs() < 1e-5);
        fly_editor_camera(&mut camera, 1 << 2, 0., 0., 0.1);
        let after = Vec3::from_array(camera.target) + camera_offset(&camera);
        assert!((after - (eye - offset.normalize() * 0.5)).length() < 1e-5);
        let forward = -offset.normalize();
        let up = forward.cross(Vec3::Y).normalize().cross(forward);
        fly_editor_camera(&mut camera, 1 << 4, 0., 0., 0.1);
        let raised = Vec3::from_array(camera.target) + camera_offset(&camera);
        assert!(raised.abs_diff_eq(after + up * 0.5, 1e-5));
        fly_editor_camera(&mut camera, 1 << 5, 0., 0., 0.1);
        let lowered = Vec3::from_array(camera.target) + camera_offset(&camera);
        assert!(lowered.abs_diff_eq(after, 1e-5));
    }
}
