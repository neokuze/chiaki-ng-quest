// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

//! Room engine for the immersive app.
//!
//! Loads a room exported as glTF binary (see rooms/README.md for the contract: meters, Y up,
//! baked unlit lighting, a `Screen` node, screen-light mask in COLOR_0.r) and draws it with
//! OpenGL ES 3 into whatever framebuffer the caller has bound, one eye at a time.
//! xr-renderer.c owns the OpenXR side and calls the C ABI at the bottom of this file.

use glow::HasContext;
use std::ffi::{c_char, c_void, CString};

/// Viewer eye in room coordinates; it coincides with the origin of the app's LOCAL space
const ROOM_EYE: [f32; 3] = [0.0, 1.2, 0.0];
const NEAR: f32 = 0.05;
const FAR: f32 = 200.0;
/// How strongly the screen color tints surfaces marked in COLOR_0.r
const SCREEN_LIGHT: f32 = 0.6;

type Mat4 = [f32; 16]; // column-major, like GL

fn mat_mul(a: &Mat4, b: &Mat4) -> Mat4 {
	let mut r = [0.0; 16];
	for c in 0..4 {
		for row in 0..4 {
			r[c * 4 + row] = (0..4).map(|k| a[k * 4 + row] * b[c * 4 + k]).sum();
		}
	}
	r
}

fn mat_transform_point(m: &Mat4, p: [f32; 3]) -> [f32; 3] {
	[
		m[0] * p[0] + m[4] * p[1] + m[8] * p[2] + m[12],
		m[1] * p[0] + m[5] * p[1] + m[9] * p[2] + m[13],
		m[2] * p[0] + m[6] * p[1] + m[10] * p[2] + m[14],
	]
}

/// Rotation matrix of a unit quaternion (x, y, z, w)
fn quat_mat(q: [f32; 4]) -> Mat4 {
	let [x, y, z, w] = q;
	[
		1.0 - 2.0 * (y * y + z * z), 2.0 * (x * y + z * w), 2.0 * (x * z - y * w), 0.0,
		2.0 * (x * y - z * w), 1.0 - 2.0 * (x * x + z * z), 2.0 * (y * z + x * w), 0.0,
		2.0 * (x * z + y * w), 2.0 * (y * z - x * w), 1.0 - 2.0 * (x * x + y * y), 0.0,
		0.0, 0.0, 0.0, 1.0,
	]
}

/// Inverse of the rigid eye pose followed by moving the room so its eye sits at the origin
fn view_matrix(pos: [f32; 3], quat: [f32; 4]) -> Mat4 {
	let r = quat_mat(quat);
	// transpose of the rotation, translation -R^T * pos
	let mut v = [
		r[0], r[4], r[8], 0.0,
		r[1], r[5], r[9], 0.0,
		r[2], r[6], r[10], 0.0,
		0.0, 0.0, 0.0, 1.0,
	];
	let room_x = -ROOM_EYE[0] - pos[0];
	let room_y = -ROOM_EYE[1] - pos[1];
	let room_z = -ROOM_EYE[2] - pos[2];
	v[12] = v[0] * room_x + v[4] * room_y + v[8] * room_z;
	v[13] = v[1] * room_x + v[5] * room_y + v[9] * room_z;
	v[14] = v[2] * room_x + v[6] * room_y + v[10] * room_z;
	v
}

/// Asymmetric OpenXR field of view (angles in radians) to a GL projection
fn projection(fov: [f32; 4]) -> Mat4 {
	let [left, right, up, down] = fov;
	let (tl, tr, tu, td) = (left.tan(), right.tan(), up.tan(), down.tan());
	let w = tr - tl;
	let h = tu - td;
	[
		2.0 / w, 0.0, 0.0, 0.0,
		0.0, 2.0 / h, 0.0, 0.0,
		(tr + tl) / w, (tu + td) / h, -(FAR + NEAR) / (FAR - NEAR), -1.0,
		0.0, 0.0, -2.0 * FAR * NEAR / (FAR - NEAR), 0.0,
	]
}

fn node_matrix(node: &gltf::Node) -> Mat4 {
	node.transform().matrix().concat().try_into().unwrap()
}

