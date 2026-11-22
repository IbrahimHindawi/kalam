//! Single-mesh, single-skin glTF animation. Assets share immutable geometry and clips.
use crate::{catalog::valid_relative_path, glb, mesh::Geometry};
use glam::{Mat3, Mat4, Quat, Vec3, Vec4};
use gltf::animation::{util::ReadOutputs, Interpolation};

const MAX_NODES: usize = 512;
const MAX_JOINTS: usize = 256;
const MAX_KEYS: usize = 65_536;
const MAX_FILE_BYTES: usize = 16 * 1024 * 1024;

#[derive(Clone, Copy)]
struct Trs {
    translation: Vec3,
    rotation: Quat,
    scale: Vec3,
}

struct Node {
    name: Option<String>,
    parent: Option<usize>,
    rest: Trs,
    rest_local: Mat4,
    authored_matrix: bool,
}

#[derive(Clone, Copy)]
struct Influence {
    joints: [u16; 4],
    weights: [f32; 4],
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum Target {
    Translation,
    Rotation,
    Scale,
}

struct Channel {
    node: usize,
    target: Target,
    times: Vec<f32>,
    values: Vec<Vec4>,
    interpolation: Interpolation,
}

struct Clip {
    name: String,
    duration: f32,
    channels: Vec<Channel>,
    animated_nodes: Vec<(usize, Trs)>,
    dirty_globals: Vec<usize>,
}

pub struct Skin {
    identity: std::sync::Arc<()>,
    pub geometry: Geometry,
    nodes: Vec<Node>,
    order: Vec<usize>,
    rest_globals: Vec<Mat4>,
    mesh_node: usize,
    joints: Vec<(usize, Mat4)>,
    influences: Vec<Influence>,
    clips: Vec<Clip>,
}

pub struct Pose {
    identity: std::sync::Arc<()>,
    local: Vec<Trs>,
    local_matrices: Vec<Mat4>,
    globals: Vec<Mat4>,
    palette: Vec<Mat4>,
    normals: Vec<Mat3>,
    channel_cursors: Vec<usize>,
    sampled: Option<(Option<usize>, f32)>,
    cpu_deformed: bool,
    pub mesh_transform: Mat4,
    pub vertices: Vec<[f32; 8]>,
}

impl Skin {
    pub fn load(
        path: &str,
        mut read: impl FnMut(&str, usize) -> Result<Vec<u8>, String>,
    ) -> Result<Self, String> {
        load_gltf(path, &mut read, Self::decode_file)
    }

    /// Load a companion animation exported with the same node order and hierarchy.
    /// Supply the source animation name when the companion contains several clips.
    pub fn add_clip(
        &mut self,
        alias: &str,
        path: &str,
        source_name: Option<&str>,
        mut read: impl FnMut(&str, usize) -> Result<Vec<u8>, String>,
    ) -> Result<(), String> {
        if self.clip(alias).is_some() {
            return Err(format!("duplicate animation name {alias:?}"));
        }
        let clip = load_gltf(path, &mut read, |file, blob| {
            check_accessors(file, blob)?;
            if file.nodes().count() != self.nodes.len() {
                return Err("animation and mesh have different node counts".into());
            }
            let mut parents = vec![None; self.nodes.len()];
            for node in file.nodes() {
                if node.name() != self.nodes[node.index()].name.as_deref() {
                    return Err(format!(
                        "animation node {} does not match the mesh",
                        node.index()
                    ));
                }
                for child in node.children() {
                    if parents[child.index()].replace(node.index()).is_some() {
                        return Err("animation node has more than one parent".into());
                    }
                }
            }
            if self
                .nodes
                .iter()
                .zip(parents)
                .any(|(node, parent)| node.parent != parent)
            {
                return Err("animation hierarchy does not match the mesh".into());
            }
            let mut animations = file
                .animations()
                .filter(|clip| source_name.is_none_or(|name| clip.name() == Some(name)));
            let animation = animations
                .next()
                .ok_or("companion has no matching animation")?;
            if animations.next().is_some() {
                return Err("specify a unique source animation name".into());
            }
            decode_clip(animation, alias.to_owned(), &self.nodes, &self.order, blob)
        })?;
        self.clips.push(clip);
        Ok(())
    }

    pub fn decode(bytes: &[u8]) -> Result<Self, String> {
        if !bytes.starts_with(b"glTF") {
            return Err("expected a binary glTF (.glb) file".into());
        }
        let file = gltf::Gltf::from_slice(bytes).map_err(|error| error.to_string())?;
        let data = glb::embedded_buffer(&file)?;
        Self::decode_file(&file, data)
    }

