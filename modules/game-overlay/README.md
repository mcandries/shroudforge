# Game overlay module

This module draws mod-defined zones (text, images, frames) over the game image. It is an implicit Vulkan layer, `shroudforge_overlay.dll`, shipped in `shroudforge/overlay` with its manifest `shroudforge_overlay.json`.

The bootstrap (`modules/entrypoints/windows-bootstrap/startup.cpp`) enables it in the game client only. It adds the folder to `VK_ADD_IMPLICIT_LAYER_PATH` and sets `SHROUDFORGE_OVERLAY_ENABLE=1` before Enshrouded creates its Vulkan instance. It skips this when `modules.gameOverlay.enabled` is false in `modloader-config.json`, or when `SHROUDFORGE_OVERLAY=0`. Processes started by the game inherit these variables, but the layer does nothing outside `enshrouded.exe`.

| Action | Implementation |
| --- | --- |
| Insert the layer, track devices, queues and swapchains | `src/layer_entry.cpp` |
| Draw the zones in the presented image with Dear ImGui | `src/renderer.cpp` |
| Lay out and draw zones and items | `src/draw_scene.cpp` |
| Store zones and options, C API for the runtime | `src/scene.cpp` |
| Decode PNG and JPEG images | `src/load_image.cpp` |
| Read the player options, handle the show/hide key | `src/player_options.cpp` |
| Player options in the Modloader (F9) | `ui/settings.ts`, `modules/loader-config/ui/show-module-settings.tsx` |
| Log to `shroudforge_overlay.log` beside the DLL | `src/log.cpp` |
| Present test with a readback | `tests/present_test.cpp` |

## Rendering

Mods describe zones (retained mode). At each `vkQueuePresentKHR` with at least one visible zone, the layer:

1. records a render pass over the presented image (load, draw, store);
2. submits it after the game's wait semaphores;
3. makes the real present wait on its own semaphore.

Without a visible zone, the present is passed through untouched and the layer does no GPU work. The ImGui backend starts on the first present that has something to draw, on that queue. Unsupported cases are not drawn: a swapchain without `COLOR_ATTACHMENT` usage, or a format other than 8-bit or 10-bit UNORM/SRGB (HDR formats are left alone).

Fonts are TrueType or OpenType files rasterized on demand by Dear ImGui 1.92. The default font is Segoe UI, or the ImGui default if it is missing. Images and fonts stay loaded for the session (at most 100 images).

## Player options

The player edits them in the Modloader (F9, Settings, Modules, Game overlay). They are saved in `modules.gameOverlay` of `modloader-config.json`:

| Key | Default | Effect |
| --- | --- | --- |
| `enabled` | `true` | Read by the bootstrap at game start: the layer is not loaded at all when false. Also read live by the layer, which stops drawing. |
| `scale` | `100` | Size of every zone, in percent (25 to 400), on top of the screen scale. |
| `opacity` | `100` | Opacity of every zone, in percent. |
| `hideKey` | `""` | Keyboard shortcut that shows or hides every zone while the game has focus, as a ShroudForge SDL binding such as `Shift+SDL:69` (Shift+F12); empty for none. Captured in F9 with the shared keybind field (`hotkeys/ui/edit-keybind.tsx`). The layer reads its keyboard part; a mouse alternative is ignored. |
| `hideInMenus` | `true` | No zone while a game menu is open. A menu is open when the game window has the focus and shows the mouse cursor: Enshrouded hides it while playing, its shape menu included, and shows it in the inventory, the map and the pause menu. The cursor must stay shown for 250 ms: while playing, the game shows it for a frame or two now and then, which would make the zones blink. Mods read it as `status().menu`. |

The layer rereads the file at most once per second, only when its modification time changed, so changes apply while the game runs.

## C API

The runtime (same process) finds the layer with `GetModuleHandleW(L"shroudforge_overlay.dll")` and calls these exports. All strings are UTF-8.

