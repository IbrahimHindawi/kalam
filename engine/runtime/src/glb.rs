use crate::mesh::Geometry;
use glam::Vec3;

const MAX_VERTICES: usize = u16::MAX as usize + 1;
const MAX_INDICES: usize = 1_048_576;

/// Decode one static mesh from a self-contained GLB. Kalam's scene supplies the
/// instance transform and material, so the primitives share one draw material.
pub fn decode(bytes: &[u8]) -> Result<Geometry, String> {
    if !bytes.starts_with(b"glTF") {
        return Err("expected a binary glTF (.glb) file".into());
    }
    let file = gltf::Gltf::from_slice(bytes).map_err(|error| error.to_string())?;
    let data = embedded_buffer(&file)?;
    decode_mesh(&file, data)
}

pub(crate) fn embedded_buffer(file: &gltf::Gltf) -> Result<&[u8], String> {
    let blob = file.blob.as_deref().ok_or("GLB has no binary chunk")?;
    let mut buffers = file.buffers();
    let buffer = buffers.next().ok_or("GLB has no buffer")?;
    if buffers.next().is_some()
        || !matches!(buffer.source(), gltf::buffer::Source::Bin)
        || buffer.length() > blob.len()
    {
        return Err("expected one embedded buffer".into());
    }
    Ok(&blob[..buffer.length()])
}

pub(crate) fn decode_mesh(file: &gltf::Document, data: &[u8]) -> Result<Geometry, String> {
    if file
        .views()
        .any(|view| view.offset() > data.len() || view.length() > data.len() - view.offset())
    {
        return Err("buffer view exceeds the mesh buffer".into());
    }
    let mut meshes = file.meshes();
    let mesh = meshes.next().ok_or("GLB has no mesh")?;
    if meshes.next().is_some() {
        return Err("expected one mesh; export separate assets for separate meshes".into());
    }

    let mut geometry = Geometry {
        vertices: Vec::new(),
        indices: Vec::new(),
    };
    for primitive in mesh.primitives() {
        if primitive.mode() != gltf::mesh::Mode::Triangles || primitive.morph_targets().len() != 0 {
            return Err("only triangle primitives without morph targets are supported".into());
        }
        for accessor in primitive
            .attributes()
            .map(|(_, accessor)| accessor)
            .chain(primitive.indices())
        {
            let view = accessor
                .view()
                .ok_or("expected a buffer view for every attribute")?;
            let size = accessor.size();
            let stride = view.stride().unwrap_or(size);
            let count = accessor.count();
            if accessor.sparse().is_some() || count == 0 || count > MAX_INDICES || stride < size {
                return Err("unsupported accessor count, stride or sparse storage".into());
            }
            let end = (count - 1)
                .checked_mul(stride)
                .and_then(|bytes| bytes.checked_add(size))
                .and_then(|bytes| bytes.checked_add(accessor.offset()));
            if end.is_none_or(|end| end > view.length()) {
                return Err("accessor exceeds its buffer view".into());
            }
        }
        use gltf::accessor::{DataType, Dimensions};
        for semantic in [gltf::Semantic::Positions, gltf::Semantic::Normals] {
            let accessor = primitive
                .get(&semantic)
                .ok_or("positions and normals are required")?;
            if accessor.data_type() != DataType::F32 || accessor.dimensions() != Dimensions::Vec3 {
                return Err("positions and normals must be float VEC3".into());
            }
        }
        if let Some(accessor) = primitive.get(&gltf::Semantic::TexCoords(0)) {
            if accessor.dimensions() != Dimensions::Vec2
                || !(accessor.data_type() == DataType::F32
                    || accessor.normalized()
                        && matches!(accessor.data_type(), DataType::U8 | DataType::U16))
            {
                return Err("TEXCOORD_0 must be a float or normalized unsigned VEC2".into());
            }
        }
        if let Some(accessor) = primitive.indices() {
            if accessor.dimensions() != Dimensions::Scalar
                || accessor.normalized()
                || !matches!(
                    accessor.data_type(),
                    DataType::U8 | DataType::U16 | DataType::U32
                )
            {
                return Err("indices must be unsigned scalars".into());
            }
        }
        let reader = primitive.reader(|_| Some(data));
        let positions = reader
            .read_positions()
            .ok_or("missing or truncated POSITION")?;
        let mut normals = reader.read_normals().ok_or("missing or truncated NORMAL")?;
        let mut uv = reader.read_tex_coords(0).map(|coords| coords.into_f32());
        if uv.is_none() && primitive.get(&gltf::Semantic::TexCoords(0)).is_some() {
            return Err("invalid TEXCOORD_0".into());
        }
        let count = positions.len();
        let base = geometry.vertices.len();
        if count == 0
            || base + count > MAX_VERTICES
            || normals.len() != count
            || uv.as_ref().is_some_and(|coords| coords.len() != count)
        {
            return Err("invalid vertex count or mismatched attributes".into());
        }
        for (p, n) in positions.zip(&mut normals) {
            let n = Vec3::from_array(n).normalize_or_zero().to_array();
            let [u, v] = uv.as_mut().and_then(Iterator::next).unwrap_or_default();
            if !p.iter().chain(&[u, v]).all(|value| value.is_finite()) || n == [0.; 3] {
                return Err("invalid position, normal or UV".into());
            }
            geometry
                .vertices
                .push([p[0], p[1], p[2], n[0], n[1], n[2], u, v]);
        }
        let append = |geometry: &mut Geometry, index: u32| -> Result<(), String> {
            if index as usize >= count || geometry.indices.len() == MAX_INDICES {
                return Err("index outside vertex range or too many indices".into());
            }
            geometry.indices.push((base + index as usize) as u16);
            Ok(())
        };
        let start = geometry.indices.len();
        match reader.read_indices() {
            Some(indices) => {
                for index in indices.into_u32() {
                    append(&mut geometry, index)?;
                }
            }
            None if primitive.indices().is_some() => return Err("truncated index data".into()),
            None => {
                for index in 0..count {
                    append(&mut geometry, index as u32)?;
                }
            }
        }
        let count = geometry.indices.len() - start;
        if count == 0 || !count.is_multiple_of(3) {
            return Err("triangle index count must be a nonzero multiple of three".into());
        }
    }
    if geometry.indices.is_empty() {
        return Err("mesh contains no triangles".into());
    }
    Ok(geometry)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn authored_room_mesh_has_valid_triangle_geometry() {
        let source = include_bytes!("../../assets/meshes/room.glb");
        let room = decode(source).unwrap();
        assert_eq!(room.indices.len(), 24);
        assert_eq!(room.vertices.len(), 16);
        assert!(room.vertices.iter().any(|v| v[1] == 2.8));
        assert!(decode(&source[..source.len() - 1]).is_err());
        assert!(decode(b"{}").is_err());
    }
}
