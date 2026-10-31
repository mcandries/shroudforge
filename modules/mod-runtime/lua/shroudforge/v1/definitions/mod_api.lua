--- @meta

--- @class ShroudForgeSettings
--- Reads the current values declared by the package's `extended.mod.json` and player config.
--- Runtime mods should read values in lifecycle callbacks when changes must apply live.
shroudforge_settings = {}

--- Returns the current player-configured value for a setting declared in `extended.mod.json`.
--- Read it when a runtime callback or action runs so saved changes are picked up.
--- @param key string Setting key declared by the mod.
--- @param fallback? boolean|string|number|any[] Value to use when the key has no saved value.
--- @return boolean|string|number|any[]|nil
function shroudforge_settings.get(key, fallback) end

--- @class ShroudForgeUi
--- Connects safe declarative UI actions to the current runtime mod.
shroudforge_ui = {}

--- Registers a Lua callback for a button declared in `extended.mod.json`.
--- The callback runs when the player activates the matching button in the Modloader.
--- @param action string Action identifier declared by a button in `extended.mod.json`.
--- @param callback fun() Function to run after that button is activated.
function shroudforge_ui.on_action(action, callback) end

--- Requests the same Modloader UI toggle as the configured shortcut.
--- This signals the client UI process directly and does not synthesize a key press.
function shroudforge_ui.toggle_modloader() end

--- A message published to this player's local Modloader news feed.
--- Reusing the same ID from this mod updates the existing notice.
--- @class ShroudForgeNotification
--- @field id string Stable notice ID scoped to the publishing mod.
--- @field level? 'info'|'success'|'warning'|'error'|'update' Visual category used for the notice.
--- @field title string Title shown in the player's local news feed. May contain `{username}`.
--- @field message string Message shown in the player's local news feed. May contain `{username}`.
--- @field action_url? string HTTPS URL opened from the notice.

--- @class ShroudForgeNotifications
shroudforge_notifications = {}

--- Publish or update a notice in the player's Modloader news feed.
--- This does not distribute the notice to other players.
--- @param notice ShroudForgeNotification
function shroudforge_notifications.publish(notice) end

--- @class ShroudForgeI18n
--- Reads mod-authored text from the same `locales/<locale>.json` files that
--- localize the mod's Modloader name, settings, groups, actions and changelog.
shroudforge_i18n = {}

--- Returns this mod's translated string for the requested locale.
--- Regional catalogs take precedence over their base language. Missing keys
--- return `fallback`, or the key itself when no fallback is supplied.
--- Pass a value returned by `shroudforge.locale()` and cache it between
--- settings changes. The catalog is loaded with the mod, so lookup does not
--- read files or loader settings.
--- @param key string String key declared under `strings` in a mod locale file.
--- @param locale string Locale code, for example `"de"` or `"de-AT"`.
--- @param fallback? string Text to return when this mod has no translation.
--- @return string
function shroudforge_i18n.get(key, locale, fallback) end

--- @class ShroudForgeInput
shroudforge_input = {}

--- @deprecated Removed and unsupported in ShroudForge 1.7.6. Calling it logs a migration error and raises a Lua error.
--- Declare a `control: "keybind"` setting, assign it in Compatibility → Hotkeys, and use is_keybind_down or was_keybind_pressed.
--- @vararg any
function shroudforge_input.is_key_down(...) end

--- Returns true while either configured keyboard or mouse chord and its exact modifiers are held
--- and a window in the game process is focused. The setting must declare `control: "keybind"`
--- in `extended.mod.json`. Live saved changes reach the runtime on its one-second configuration refresh.
--- Other settings follow the existing mod-settings live/restart policy.
--- An empty binding or a false enabledWhen condition disables the shortcut.
--- Poll from `on_update` and detect rising edges.
--- @param setting_key string Key of this mod's declared keybind setting.
--- @return boolean
function shroudforge_input.is_keybind_down(setting_key) end

--- Returns true once when the configured shortcut was pressed since this mod last queried it.
--- Short taps between runtime updates are retained. The setting must declare control: "keybind".
--- An empty binding or a false enabledWhen condition disables the shortcut.
--- @param setting_key string Key of this mod's declared keybind setting.
--- @return boolean
function shroudforge_input.was_keybind_pressed(setting_key) end

--- Returns the current display label supplied by SDL, including modifiers and both alternatives.
--- One setting string stores a keyboard chord, a mouse chord, or keyboard|mouse,
--- for example `Ctrl+SDL:29|Mouse:4`. The editor shows separate columns for each device.
--- Returns nil when the shortcut is disabled.
--- @param setting_key string Key of this mod's declared keybind setting.
--- @return string|nil
function shroudforge_input.keybind_label(setting_key) end

--- A point on the screen or in a zone: a name, or `{x, y}` as fractions (0 to 1).
--- @alias ShroudForgeOverlayPoint 'top-left'|'top'|'top-right'|'left'|'center'|'right'|'bottom-left'|'bottom'|'bottom-right'|number[]

--- Colors are `"#RRGGBB"`, `"#RRGGBBAA"` or `{r, g, b, a}` (0 to 255).
--- @alias ShroudForgeOverlayColor string|integer[]