    fn decode_file(file: &gltf::Document, blob: &[u8]) -> Result<Self, String> {
        let geometry = glb::decode_mesh(file, blob)?;
        check_accessors(file, blob)?;
        let mesh = file.meshes().next().ok_or("skin has no mesh")?;
        let mut instances = file.nodes().filter(|node| node.mesh().is_some());
        let mesh_node = instances.next().ok_or("skin has no mesh node")?;
        if instances.next().is_some() || file.skins().count() != 1 {
            return Err("expected one mesh instance and one skin".into());
        }
        let skin = mesh_node.skin().ok_or("mesh node has no skin")?;
        let joint_nodes: Vec<_> = skin.joints().map(|node| node.index()).collect();
        if joint_nodes.is_empty()
            || joint_nodes.len() > MAX_JOINTS
            || file.nodes().count() > MAX_NODES
        {
            return Err("skin exceeds the node or joint limit".into());
        }
        let inverse_binds: Vec<_> = match skin.reader(|_| Some(blob)).read_inverse_bind_matrices() {
            Some(matrices) => matrices.map(|m| Mat4::from_cols_array_2d(&m)).collect(),
            None => vec![Mat4::IDENTITY; joint_nodes.len()],
        };
        if inverse_binds.len() != joint_nodes.len() || inverse_binds.iter().any(|m| !m.is_finite())
        {
            return Err("invalid inverse bind matrices".into());
        }
        let mut influences = Vec::with_capacity(geometry.vertices.len());
        for primitive in mesh.primitives() {
            let reader = primitive.reader(|_| Some(blob));
            let count = reader.read_positions().ok_or("skin needs POSITION")?.len();
            let mut sets = Vec::with_capacity(4);
            if primitive.attributes().any(|(semantic, _)| {
                matches!(semantic, gltf::Semantic::Joints(set) | gltf::Semantic::Weights(set) if set > 3)
            }) {
                return Err("skin supports JOINTS and WEIGHTS sets 0 through 3".into());
            }
            for set in 0..4 {
                match (reader.read_joints(set), reader.read_weights(set)) {
                    (Some(joints), Some(weights)) => {
                        let (joints, weights) = (joints.into_u16(), weights.into_f32());
                        if joints.len() != count || weights.len() != count {
                            return Err("skin influence counts do not match POSITION".into());
                        }
                        sets.push(joints.zip(weights));
                    }
                    (None, None) if set > 0 => {}
                    _ => {
                        return Err(format!(
                            "skin needs matching JOINTS_{set} and WEIGHTS_{set}"
                        ))
                    }
                }
            }
            for _ in 0..count {
                let mut weights = [(0, 0.); 16];
                for (set, reader) in sets.iter_mut().enumerate() {
                    let (joints, values) = reader.next().ok_or("truncated skin influences")?;
                    for (slot, pair) in weights[set * 4..][..4]
                        .iter_mut()
                        .zip(joints.into_iter().zip(values))
                    {
                        *slot = pair;
                    }
                }
                influences.push(strongest_influences(weights, joint_nodes.len())?);
            }
        }
        let mut nodes = file
            .nodes()
            .map(|node| -> Result<Node, String> {
                let transform = node.transform();
                let authored_matrix = matches!(transform, gltf::scene::Transform::Matrix { .. });
                let (rest, rest_local) = if authored_matrix {
                    (
                        Trs {
                            translation: Vec3::ZERO,
                            rotation: Quat::IDENTITY,
                            scale: Vec3::ONE,
                        },
                        Mat4::from_cols_array_2d(&transform.matrix()),
                    )
                } else {
                    let (translation, rotation, scale) = transform.decomposed();
                    let rotation = Quat::from_array(rotation);
                    let norm = rotation.length_squared();
                    if !norm.is_finite() || norm <= 1e-8 {
                        return Err(format!(
                            "node {} has an invalid rest rotation",
                            node.index()
                        ));
                    }
                    let rest = Trs {
                        translation: Vec3::from_array(translation),
                        rotation: rotation.normalize(),
                        scale: Vec3::from_array(scale),
                    };
                    (rest, rest.matrix())
                };
                if !rest_local.is_finite() {
                    return Err(format!(
                        "node {} has a nonfinite rest transform",
                        node.index()
                    ));
                }
                Ok(Node {
                    name: node.name().map(str::to_owned),
                    parent: None,
                    rest,
                    rest_local,
                    authored_matrix,
                })
            })
            .collect::<Result<Vec<_>, _>>()?;
        for node in file.nodes() {
            for child in node.children() {
                if nodes[child.index()].parent.replace(node.index()).is_some() {
                    return Err("skin node has more than one parent".into());
                }
            }
        }
        let mut order = Vec::with_capacity(nodes.len());
        for (index, node) in nodes.iter().enumerate() {
            if node.parent.is_none() {
                order.push(index);
            }
        }
        let mut index = 0;
        while index < order.len() {
            if let Some(node) = file.nodes().nth(order[index]) {
                order.extend(node.children().map(|child| child.index()));
            }
            index += 1;
        }
        if order.len() != nodes.len() {
            return Err("skin hierarchy contains a cycle".into());
        }
        let mut rest_globals = vec![Mat4::IDENTITY; nodes.len()];
        for &index in &order {
            let node = &nodes[index];
            rest_globals[index] = node.parent.map_or(node.rest_local, |parent| {
                rest_globals[parent] * node.rest_local
            });
        }
        let clips = file
            .animations()
            .map(|animation| {
                let name = animation
                    .name()
                    .map(str::to_owned)
                    .unwrap_or_else(|| format!("clip-{}", animation.index()));
                decode_clip(animation, name, &nodes, &order, blob)
            })
            .collect::<Result<Vec<_>, _>>()?;
        Ok(Self {
            identity: std::sync::Arc::new(()),
            geometry,
            nodes,
            order,
            rest_globals,
            mesh_node: mesh_node.index(),
            joints: joint_nodes.into_iter().zip(inverse_binds).collect(),
            influences,
            clips,
        })
    }

