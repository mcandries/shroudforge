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

--- @class ShroudForgeInput
shroudforge_input = {}

--- Returns true while F1 through F8 is held and the game window is focused.
--- Poll from a runtime `on_update` callback and detect rising edges.
--- @param key 'F1'|'F2'|'F3'|'F4'|'F5'|'F6'|'F7'|'F8'
--- @return boolean
function shroudforge_input.is_key_down(key) end

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
--- @field type? 'text'|'image'|'row'|'flow'|'chip'|'icon'|'toggle'|'grow'|'spacer'|'separator'|'progress' Default: `text`.
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
--- @field color? ShroudForgeOverlayColor Separator, bar or icon color; toggle track when off.
--- @field background? ShroudForgeOverlayColor Bar or chip background.
--- @field thickness? number Separator thickness.
--- @field value? number Bar fill, 0 to 1.
--- @field box? number[] Chip `{min width, height}`. Default `{17, 17}`.
--- @field padding? number Chip horizontal padding. Default 4.
--- @field path? string Icon: SVG path data (M L H V A Z), stroked.
--- @field viewbox? number Icon box side. Default 24.
--- @field stroke? number Icon line width, in viewbox units. Default 1.8.
--- @field on? boolean Toggle state.
--- @field on_color? ShroudForgeOverlayColor Toggle track when on.
--- @field knob? ShroudForgeOverlayColor Toggle knob.

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
--- @field ui ShroudForgeUi Register callbacks for Modloader buttons.
--- @field input ShroudForgeInput Read supported function-key states.
--- @field notifications ShroudForgeNotifications Publish notices in the local Modloader feed.
--- @field overlay ShroudForgeOverlay Draw zones over the game image.
shroudforge = {}

--- Language the player chose for ShroudForge in the Modloader, such as `"en"`,
--- `"de"` or `"fr"`; `"en"` when unknown. Reads the settings file: call it
--- when settings change, not at every update.
--- @return string
function shroudforge.locale() end