| Export | Use |
| --- | --- |
| `int ShroudforgeOverlay_ApiVersion()` | Version of this API (1). |
| `int ShroudforgeOverlay_SetZone(owner, id, json, error, error_size)` | Create or replace zone `id` of mod `owner`. Returns 0, or -1 with a message. |
| `void ShroudforgeOverlay_RemoveZone(owner, id)` | Remove one zone. |
| `void ShroudforgeOverlay_ClearOwner(owner)` | Remove every zone of a mod. |
| `int ShroudforgeOverlay_GetStatus(out, out_size)` | `{"active", "width", "height", "scale"}` of the current output. |

Paths in zones must be absolute. Mods use the Lua API `shroudforge.overlay` (`set`, `remove`, `clear`, `status`, documented in `modules/mod-runtime/lua/shroudforge/v1/definitions/mod_api.lua`). The runtime resolves the `image` and `font` fields inside the mod's folder (`resolve_overlay_paths.rs`; zip packages are refused) and calls the layer (`game_overlay.rs`). A mod's zones are removed when it stops running (`session/record_runtime_state.rs`). Limits: 32 zones per mod, 256 items per zone, 4096 bytes per text, rows nested 3 deep.

## Zone format

Sizes and offsets are reference pixels of a screen 1080 pixels high, or `reference` pixels high when the zone sets it (900 for a design made at 1600x900). They are multiplied by `screen height / reference × scale option`, the same factor in both directions: nothing is distorted, and on a wider screen only the anchors move. Colors are `"#RRGGBB"`, `"#RRGGBBAA"` or `[r, g, b, a]`.

| Field | Meaning |
| --- | --- |
| `anchor` | Screen point: `top-left`, `top`, `top-right`, `left`, `center`, `right`, `bottom-left`, `bottom`, `bottom-right`, or `[x, y]` as fractions of the screen. Default `top-left`. |
| `pivot` | Point of the zone placed on the anchor, same values as fractions of the zone. Default: same as `anchor`. |
| `offset` | `[x, y]` moved from that position. |
| `size` | `[width, height]`; 0 fits the content. With a width, text wraps. |
| `reference` | Screen height the zone's pixel values are designed for (240 to 4320). Default 1080. |
| `order` | Drawing order, lower first. Zones holding only `world` items are drawn before all others, under the panels. |
| `visible` | `false` hides the zone without removing it. |
| `background`, `border`, `border_width`, `rounding` | Plain background and border. |
| `frame` | Nine-slice image: `{image, slice = n or [left, top, right, bottom] (image pixels), scale, tint}`. |
| `padding`, `spacing` | Inner margin (`n` or `[x, y]`) and space between items. |
| `text` | Default text style of the zone: `font` (TTF path), `size`, `color`, `shadow`, `shadow_color`, `shadow_offset` (`[x, y]`, default `[1.5, 1.5]`), `outline`, `outline_color`. |
| `items` | List, drawn top to bottom. |

Items (a plain string is a text item):