    pub fn clip(&self, name: &str) -> Option<usize> {
        self.clips.iter().position(|clip| clip.name == name)
    }

    pub fn node(&self, name: &str) -> Option<usize> {
        self.nodes.iter().position(|node| node.name.as_deref() == Some(name))
    }

    pub fn node_transform(&self, pose: &Pose, node: usize) -> Option<Mat4> {
        if !std::sync::Arc::ptr_eq(&self.identity, &pose.identity) {
            return None;
        }
        pose.globals.get(node).copied()
    }

    pub fn joint_count(&self) -> u16 {
        self.joints.len() as u16
    }

    /// Immutable position, normal, UV, four joint indices, and four weights.
    pub fn gpu_vertices(&self) -> Vec<[f32; 16]> {
        self.geometry
            .vertices
            .iter()
            .zip(&self.influences)
            .map(|(source, influence)| {
                let mut vertex = [0.; 16];
                vertex[..8].copy_from_slice(source);
                vertex[8..12].copy_from_slice(&influence.joints.map(f32::from));
                vertex[12..].copy_from_slice(&influence.weights);
                vertex
            })
            .collect()
    }

    pub fn pose(&self) -> Pose {
        let mut pose = self.palette_pose();
        pose.vertices.clone_from(&self.geometry.vertices);
        pose
    }

    /// Create pose storage for palette evaluation without a CPU vertex buffer.
    pub fn palette_pose(&self) -> Pose {
        Pose {
            identity: self.identity.clone(),
            local: self.nodes.iter().map(|node| node.rest).collect(),
            local_matrices: self.nodes.iter().map(|node| node.rest_local).collect(),
            globals: self.rest_globals.clone(),
            palette: vec![Mat4::IDENTITY; self.joints.len()],
            normals: vec![Mat3::IDENTITY; self.joints.len()],
            channel_cursors: vec![
                0;
                self.clips
                    .iter()
                    .map(|clip| clip.channels.len())
                    .max()
                    .unwrap_or(0)
            ],
            sampled: None,
            cpu_deformed: false,
            mesh_transform: Mat4::IDENTITY,
            vertices: Vec::new(),
        }
    }

    /// Returns true when this pose was evaluated. Rendering can reuse the
    /// previous vertex buffer when the clip and local time are unchanged.
    pub fn sample(
        &self,
        clip_index: Option<usize>,
        seconds: f32,
        pose: &mut Pose,
    ) -> Result<bool, &'static str> {
        self.sample_palette(clip_index, seconds, pose)?;
        if pose.cpu_deformed {
            return Ok(false);
        }
        pose.vertices.resize(self.geometry.vertices.len(), [0.; 8]);
        for ((source, influence), output) in self
            .geometry
            .vertices
            .iter()
            .zip(&self.influences)
            .zip(&mut pose.vertices)
        {
            if influence.weights[0] == 0. {
                *output = *source;
                continue;
            }
            let mut position = Vec3::ZERO;
            let mut normal = Vec3::ZERO;
            let p = Vec3::new(source[0], source[1], source[2]);
            let n = Vec3::new(source[3], source[4], source[5]);
            for (&joint, &weight) in influence.joints.iter().zip(&influence.weights) {
                if weight == 0. {
                    break;
                }
                let joint = usize::from(joint);
                position += weight * pose.palette[joint].transform_point3(p);
                normal += weight * (pose.normals[joint] * n);
            }
            let normal = normal.try_normalize().unwrap_or(n);
            *output = [
                position.x, position.y, position.z, normal.x, normal.y, normal.z, source[6],
                source[7],
            ];
        }
        pose.cpu_deformed = true;
        Ok(true)
    }