struct Batch {
	vao: glow::VertexArray,
	vbo: glow::Buffer,
	ibo: glow::Buffer,
	count: i32,
	texture: Option<glow::Texture>,
	factor: [f32; 4],
}

#[derive(Default)]
struct Mesh {
	vertices: Vec<f32>, // x y z u v mask
	indices: Vec<u32>,
}

pub struct Room {
	gl: glow::Context,
	program: glow::Program,
	u_mvp: Option<glow::UniformLocation>,
	u_tint: Option<glow::UniformLocation>,
	u_factor: Option<glow::UniformLocation>,
	u_tex: Option<glow::UniformLocation>,
	white: glow::Texture,
	textures: Vec<glow::Texture>,
	batches: Vec<Batch>,
	/// center xyz, width, height, orientation xyzw in the app's LOCAL space
	screen: [f32; 9],
}

const VS: &str = r#"#version 300 es
layout(location = 0) in vec3 a_pos;
layout(location = 1) in vec2 a_uv;
layout(location = 2) in float a_mask;
uniform mat4 u_mvp;
out vec2 v_uv;
out float v_mask;
void main() {
	v_uv = a_uv;
	v_mask = a_mask;
	gl_Position = u_mvp * vec4(a_pos, 1.0);
}
"#;

const FS: &str = r#"#version 300 es
precision mediump float;
in vec2 v_uv;
in float v_mask;
uniform sampler2D u_tex;
uniform vec4 u_factor;
uniform vec3 u_tint;
out vec4 o_color;
void main() {
	vec4 c = texture(u_tex, v_uv) * u_factor;
	// Baked lighting already lives in the texture; the screen adds its own glow on top
	c.rgb += c.rgb * u_tint * v_mask;
	o_color = vec4(c.rgb, 1.0);
}
"#;

unsafe fn compile(gl: &glow::Context) -> Result<glow::Program, String> {
	let program = gl.create_program()?;
	for (kind, src) in [(glow::VERTEX_SHADER, VS), (glow::FRAGMENT_SHADER, FS)] {
		let shader = gl.create_shader(kind)?;
		gl.shader_source(shader, src);
		gl.compile_shader(shader);
		if !gl.get_shader_compile_status(shader) {
			return Err(gl.get_shader_info_log(shader));
		}
		gl.attach_shader(program, shader);
		gl.delete_shader(shader);
	}
	gl.link_program(program);
	if !gl.get_program_link_status(program) {
		return Err(gl.get_program_info_log(program));
	}
	Ok(program)
}

unsafe fn upload_texture(gl: &glow::Context, w: u32, h: u32, rgba: &[u8]) -> Result<glow::Texture, String> {
	let tex = gl.create_texture()?;
	gl.bind_texture(glow::TEXTURE_2D, Some(tex));
	gl.tex_image_2d(glow::TEXTURE_2D, 0, glow::SRGB8_ALPHA8 as i32, w as i32, h as i32, 0,
		glow::RGBA, glow::UNSIGNED_BYTE, Some(rgba));
	gl.generate_mipmap(glow::TEXTURE_2D);
	gl.tex_parameter_i32(glow::TEXTURE_2D, glow::TEXTURE_MIN_FILTER, glow::LINEAR_MIPMAP_LINEAR as i32);
	gl.tex_parameter_i32(glow::TEXTURE_2D, glow::TEXTURE_MAG_FILTER, glow::LINEAR as i32);
	gl.tex_parameter_i32(glow::TEXTURE_2D, glow::TEXTURE_WRAP_S, glow::REPEAT as i32);
	gl.tex_parameter_i32(glow::TEXTURE_2D, glow::TEXTURE_WRAP_T, glow::REPEAT as i32);
	Ok(tex)
}