| `type` | Fields |
| --- | --- |
| `text` | `text`, the text style fields, `align` (`left`, `center`, `right`). |
| `image` | `image` (path), `size` (`n`, `[w, h]`, one side 0 keeps the ratio; default: image size), `tint`, `uv` `[u0, v0, u1, v1]` for an icon in an atlas, `rounding`, `align`. |
| `row` | `items` side by side, centered vertically, `spacing`, text style fields inherited by its items. A row holding a `grow` spans the zone width. |
| `grow` | In a row: takes the width left, to push the next items to the right. |
| `flow` | `items` (text, chip, icon, image, toggle) laid out like a sentence: texts are cut into words and every line wraps at the zone width. `spacing` between non-text items (default 5), `line_gap` between lines. Punctuation sticks to what precedes it. Chinese and Japanese text, which has no spaces, may wrap between any two characters, never before closing punctuation, small kana or `ー`, nor after an opening bracket. |
| `chip` | `text` on a rounded background, such as a key: text style fields (default dark text, no shadow), `background` (default `#F2EFE7`), `rounding` (3), `padding` (horizontal, 4), `box` `[min width, height]` (`[17, 17]`). |
| `icon` | `path`: SVG path data stroked as lines (`M L H V A Z`, absolute or relative; curves are refused), `viewbox` (square side, 24), `size` (18), `stroke` (1.8, in viewbox units), `color` (default: the text color). |
| `toggle` | `on`, `size` (`[26, 14]`), `color` (track off), `on_color` (track on, `#C9A24E`), `knob`. |
| `spacer` | `size`: height. |
| `separator` | `color`, `thickness`: a line across the zone. |
| `progress` | `value` 0 to 1, `size`, `color`, `background`, `rounding`. |
| `grid` | `rows`: strings of ASCII characters (64 rows and 64 characters at most), one cell per character; `colors`: character → color, a character without a color leaves its cell empty; `marks` (optional): character → color of a centred square of about half the cell, drawn over its color, to flag a cell without hiding what it holds; `cell` (side, 8), `gap` (between cells, 1), `rounding`. For maps, shapes or small charts drawn cell by cell in a single item. |
| `world` | Points and segments of the game world, drawn where the game's camera puts them on the screen, at every frame, over the whole screen whatever the zone's place. `points`: up to 256 `{at = {x, y, z}, color, size, shape}` (meters; `size` in reference pixels, 12; `shape` `ring`, `cross` or `dot`); `quads`: up to 64 `{corners = {four {x, y, z}}, color}`, filled flat quadrilaterals (meters, corners in order), drawn under the lines. `lines`: up to 256 `{from, to, color, width}`; `aim_face`: `{color, width, origin = {x, y, z}, cell}`, the face the building cursor aims at, outlined at every frame from the cursor read for that frame (grid of `cell` meters, 0.5, from `origin`), not drawn while it aims at nothing; `camera_lead`: seconds (-0.2 to 0.2) the camera is foreseen ahead from its mean turn and move over the last 0.1 s (at most 8 degrees); negative, the camera read that long before is used instead. How the camera read changes is then logged every 5 s. The camera is the local client's, read from ShroudForge's runtime (`KfcRuntimeReadLocalCamera`) for each frame: nothing is drawn without it. |
| `voxels` | A box of cells seen in 3D, in a view of `size` reference pixels (240 × 180). `box`: `{x, y, z}` cells, 1 to 128 each; `cells`: run-length text of the cells, x fastest, then y, then z, one ASCII character per cell, a decimal count before a character repeats it (`"40.3#"` is 40 `.` then 3 `#`); `colors`: character → color, a character without a color is empty; `marks` (optional): character → color of a dot drawn on top of its cells. Camera: `yaw` (degrees, 0 looks along +z with +x on the right and +y up), `pitch` (degrees looking down, 30), `zoom` (1 fits the box), `spin` (degrees per second added to the yaw by the overlay, so a turning view needs no new zone). `opacity` (0 to 1): the view over the game; see-through (this or the layer's opacity below 1), the solid view is drawn opaque into an off-screen image as large as the screen, then laid over the game at once, so it is the opaque view made lighter (where the renderer cannot create that image, only the faces seen from the camera are drawn, a ray from five points of each face). `xray`: every face turned to the camera, the faces between cells of different characters too, those of the `ghost` characters at `ghost_alpha` (0.6); `edge`: outline color of the faces (cells of 3 pixels or more); `plain`: in solid view, no outlines nor anti-aliased borders, one continuous surface even see-through; `ease`: seconds a new `yaw` or `pitch` takes to be reached (the view turns instead of jumping, kept per zone), 0 at once; `follow_camera`: the view turns with the game's camera at every frame, its yaw being the camera's plus `camera_offset` degrees (`yaw` stays the angle used while the camera cannot be read); `perspective` (0 to 1): 0 a parallel view (lines stay parallel), up to 1 a perspective with a field of 90 degrees, the box still fitting the view; `grid` and `grid_color`: lines on the floor of the box every `grid` cells. The faces are built once when the zone is set, then projected and drawn from the farthest at every frame, lit by their direction. |

## Test

Build with `dev/build/build_game_overlay.ps1`. Copy `build/game-overlay/Release/overlay_present_test.exe` beside the built layer as `enshrouded.exe`. Then run it with `VK_ADD_IMPLICIT_LAYER_PATH` set to that folder and `SHROUDFORGE_OVERLAY_ENABLE=1`:

```powershell
enshrouded.exe modules/game-overlay/tests/assets capture.bmp
```

It publishes three test zones, presents frames and saves the last presented image.