    /// Evaluate the joint palette without deforming any vertices.
    pub fn sample_palette(
        &self,
        clip_index: Option<usize>,
        seconds: f32,
        pose: &mut Pose,
    ) -> Result<bool, &'static str> {
        if !std::sync::Arc::ptr_eq(&pose.identity, &self.identity) || !seconds.is_finite() {
            return Err("pose storage or time does not match the skin");
        }
        let clip = clip_index
            .map(|index| self.clips.get(index).ok_or("unknown animation clip"))
            .transpose()?;
        if clip.is_some_and(|clip| clip.channels.len() > pose.channel_cursors.len()) {
            return Err("create the pose after loading its companion clips");
        }
        let time = clip
            .filter(|clip| clip.duration > 0.)
            .map_or(0., |clip| seconds.rem_euclid(clip.duration));
        let sample = (clip_index, time);
        if pose.sampled == Some(sample) {
            return Ok(false);
        }
        pose.cpu_deformed = false;
        if pose
            .sampled
            .take()
            .is_none_or(|(previous, _)| previous != clip_index)
        {
            // Reset components and globals that were driven by the old clip.
            for (index, node) in self.nodes.iter().enumerate() {
                pose.local[index] = node.rest;
                pose.local_matrices[index] = node.rest_local;
            }
            pose.globals.copy_from_slice(&self.rest_globals);
            pose.channel_cursors.fill(0);
            if let Some(clip) = clip {
                for &(index, rest) in &clip.animated_nodes {
                    pose.local[index] = rest;
                }
            }
        }
        if let Some(clip) = clip {
            for (channel, cursor) in clip.channels.iter().zip(&mut pose.channel_cursors) {
                let value = channel.sample(time, cursor);
                let node = &mut pose.local[channel.node];
                match channel.target {
                    Target::Translation => node.translation = value.truncate(),
                    Target::Scale => node.scale = value.truncate(),
                    Target::Rotation => {
                        let norm = value.length_squared();
                        if !norm.is_finite() || norm <= 1e-8 {
                            return Err("invalid interpolated rotation");
                        }
                        node.rotation = Quat::from_array((value / norm.sqrt()).to_array());
                    }
                }
            }
            for &(index, _) in &clip.animated_nodes {
                pose.local_matrices[index] = pose.local[index].matrix();
            }
            for &index in &clip.dirty_globals {
                let local = pose.local_matrices[index];
                pose.globals[index] = self.nodes[index]
                    .parent
                    .map_or(local, |parent| pose.globals[parent] * local);
            }
        }
        pose.mesh_transform = pose.globals[self.mesh_node];
        let inverse_mesh = pose.mesh_transform.inverse();
        if !inverse_mesh.is_finite() {
            return Err("singular mesh transform");
        }
        for (index, &(node, inverse_bind)) in self.joints.iter().enumerate() {
            let joint = inverse_mesh * pose.globals[node] * inverse_bind;
            if !joint.is_finite() {
                return Err("nonfinite joint transform");
            }
            pose.palette[index] = joint;
            pose.normals[index] = normal_transform(joint);
        }
        pose.sampled = Some(sample);
        Ok(true)
    }
}

impl Pose {
    /// Write the six RGBA32F texels read by both skinned vertex shaders.
    pub fn write_gpu_palette(&self, output: &mut [[f32; 4]]) -> Result<(), &'static str> {
        if self.sampled.is_none() || output.len() != self.palette.len() * 6 {
            return Err("palette output does not match the evaluated pose");
        }
        for ((joint, normal), rows) in self
            .palette
            .iter()
            .zip(&self.normals)
            .zip(output.chunks_exact_mut(6))
        {
            let position = joint.transpose().to_cols_array_2d();
            let normal = normal.transpose().to_cols_array_2d();
            rows[..3].copy_from_slice(&position[..3]);
            for (row, vector) in rows[3..].iter_mut().zip(normal) {
                *row = [vector[0], vector[1], vector[2], 0.];
            }
        }
        Ok(())
    }
}

fn normal_transform(joint: Mat4) -> Mat3 {
    let [x, y, z] = [
        joint.x_axis.truncate(),
        joint.y_axis.truncate(),
        joint.z_axis.truncate(),
    ];
    let cofactor = Mat3::from_cols(y.cross(z), z.cross(x), x.cross(y));
    let determinant = x.dot(cofactor.x_axis);
    let inverse_transpose = cofactor * determinant.recip();
    if inverse_transpose.is_finite() {
        inverse_transpose
    } else {
        // A zero-scale bone can deliberately collapse geometry. Keep any
        // surviving surface normal and let a fully collapsed vertex use rest.
        cofactor
    }
}

fn load_gltf<T>(
    path: &str,
    read: &mut impl FnMut(&str, usize) -> Result<Vec<u8>, String>,
    decode: impl FnOnce(&gltf::Document, &[u8]) -> Result<T, String>,
) -> Result<T, String> {
    if !valid_relative_path(path) {
        return Err("invalid skinned asset path".into());
    }
    let bytes = read(path, MAX_FILE_BYTES)?;
    if bytes.len() > MAX_FILE_BYTES {
        return Err("glTF source exceeds the size limit".into());
    }
    if path.ends_with(".glb") {
        let file = gltf::Gltf::from_slice(&bytes).map_err(|err| err.to_string())?;
        return decode(&file, glb::embedded_buffer(&file)?);
    }
    if !path.ends_with(".gltf") {
        return Err("expected .gltf or .glb skin".into());
    }
    let file = gltf::Gltf::from_slice(&bytes).map_err(|error| error.to_string())?;
    let mut buffers = file.buffers();
    let buffer = buffers.next().ok_or("glTF has no buffer")?;
    let gltf::buffer::Source::Uri(uri) = buffer.source() else {
        return Err("expected a local .bin buffer for the .gltf".into());
    };
    if buffers.next().is_some() || !uri.ends_with(".bin") || !valid_relative_path(uri) {
        return Err("expected a single relative .bin buffer beside the .gltf".into());
    }
    let buffer_path = path.rsplit_once('/').map_or_else(
        || uri.to_owned(),
        |(directory, _)| format!("{directory}/{uri}"),
    );
    if buffer.length() > MAX_FILE_BYTES {
        return Err("external skin buffer exceeds the size limit".into());
    }
    let data = read(&buffer_path, buffer.length())?;
    let data = data
        .get(..buffer.length())
        .ok_or("external skin buffer is truncated")?;
    decode(&file, data)
}