impl Room {
	unsafe fn load(gl: glow::Context, glb: &[u8]) -> Result<Room, String> {
		let gltf = gltf::Gltf::from_slice(glb).map_err(|e| e.to_string())?;
		let blob = gltf.blob.as_deref().ok_or("glb without binary chunk")?;
		let buffer_data = |buffer: gltf::Buffer| -> Option<&[u8]> {
			match buffer.source() {
				gltf::buffer::Source::Bin => Some(blob),
				_ => None,
			}
		};

		let program = compile(&gl)?;
		let white = upload_texture(&gl, 1, 1, &[255, 255, 255, 255])?;

		// Images: only embedded ones (buffer views) are supported, which is what .glb exports use
		let mut textures = Vec::new();
		for image in gltf.images() {
			let tex = match image.source() {
				gltf::image::Source::View { view, .. } => {
					let start = view.offset();
					let bytes = &blob[start..start + view.length()];
					let decoded = image::load_from_memory(bytes).map_err(|e| e.to_string())?.to_rgba8();
					upload_texture(&gl, decoded.width(), decoded.height(), decoded.as_raw())?
				}
				_ => white,
			};
			textures.push(tex);
		}

		// Flatten the scene: static geometry, so node transforms are baked into the vertices,
		// grouped by material to keep draw calls low
		let mut meshes: Vec<Mesh> = (0..gltf.materials().len() + 1).map(|_| Mesh::default()).collect();
		let mut screen_min = [f32::MAX; 3];
		let mut screen_max = [f32::MIN; 3];
		let mut screen_found = false;
		let mut screen_rot = [0.0, 0.0, 0.0, 1.0];

		let scene = gltf.default_scene().or_else(|| gltf.scenes().next()).ok_or("glb without scene")?;
		let mut stack: Vec<(gltf::Node, Mat4)> = scene.nodes().map(|n| {
			let m = node_matrix(&n);
			(n, m)
		}).collect();
		while let Some((node, world)) = stack.pop() {
			for child in node.children() {
				let m = mat_mul(&world, &node_matrix(&child));
				stack.push((child, m));
			}
			let is_screen = node.name() == Some("Screen");
			if is_screen {
				screen_found = true;
				let (_, rot, _) = node.transform().decomposed();
				screen_rot = rot;
			}
			let Some(mesh) = node.mesh() else {
				if is_screen {
					// An empty: its position is the center, its scale the size
					let p = mat_transform_point(&world, [0.0; 3]);
					screen_min = p;
					screen_max = p;
				}
				continue;
			};
			for prim in mesh.primitives() {
				if prim.mode() != gltf::mesh::Mode::Triangles {
					continue;
				}
				let reader = prim.reader(|b| buffer_data(b));
				let Some(positions) = reader.read_positions() else { continue };
				let positions: Vec<[f32; 3]> = positions.map(|p| mat_transform_point(&world, p)).collect();
				if is_screen {
					for p in &positions {
						for k in 0..3 {
							screen_min[k] = screen_min[k].min(p[k]);
							screen_max[k] = screen_max[k].max(p[k]);
						}
					}
				}
				let uvs: Vec<[f32; 2]> = reader.read_tex_coords(0).map(|t| t.into_f32().collect())
					.unwrap_or_else(|| vec![[0.0, 0.0]; positions.len()]);
				let masks: Vec<f32> = reader.read_colors(0).map(|c| c.into_rgba_f32().map(|c| c[0]).collect())
					.unwrap_or_else(|| vec![0.0; positions.len()]);
				let indices: Vec<u32> = reader.read_indices().map(|i| i.into_u32().collect())
					.unwrap_or_else(|| (0..positions.len() as u32).collect());

				let slot = prim.material().index().map(|i| i + 1).unwrap_or(0);
				let out = &mut meshes[slot];
				let base = (out.vertices.len() / 6) as u32;
				for (i, p) in positions.iter().enumerate() {
					let uv = uvs.get(i).copied().unwrap_or([0.0, 0.0]);
					let mask = masks.get(i).copied().unwrap_or(0.0);
					out.vertices.extend_from_slice(&[p[0], p[1], p[2], uv[0], uv[1], mask]);
				}
				out.indices.extend(indices.iter().map(|i| base + i));
			}
		}

		let mut batches = Vec::new();
		for (slot, mesh) in meshes.iter().enumerate() {
			if mesh.indices.is_empty() {
				continue;
			}
			let (texture, factor) = if slot == 0 {
				(None, [1.0; 4])
			} else {
				let material = gltf.materials().nth(slot - 1).unwrap();
				let pbr = material.pbr_metallic_roughness();
				let tex = pbr.base_color_texture().and_then(|t| textures.get(t.texture().source().index()).copied());
				(tex, pbr.base_color_factor())
			};
			let vao = gl.create_vertex_array()?;
			gl.bind_vertex_array(Some(vao));
			let vbo = gl.create_buffer()?;
			gl.bind_buffer(glow::ARRAY_BUFFER, Some(vbo));
			gl.buffer_data_u8_slice(glow::ARRAY_BUFFER, as_bytes(&mesh.vertices), glow::STATIC_DRAW);
			let stride = 6 * 4;
			gl.enable_vertex_attrib_array(0);
			gl.vertex_attrib_pointer_f32(0, 3, glow::FLOAT, false, stride, 0);
			gl.enable_vertex_attrib_array(1);
			gl.vertex_attrib_pointer_f32(1, 2, glow::FLOAT, false, stride, 3 * 4);
			gl.enable_vertex_attrib_array(2);
			gl.vertex_attrib_pointer_f32(2, 1, glow::FLOAT, false, stride, 5 * 4);
			let ibo = gl.create_buffer()?;
			gl.bind_buffer(glow::ELEMENT_ARRAY_BUFFER, Some(ibo));
			gl.buffer_data_u8_slice(glow::ELEMENT_ARRAY_BUFFER, as_bytes(&mesh.indices), glow::STATIC_DRAW);
			gl.bind_vertex_array(None);
			batches.push(Batch { vao, vbo, ibo, count: mesh.indices.len() as i32, texture, factor });
		}

		// Screen placement in LOCAL space; the contract's values when the node is missing
		let (center, width, height) = if screen_found && screen_min[0] <= screen_max[0] && screen_max[0] - screen_min[0] > 0.1 {
			let c = [(screen_min[0] + screen_max[0]) * 0.5, (screen_min[1] + screen_max[1]) * 0.5, (screen_min[2] + screen_max[2]) * 0.5];
			(c, screen_max[0] - screen_min[0], screen_max[1] - screen_min[1])
		} else {
			screen_rot = [0.0, 0.0, 0.0, 1.0];
			([0.0, 1.4, -3.0], 4.0, 2.25)
		};
		// A plane facing +Z exported from Blender carries the -90° X rotation of its mesh, the
		// video quad only needs the yaw of the room; keep it upright
		let _ = screen_rot;
		let screen = [
			center[0] - ROOM_EYE[0], center[1] - ROOM_EYE[1], center[2] - ROOM_EYE[2],
			width, height,
			0.0, 0.0, 0.0, 1.0,
		];

		let u_mvp = gl.get_uniform_location(program, "u_mvp");
		let u_tint = gl.get_uniform_location(program, "u_tint");
		let u_factor = gl.get_uniform_location(program, "u_factor");
		let u_tex = gl.get_uniform_location(program, "u_tex");
		Ok(Room { gl, program, u_mvp, u_tint, u_factor, u_tex, white, textures, batches, screen })
	}