--- Text style of a zone, a row or a text item.
--- @class ShroudForgeOverlayTextStyle
--- @field font? string TrueType or OpenType file, relative to the mod folder. Default: Segoe UI.
--- @field size? number Reference pixels.
--- @field color? ShroudForgeOverlayColor
--- @field shadow? boolean
--- @field shadow_color? ShroudForgeOverlayColor
--- @field shadow_offset? number[] `{x, y}` in reference pixels. Default `{1.5, 1.5}`.
--- @field outline? number Outline width in reference pixels.
--- @field outline_color? ShroudForgeOverlayColor

--- One line of a zone. A plain string is a text item.
--- @class ShroudForgeOverlayItem: ShroudForgeOverlayTextStyle
--- @field type? 'text'|'image'|'row'|'flow'|'chip'|'icon'|'toggle'|'grow'|'spacer'|'separator'|'progress'|'grid'|'voxels'|'world'|'drum' Default: `text`.
--- @field text? string Text or chip content.
--- @field align? 'left'|'center'|'right'
--- @field image? string PNG or JPEG file, relative to the mod folder.
--- @field size? number|number[] Text size for text and chips; size of an image, icon, toggle or bar; spacer height.
--- @field tint? ShroudForgeOverlayColor Image color multiplier.
--- @field uv? number[] `{u0, v0, u1, v1}` part of the image, for icons in an atlas.
--- @field rounding? number
--- @field items? (ShroudForgeOverlayItem|string)[] Row content side by side, or flow content wrapped like a sentence.
--- @field spacing? number Space between the items of a row or the non-text items of a flow.
--- @field line_gap? number Flow: space between wrapped lines.
--- @field indent? number Flow: lines after the first start this much further right (a hanging indent).
--- @field color? ShroudForgeOverlayColor Separator, bar or icon color; toggle track when off.
--- @field background? ShroudForgeOverlayColor Bar or chip background.
--- @field thickness? number Separator thickness.
--- @field value? number Bar fill, 0 to 1.
--- @field box? number[] Chip `{min width, height}`, default `{17, 17}`; voxels: `{x, y, z}` cells (1 to 256 each).
--- @field padding? number Chip horizontal padding. Default 4.
--- @field path? string Icon: SVG path data (M L H V A Z), stroked.
--- @field viewbox? number Icon box side. Default 24.
--- @field stroke? number Icon line width, in viewbox units. Default 1.8.
--- @field on? boolean Toggle state.
--- @field on_color? ShroudForgeOverlayColor Toggle track when on.
--- @field knob? ShroudForgeOverlayColor Toggle knob.
--- @field rows? string[] Grid: one string per row, one cell per ASCII character (64 x 64 at most).
--- @field colors? table<string, ShroudForgeOverlayColor> Grid or voxels: color of each character; others leave their cell empty.
--- @field marks? table<string, ShroudForgeOverlayColor> Grid: a centred square over the cells of these characters; voxels: a dot drawn on top.
--- @field cell? number Grid: cell side. Default 8.
--- @field gap? number Grid: space between cells. Default 1.
--- @field cells? string Voxels: run-length text of the box's cells, x fastest, then y, then z; each ASCII character is a cell, a count before it repeats it (`"40.3#"`).
--- @field yaw? number Voxels: camera turn in degrees, 0 looks along +z with +x on the right. Default 0.
--- @field pitch? number Voxels: camera tilt in degrees, looking down when positive. Default 30.
--- @field zoom? number Voxels: 1 fits the box in the view. Default 1.
--- @field spin? number Voxels: degrees per second added to the yaw, turned by the overlay itself. Default 0.
--- @field xray? boolean Voxels: draw every face turned to the camera, inner ones included, `ghost` ones at `ghost_alpha`.
--- @field ghost_alpha? number Voxels: opacity of the `ghost` characters' faces in X-ray, 0 to 1. Default 0.6.
--- @field opacity? number Voxels: opacity of the view over the game, 0 to 1; see-through, the opaque view made lighter (drawn off screen), hidden faces stay hidden. Default 1.
--- @field plain? boolean Voxels: solid view without outlines nor anti-aliased borders, one continuous surface.
--- @field ease? number Voxels: seconds a new yaw or pitch takes to be reached (the view turns), 0 at once. Default 0.
--- @field perspective? number Voxels: 0 a parallel view, up to 1 a perspective with a field of 90 degrees. Default 0.
--- @field follow_camera? boolean Voxels: turn with the game's camera at every frame (its yaw plus `camera_offset`); `yaw` is used while the camera cannot be read.
--- @field camera_offset? number Voxels: degrees added to the game camera's yaw with `follow_camera`.
--- @field points? table[] World: up to 256 `{at = {x, y, z}, color?, size?, shape?}` in meters, drawn where the game's camera puts them at every frame; shape `ring` (default), `cross` or `dot`.
--- @field options? string[] Drum: 1 to 8 texts, the choices.
--- @field index? integer Drum: the current choice, 1-based.
--- @field id? string Drum: keeps its turn between updates of the zone.
--- @field lines? table[] World: up to 2048 `{from = {x, y, z}, to = {x, y, z}, color?, width?}` in meters; voxels: up to 256 in cells of the box, drawn over everything.
--- @field aim_face? table World: `{color?, width?, origin?, cell?}`, the face the building cursor aims at, outlined at every frame.
--- @field cells? table World: `{origin = {x, y, z}, cell?, box = {x, y, z}, data, colors, smooth?, edge_opacity?}`, a box of cells drawn in the world; `smooth`: outlined only where its surface turns; `edge_opacity`: 0 to 1 (0.9).
--- @field solids? table World: `{origin = {x, y, z}, cell?, box = {x, y, z}, data}`, filled cells (`#` in `data`): quads and lines behind them are drawn lighter; the wire view of `cells` walks them to tell what hides an edge.
--- @field quads? table[] World: up to 64 filled quadrilaterals {corners = four {x, y, z}, color}.
--- @field camera_lead? number World: seconds (-0.2 to 0.2) the camera is foreseen ahead (negative: delayed).
--- @field ghost? string Voxels: characters drawn see-through in X-ray.
--- @field edge? ShroudForgeOverlayColor Voxels: outline of each face (when cells are 3 pixels or more).
--- @field grid? number Voxels: lines on the floor of the box every this many cells. Default 0 (none).
--- @field grid_color? ShroudForgeOverlayColor Voxels: floor lines.