fn check_accessors(file: &gltf::Document, blob: &[u8]) -> Result<(), String> {
    if file
        .views()
        .any(|view| view.offset() > blob.len() || view.length() > blob.len() - view.offset())
    {
        return Err("skin buffer view exceeds the binary data".into());
    }
    for accessor in file.accessors() {
        let view = accessor.view().ok_or("skin accessor needs a buffer view")?;
        let stride = view.stride().unwrap_or(accessor.size());
        let count = accessor.count();
        let end = count
            .checked_sub(1)
            .and_then(|rows| rows.checked_mul(stride))
            .and_then(|start| start.checked_add(accessor.size()))
            .and_then(|end| end.checked_add(accessor.offset()));
        if accessor.sparse().is_some()
            || stride < accessor.size()
            || count > 1_048_576
            || end.is_none_or(|end| end > view.length())
        {
            return Err("unsupported or truncated skin accessor".into());
        }
    }

    Ok(())
}

fn decode_clip(
    animation: gltf::Animation<'_>,
    name: String,
    nodes: &[Node],
    order: &[usize],
    blob: &[u8],
) -> Result<Clip, String> {
    let mut channels = Vec::new();
    let mut duration = 0_f32;
    for channel in animation.channels() {
        let node = channel.target().node().index();
        if matches!(
            channel.target().node().transform(),
            gltf::scene::Transform::Matrix { .. }
        ) {
            return Err("animated nodes must store local translation, rotation and scale".into());
        }
        let interpolation = channel.sampler().interpolation();
        if channel.sampler().input().count() > MAX_KEYS {
            return Err("animation channel exceeds the key limit".into());
        }
        let reader = channel.reader(|_| Some(blob));
        let times = reader
            .read_inputs()
            .ok_or("invalid animation times")?
            .collect::<Vec<_>>();
        if times.is_empty()
            || times[0] < 0.
            || times.iter().any(|time| !time.is_finite())
            || times.windows(2).any(|pair| pair[0] >= pair[1])
        {
            return Err("animation key times must increase".into());
        }
        let (target, mut values): (Target, Vec<Vec4>) =
            match reader.read_outputs().ok_or("invalid animation values")? {
                ReadOutputs::Translations(values) => (
                    Target::Translation,
                    values.map(|v| Vec3::from_array(v).extend(0.)).collect(),
                ),
                ReadOutputs::Rotations(values) => (
                    Target::Rotation,
                    values.into_f32().map(Vec4::from_array).collect(),
                ),
                ReadOutputs::Scales(values) => (
                    Target::Scale,
                    values.map(|v| Vec3::from_array(v).extend(0.)).collect(),
                ),
                ReadOutputs::MorphTargetWeights(_) => {
                    return Err("morph animation is unsupported".into())
                }
            };
        let cubic = interpolation == Interpolation::CubicSpline;
        if values.len() != times.len() * if cubic { 3 } else { 1 }
            || values.iter().any(|value| !value.is_finite())
        {
            return Err("invalid animation key values".into());
        }
        if target == Target::Rotation {
            for value in values
                .iter_mut()
                .skip(usize::from(cubic))
                .step_by(if cubic { 3 } else { 1 })
            {
                let length = value.length_squared();
                if !length.is_finite() || length <= 1e-8 {
                    return Err("invalid animation rotation".into());
                }
                *value = value.normalize();
            }
        }
        duration = duration.max(*times.last().unwrap());
        channels.push(Channel {
            node,
            target,
            times,
            values,
            interpolation,
        });
    }

    if channels.is_empty() {
        return Err("animation has no channels".into());
    }
    let mut paths = vec![[false; 3]; nodes.len()];
    for channel in &channels {
        let index = channel.target as usize;
        if std::mem::replace(&mut paths[channel.node][index], true) {
            return Err(format!(
                "{name}: duplicate channel for node {}",
                channel.node
            ));
        }
    }
    let animated_nodes = order
        .iter()
        .copied()
        .filter(|&node| paths[node].iter().any(|&set| set))
        .map(|index| nodes[index].animation_base().map(|rest| (index, rest)))
        .collect::<Result<Vec<_>, _>>()?;
    let mut dirty = vec![false; nodes.len()];
    for &(node, _) in &animated_nodes {
        dirty[node] = true;
    }
    let mut dirty_globals = Vec::new();
    for &node in order {
        if dirty[node] || nodes[node].parent.is_some_and(|parent| dirty[parent]) {
            dirty[node] = true;
            dirty_globals.push(node);
        }
    }

    Ok(Clip {
        name,
        duration,
        channels,
        animated_nodes,
        dirty_globals,
    })
}