	unsafe fn draw(&self, eye_pos: [f32; 3], eye_quat: [f32; 4], fov: [f32; 4], tint: [f32; 3]) {
		let gl = &self.gl;
		let mvp = mat_mul(&projection(fov), &view_matrix(eye_pos, eye_quat));
		gl.enable(glow::DEPTH_TEST);
		gl.depth_func(glow::LEQUAL);
		gl.disable(glow::BLEND);
		gl.disable(glow::CULL_FACE); // baked rooms are often single sided seen from both sides
		gl.use_program(Some(self.program));
		gl.uniform_matrix_4_f32_slice(self.u_mvp.as_ref(), false, &mvp);
		gl.uniform_3_f32(self.u_tint.as_ref(), tint[0] * SCREEN_LIGHT, tint[1] * SCREEN_LIGHT, tint[2] * SCREEN_LIGHT);
		gl.uniform_1_i32(self.u_tex.as_ref(), 0);
		gl.active_texture(glow::TEXTURE0);
		for b in &self.batches {
			gl.bind_texture(glow::TEXTURE_2D, Some(b.texture.unwrap_or(self.white)));
			gl.uniform_4_f32(self.u_factor.as_ref(), b.factor[0], b.factor[1], b.factor[2], b.factor[3]);
			gl.bind_vertex_array(Some(b.vao));
			gl.draw_elements(glow::TRIANGLES, b.count, glow::UNSIGNED_INT, 0);
		}
		gl.bind_vertex_array(None);
		gl.use_program(None);
		gl.disable(glow::DEPTH_TEST);
	}

