# Rooms for the immersive app

Each room is a glTF binary loaded by the room engine from `android/app/src/main/assets/rooms/<name>.glb`.
The sources (Blender file, bake, preview, validation) live in `rooms/<name>/`.

Contract every room follows:
- Meters, Y up, -Z forward. Viewer eye at (0, 1.2, 0) looking at -Z.
- A 16:9 node named `Screen`, 4.0 m wide, centered at (0, 1.4, -3.0), facing +Z. The game video goes there.
- Optional `Seat_*` empties at eye height for other viewpoints.
- Lighting fully baked into `baseColorTexture`, materials use `KHR_materials_unlit`, no runtime lights.
- `COLOR_0.r` = how much the surface is lit by the screen (tinted at runtime with the average video color).
- Budget: 150k triangles, 8 materials, textures up to 2048x2048, under 40 MB.