impl Node {
    fn animation_base(&self) -> Result<Trs, String> {
        if !self.authored_matrix {
            return Ok(self.rest);
        }
        let determinant = self.rest_local.determinant();
        if !determinant.is_finite() || determinant == 0. {
            return Err("animated joint has a singular rest matrix".into());
        }
        let (scale, rotation, translation) = self.rest_local.to_scale_rotation_translation();
        let trs = Trs {
            scale,
            rotation: rotation.normalize(),
            translation,
        };
        let test = trs.matrix().to_cols_array();
        if test
            .iter()
            .zip(self.rest_local.to_cols_array())
            .any(|(&a, b)| !a.is_finite() || (a - b).abs() > 1e-4 * b.abs().max(1.))
        {
            return Err("animated joint rest matrix cannot be represented as TRS".into());
        }
        Ok(trs)
    }
}

impl Trs {
    fn matrix(&self) -> Mat4 {
        Mat4::from_scale_rotation_translation(self.scale, self.rotation, self.translation)
    }
}

impl Channel {
    fn sample(&self, time: f32, cursor: &mut usize) -> Vec4 {
        let times = &self.times;
        let last = times.len() - 1;
        let interval =
            |index: usize| times[index] <= time && (index == last || time < times[index + 1]);
        if !interval(*cursor) {
            let next = (*cursor + 1).min(last);
            *cursor = if interval(next) {
                next
            } else {
                times.partition_point(|&key| key <= time).saturating_sub(1)
            };
        }
        let a = *cursor;
        let cubic = self.interpolation == Interpolation::CubicSpline;
        let stride = if cubic { 3 } else { 1 };
        let value = |key| self.values[key * stride + usize::from(cubic)];
        let left = value(a);
        if time <= times[0] || a == last || self.interpolation == Interpolation::Step {
            return left;
        }
        let duration = times[a + 1] - times[a];
        let t = (time - times[a]) / duration;
        let mut right = value(a + 1);
        if cubic {
            let t2 = t * t;
            let t3 = t2 * t;
            let left_tangent = self.values[a * 3 + 2] * duration;
            let right_tangent = self.values[(a + 1) * 3] * duration;
            return left * (2. * t3 - 3. * t2 + 1.)
                + left_tangent * (t3 - 2. * t2 + t)
                + right * (-2. * t3 + 3. * t2)
                + right_tangent * (t3 - t2);
        }
        if self.target == Target::Rotation && left.dot(right) < 0. {
            right = -right;
        }
        left.lerp(right, t)
    }
}