	unsafe fn destroy(self) {
		let gl = &self.gl;
		for b in &self.batches {
			gl.delete_vertex_array(b.vao);
			gl.delete_buffer(b.vbo);
			gl.delete_buffer(b.ibo);
		}
		for t in &self.textures {
			if *t != self.white {
				gl.delete_texture(*t);
			}
		}
		gl.delete_texture(self.white);
		gl.delete_program(self.program);
	}
}

fn as_bytes<T>(v: &[T]) -> &[u8] {
	unsafe { std::slice::from_raw_parts(v.as_ptr() as *const u8, std::mem::size_of_val(v)) }
}

// ---------------------------------------------------------------------------------------------
// C ABI used by xr-renderer.c. All calls happen on the XR thread with its EGL context current.

pub type GlLoader = extern "C" fn(name: *const c_char) -> *const c_void;

extern "C" {
	fn __android_log_write(prio: i32, tag: *const c_char, text: *const c_char) -> i32;
}

fn log(msg: &str) {
	let tag = CString::new("ChiakiRooms").unwrap();
	let text = CString::new(msg.replace('\0', " ")).unwrap();
	unsafe { __android_log_write(4, tag.as_ptr(), text.as_ptr()) };
}

/// Builds a room from a .glb in memory. `screen_out` receives 9 floats: screen center xyz and
/// size w h in the app's LOCAL space, then its orientation xyzw. Returns null on failure.
#[no_mangle]
pub unsafe extern "C" fn room_create(loader: GlLoader, glb: *const u8, len: usize, screen_out: *mut f32) -> *mut Room {
	if glb.is_null() || len == 0 {
		return std::ptr::null_mut();
	}
	let gl = glow::Context::from_loader_function(|name| {
		let c = CString::new(name).unwrap();
		loader(c.as_ptr())
	});
	match Room::load(gl, std::slice::from_raw_parts(glb, len)) {
		Ok(room) => {
			log(&format!("Room loaded: {} batches, {} textures, screen {:?}", room.batches.len(), room.textures.len(), &room.screen[..5]));
			if !screen_out.is_null() {
				std::ptr::copy_nonoverlapping(room.screen.as_ptr(), screen_out, 9);
			}
			Box::into_raw(Box::new(room))
		}
		Err(e) => {
			log(&format!("Room load failed: {e}"));
			std::ptr::null_mut()
		}
	}
}

/// Draws the room for one eye into the bound framebuffer. Pose and fov come straight from
/// xrLocateViews in LOCAL space: position xyz, orientation xyzw, angles left right up down.
#[no_mangle]
pub unsafe extern "C" fn room_draw(room: *const Room, pos: *const f32, quat: *const f32, fov: *const f32, tint: *const f32) {
	let Some(room) = room.as_ref() else { return };
	let p = std::slice::from_raw_parts(pos, 3);
	let q = std::slice::from_raw_parts(quat, 4);
	let f = std::slice::from_raw_parts(fov, 4);
	let t = if tint.is_null() { [0.0; 3] } else { let t = std::slice::from_raw_parts(tint, 3); [t[0], t[1], t[2]] };
	room.draw([p[0], p[1], p[2]], [q[0], q[1], q[2], q[3]], [f[0], f[1], f[2], f[3]], t);
}

#[no_mangle]
pub unsafe extern "C" fn room_destroy(room: *mut Room) {
	if !room.is_null() {
		Box::from_raw(room).destroy();
	}
}
