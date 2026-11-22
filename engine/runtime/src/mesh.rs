use std::f32::consts::{PI, TAU};

#[derive(PartialEq)]
pub struct Geometry {
    pub vertices: Vec<[f32; 8]>,
    pub indices: Vec<u16>,
}

pub fn sphere() -> Geometry {
    const RINGS: u16 = 20;
    const SLICES: u16 = 32;
    let mut vertices = Vec::new();
    let mut indices = Vec::new();
    for j in 0..=RINGS {
        let v = f32::from(j) / f32::from(RINGS);
        let (s, c) = (v * PI).sin_cos();
        for i in 0..=SLICES {
            let u = f32::from(i) / f32::from(SLICES);
            let (sz, cx) = (u * TAU).sin_cos();
            let (x, y, z) = (s * cx, c, s * sz);
            vertices.push([x, y, z, x, y, z, u, v]);
            if i < SLICES && j < RINGS {
                let a = j * (SLICES + 1) + i;
                let b = a + SLICES + 1;
                indices.extend([a, a + 1, b, b, a + 1, b + 1]);
            }
        }
    }
    Geometry { vertices, indices }
}
pub fn floor() -> Geometry {
    Geometry {
        vertices: vec![
            [-10., 0., -6., 0., 1., 0., 0., 0.],
            [-10., 0., 6., 0., 1., 0., 0., 1.],
            [10., 0., 6., 0., 1., 0., 1., 1.],
            [10., 0., -6., 0., 1., 0., 1., 0.],
        ],
        indices: vec![0, 1, 2, 0, 2, 3],
    }
}