fn strongest_influences(
    entries: [(u16, f32); 16],
    joint_count: usize,
) -> Result<Influence, String> {
    let mut top = [(0, 0.); 4];
    for (joint, weight) in entries {
        if !weight.is_finite() || weight < 0. || (weight > 0. && usize::from(joint) >= joint_count)
        {
            return Err("invalid joint index or skin weight".into());
        }
        if let Some(index) = top.iter().position(|&(_, other)| weight > other) {
            top[index..].rotate_right(1);
            top[index] = (joint, weight);
        }
    }
    let total: f32 = top.iter().map(|&(_, weight)| weight).sum();
    if !total.is_finite() {
        return Err("skin weights overflow".into());
    }
    let weights = if total > 0. {
        top.map(|(_, weight)| weight / total)
    } else {
        [0.; 4]
    };
    Ok(Influence {
        joints: top.map(|(joint, _)| joint),
        weights,
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::{json, Value};

    fn glb_fixture(animated: bool) -> Vec<u8> {
        let mut binary = Vec::new();
        let mut views = Vec::new();
        let mut accessors = Vec::new();
        let mut add = |bytes: &[u8], mut accessor: Value| {
            let index = accessors.len();
            accessor["bufferView"] = index.into();
            views.push(json!({"buffer":0, "byteOffset":binary.len(), "byteLength":bytes.len()}));
            binary.extend(bytes);
            accessors.push(accessor);
            index
        };
        let floats = |values: &[f32]| {
            values
                .iter()
                .flat_map(|item| item.to_le_bytes())
                .collect::<Vec<_>>()
        };
        let position = add(
            &floats(&[0., 0., 0., 1., 0., 0., 0., 1., 0.]),
            json!({"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[1,1,0]}),
        );
        let normal = add(
            &floats(&[1., 0., 0., 1., 0., 0., 1., 0., 0.]),
            json!({"componentType":5126,"count":3,"type":"VEC3"}),
        );
        let joints = add(
            &[0; 12],
            json!({"componentType":5121,"count":3,"type":"VEC4"}),
        );
        let weights = add(
            &floats(&[1., 0., 0., 0., 0., 0., 0., 0., 1., 0., 0., 0.]),
            json!({"componentType":5126,"count":3,"type":"VEC4"}),
        );
        let inverse_bind = Mat4::from_translation(Vec3::new(-2., -2., 0.));
        let inverse_bind = add(
            &floats(&inverse_bind.to_cols_array()),
            json!({"componentType":5126,"count":1,"type":"MAT4"}),
        );
        let times = add(
            &floats(&[0., 1., 2.]),
            json!({"componentType":5126,"count":3,"type":"SCALAR","min":[0],"max":[2]}),
        );
        let rotate = std::f32::consts::FRAC_1_SQRT_2;
        let rotation = add(
            &floats(&[
                0., 0., 0., 1., 0., 0., rotate, rotate, 0., 0., rotate, rotate,
            ]),
            json!({"componentType":5126,"count":3,"type":"VEC4"}),
        );
        let scale = add(
            &floats(&[1.; 9]),
            json!({"componentType":5126,"count":3,"type":"VEC3"}),
        );
        let child_local = Mat4::from_translation(Vec3::Y * 2.);
        let mut doc = json!({
            "asset":{"version":"2.0"},
            "scene":0,"scenes":[{"nodes":[1,2]}],
            "nodes":[{"matrix":child_local.to_cols_array()},
                     {"mesh":0,"skin":0,"translation":[3,0,0]},
                     {"translation":[5,0,0],"children":[0]}],
            "meshes":[{"primitives":[{"attributes":{"POSITION":position,"NORMAL":normal,
                                                  "JOINTS_0":joints,"WEIGHTS_0":weights}}]}],
            "skins":[{"joints":[0],"inverseBindMatrices":inverse_bind,"skeleton":2}],
            "accessors":accessors,"bufferViews":views,"buffers":[{"byteLength":binary.len()}]
        });
        if animated {
            let clip = |name, output, path| {
                json!({
                    "name":name,"samplers":[{"input":times,"output":output}],
                    "channels":[{"sampler":0,"target":{"node":2,"path":path}}]
                })
            };
            doc["animations"] = json!([
                clip("turn", rotation, "rotation"),
                clip("rest", scale, "scale")
            ]);
        }
        let mut json = serde_json::to_vec(&doc).unwrap();
        while json.len() % 4 != 0 {
            json.push(b' ');
        }
        let mut glb = b"glTF".to_vec();
        glb.extend(2_u32.to_le_bytes());
        glb.extend(((28 + json.len() + binary.len()) as u32).to_le_bytes());
        glb.extend((json.len() as u32).to_le_bytes());
        glb.extend(b"JSON");
        glb.append(&mut json);
        glb.extend((binary.len() as u32).to_le_bytes());
        glb.extend(b"BIN\0");
        glb.append(&mut binary);
        glb
    }

    #[test]
    fn unkeyed_child_keeps_its_matrix_and_inherits_parent_animation() {
        let skin = Skin::decode(&glb_fixture(true)).unwrap();
        let mut pose = skin.pose();
        let turn = skin.clip("turn");
        assert!(skin.sample(turn, 1., &mut pose).unwrap());
        let vertex = pose.vertices[0];
        assert!(
            (Vec3::new(vertex[0], vertex[1], vertex[2]) - Vec3::new(2., -2., 0.)).length() < 1e-5
        );
        assert!((Vec3::new(vertex[3], vertex[4], vertex[5]) - Vec3::Y).length() < 1e-5);
        assert_eq!(pose.vertices[1], skin.geometry.vertices[1]);
        assert!(!skin.sample(turn, 1., &mut pose).unwrap());
        skin.sample(turn, 2., &mut pose).unwrap(); // End of clip wraps to the first key.
        assert_eq!(pose.vertices[0], skin.geometry.vertices[0]);
        skin.sample(turn, 1., &mut pose).unwrap();
        skin.sample(skin.clip("rest"), 1., &mut pose).unwrap(); // Scale-only clip resets rotation.
        assert_eq!(pose.vertices[0], skin.geometry.vertices[0]);
        let still = Skin::decode(&glb_fixture(false)).unwrap();
        let mut pose = still.pose();
        still.sample(None, 0., &mut pose).unwrap();
        assert_eq!(pose.vertices, still.geometry.vertices);
    }

    #[test]
    fn packed_palette_matches_cpu_deformation_on_unkeyed_child() {
        let skin = Skin::decode(&glb_fixture(true)).unwrap();
        let clip = skin.clip("turn");
        let mut pose = skin.palette_pose();
        assert!(skin.sample_palette(clip, 1., &mut pose).unwrap());
        assert!(pose.vertices.is_empty());
        let mut palette = vec![[0.; 4]; usize::from(skin.joint_count()) * 6];
        pose.write_gpu_palette(&mut palette).unwrap();
        assert!(skin.sample(clip, 1., &mut pose).unwrap());
        assert!(!skin.sample(clip, 1., &mut pose).unwrap());

        for (vertex, cpu) in skin.gpu_vertices().iter().zip(&pose.vertices) {
            let position = Vec3::from_slice(&vertex[..3]);
            let normal = Vec3::from_slice(&vertex[3..6]);
            let (mut p, mut n) = (Vec3::ZERO, Vec3::ZERO);
            for (&joint, &weight) in vertex[8..12].iter().zip(&vertex[12..16]) {
                if weight == 0. {
                    continue;
                }
                let rows = &palette[joint as usize * 6..][..6];
                let transform = |offset, vector| {
                    Vec3::from_array(std::array::from_fn(|i| {
                        Vec4::from_array(rows[offset + i]).dot(vector)
                    }))
                };
                p += weight * transform(0, position.extend(1.));
                n += weight * transform(3, normal.extend(0.));
            }
            if vertex[12] == 0. {
                p = position;
            }
            assert!(p.abs_diff_eq(Vec3::from_slice(&cpu[..3]), 1e-5));
            assert!(n
                .try_normalize()
                .unwrap_or(normal)
                .abs_diff_eq(Vec3::from_slice(&cpu[3..6]), 1e-5));
        }
    }

    #[test]
    fn step_cursors_seek_and_cubic_tangents_use_key_duration() {
        let mut channel = Channel {
            node: 0,
            target: Target::Translation,
            interpolation: Interpolation::Step,
            times: vec![0., 2., 4.],
            values: vec![Vec4::ZERO, Vec4::X, Vec4::Y],
        };
        let mut cursor = 0;
        assert_eq!(channel.sample(2., &mut cursor), Vec4::X);
        assert_eq!(channel.sample(4., &mut cursor), Vec4::Y);
        assert_eq!(channel.sample(1., &mut cursor), Vec4::ZERO);
        channel.interpolation = Interpolation::CubicSpline;
        channel.times.truncate(2);
        channel.values = vec![
            Vec4::ZERO,
            Vec4::ZERO,
            Vec4::X,
            Vec4::ZERO,
            Vec4::ZERO,
            Vec4::ZERO,
        ];
        assert_eq!(channel.sample(1., &mut cursor), Vec4::X * 0.25);
    }

    #[test]
    fn strongest_four_are_retained_across_influence_sets() {
        let entries = std::array::from_fn(|index| (index as u16, index as f32));
        let influence = strongest_influences(entries, 16).unwrap();
        assert_eq!(influence.joints, [15, 14, 13, 12]);
        assert_eq!(
            influence.weights,
            [15. / 54., 14. / 54., 13. / 54., 12. / 54.]
        );
        assert!(strongest_influences(entries, 10).is_err());
    }

    #[test]
    fn normals_handle_nonuniform_and_collapsed_joint_scales() {
        let scale = Mat4::from_scale(Vec3::new(2., 4., 8.));
        let normal = normal_transform(scale);
        assert_eq!(normal * Vec3::ONE, Vec3::new(0.5, 0.25, 0.125));
        let collapsed = normal_transform(Mat4::from_scale(Vec3::new(2., 0., 8.)));
        assert!(collapsed.is_finite());
        assert_eq!((collapsed * Vec3::Y).normalize(), Vec3::Y);
    }

    #[test]
    fn companion_clip_preserves_unkeyed_matrix_components_and_checks_hierarchy() {
        let raw = glb_fixture(true);
        let len = u32::from_le_bytes(raw[12..16].try_into().unwrap()) as usize;
        let buffer = raw[28 + len..].to_vec();
        let mut clip: Value = serde_json::from_slice(&raw[20..20 + len]).unwrap();
        clip["buffers"][0]["uri"] = "pose.bin".into();
        let mut mesh = clip.clone();
        mesh.as_object_mut().unwrap().remove("animations");
        mesh["nodes"][2] = json!({
            "matrix": Mat4::from_translation(Vec3::X * 5.).to_cols_array(),
            "children":[0]
        });
        let mut assets = std::collections::BTreeMap::from([
            ("mesh/model.gltf", serde_json::to_vec(&mesh).unwrap()),
            ("mesh/pose.bin", buffer.clone()),
            ("clips/idle.gltf", serde_json::to_vec(&clip).unwrap()),
            ("clips/pose.bin", buffer),
        ]);
        let read = |path: &str, _| assets.get(path).cloned().ok_or_else(|| path.to_owned());
        let mut skin = Skin::load("mesh/model.gltf", read).unwrap();
        skin.add_clip("external", "clips/idle.gltf", Some("turn"), read)
            .unwrap();
        let mut pose = skin.pose();
        skin.sample(skin.clip("external"), 1., &mut pose).unwrap();
        let vertex = pose.vertices[0];
        assert!(
            (Vec3::new(vertex[0], vertex[1], vertex[2]) - Vec3::new(2., -2., 0.)).length() < 1e-5
        );
        clip["nodes"][2].as_object_mut().unwrap().remove("children");
        assets.insert("clips/idle.gltf", serde_json::to_vec(&clip).unwrap());
        assert!(skin
            .add_clip("bad", "clips/idle.gltf", Some("turn"), |path, _| assets
                .get(path)
                .cloned()
                .ok_or_else(|| path.to_owned()))
            .unwrap_err()
            .contains("hierarchy"));
    }
}