--- A zone drawn over the game image. Sizes and offsets are reference pixels of a
--- screen 1080 pixels high (or `reference`), scaled by the real screen height.
--- @class ShroudForgeOverlayZone
--- @field anchor? ShroudForgeOverlayPoint Screen point the zone is attached to. Default: `top-left`.
--- @field pivot? ShroudForgeOverlayPoint Point of the zone placed on the anchor. Default: same as `anchor`.
--- @field offset? number[] `{x, y}` added to that position.
--- @field size? number[] `{width, height}`; 0 fits the content. A width wraps text.
--- @field reference? number Screen height the values are designed for. Default 1080.
--- @field order? integer Drawing order, lower first.
--- @field visible? boolean
--- @field background? ShroudForgeOverlayColor
--- @field border? ShroudForgeOverlayColor
--- @field border_width? number
--- @field rounding? number
--- @field padding? number|number[]
--- @field spacing? number Space between items.
--- @field frame? {image: string, slice?: number|number[], scale?: number, tint?: ShroudForgeOverlayColor} Nine-slice image behind the content.
--- @field text? ShroudForgeOverlayTextStyle Default text style of the zone.
--- @field items? (ShroudForgeOverlayItem|string)[] Content, top to bottom.

--- @class ShroudForgeOverlayStatus
--- @field available boolean The overlay layer is loaded in the game.
--- @field active boolean The game image is being drawn on.
--- @field width? integer Game image size in pixels.
--- @field height? integer
--- @field scale? number Screen pixels per reference pixel (1080 reference, with the player's size option).
--- @field menu? boolean A game menu is open (the game shows the mouse cursor).
--- @field frame_ms? number The game's average frame time in milliseconds over about the last second (0 before; when no frame for over a second, the time since the last one). With the `frame_time` feature.
--- @field features? string[] What the layer knows beyond its first version (`dashes`, `same_mesh`, `blobs`, `frame_time`); see modules/game-overlay/README.md.

--- Zones drawn over the game image. Each mod only sees its own zones; they are
--- removed when the mod stops. The player can hide the overlay.
--- @class ShroudForgeOverlay
shroudforge_overlay = {}

--- Creates or replaces a zone of this mod.
--- @param id string Zone identifier (letters, digits, `.`, `-`, `_`).
--- @param zone ShroudForgeOverlayZone
--- @return true|nil ok
--- @return string? reason Why the zone was refused, or that the overlay is not loaded.
function shroudforge_overlay.set(id, zone) end

--- Removes a zone of this mod.
--- @param id string
function shroudforge_overlay.remove(id) end

--- Removes every zone of this mod.
function shroudforge_overlay.clear() end

--- @return ShroudForgeOverlayStatus
function shroudforge_overlay.status() end

--- @class ShroudForge
--- @field version string Version of the ShroudForge Lua API.
--- @field mod_id string Stable ID of the currently running mod.
--- @field mod_kind 'lua' Runtime package kind for the currently running mod.
--- @field settings ShroudForgeSettings Read the current values from the mod's settings.
--- @field ui ShroudForgeUi Register Modloader button callbacks and request the UI toggle.
--- @field input ShroudForgeInput Read declared keyboard and mouse shortcuts using SDL.
--- @field notifications ShroudForgeNotifications Publish notices in the local Modloader feed.
--- @field i18n ShroudForgeI18n Read mod-authored text from the locale catalogs.
--- @field overlay ShroudForgeOverlay Draw zones over the game image.
shroudforge = {}

--- Language the player chose for ShroudForge in the Modloader, such as `"en"`,
--- `"de"`, `"fr"` or a regional variant such as `"de-AT"`; `"en"` when the
--- setting cannot be read. Reads the settings file: call it when settings
--- change, not at every update.
--- @return string
function shroudforge.locale() end
