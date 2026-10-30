# Changelog

All notable changes to Voxel Rethinked. Each entry matches exactly one commit.

## [#097] 2026-10-30, G5.4 Save worlds
### Added
- Worlds save: edited chunks go to region files, the player to player.dat, on quitting and every Server.AutosaveMinutes. Only edited chunks are saved, and an edit is safe in memory from its own frame, even if the chunk unloads before the next save (game/src/server/world_saver).
- Each world has its own far terrain cache and uses its own seed, so seeds now change the terrain. World type and height still wait for the real generator.
- setblock and getblock console commands.
### Changed
- Server.Save.* settings show at the menu.

## [#096] 2026-10-30, G5.1 G5.2 World list and create world
### Added
- World list behind Singleplayer: worlds sorted by last played, with name, date, type and size, a search box, Play (also a double click), Create New World, Edit, and Delete with a confirmation.
- Create World with tabs: name, seed (numbers used as they are, text hashed, empty or Random picks one), world type, height presets or custom heights, structures, far terrain pre-map and quality, worldgen preset.
- Edit World: rename a world and read what it was made with.
- world.toml for every world (game/src/server/world_store), written atomically. Disk work runs on IO threads, so menus never wait. Until world generation exists, the seed and type are stored and the demo terrain loads.
- A uiclick console command for scripted runs through real screens, UiContext::topWidget, and tests for seeds, heights, world.toml, the list order, folder names and deleting.
### Changed
- Singleplayer opens the world list. The play console command still enters the demo terrain directly.

## [#095] 2026-10-30, G2.5 G2.6 Block tints, overlays and the No shading option
### Added
- Block textures can be tinted and can have an overlay drawn over them, set per texture layer (voxel::textureLook), so a block tints its top but not its underside. Grass, oak leaves and water now take their color from the block files: tint_color, tint_faces and [overlays]. Biome files replace these colors later (G3.7).
- Graphics.Lighting.NoShading: "Removes block shading and makes the game more vibrant". It changes chunks and far terrain together.
- BlockCatalog::shownColors, so far terrain shows tinted colors.
### Changed
- Default pack textures this needs: grey grass top and water, and the oak leaves overlay.
### Fixed
- Far terrain could show wrong colors after a block or texture was added: cached tiles store material numbers, and the cache key now covers them.
### Performance
- GPU 0.58 ms with tints against 0.63 ms before on the same view, so no measurable cost.

## [#094] 2026-10-30, One command build and building guide
### Added
- build.cmd: builds, tests, runs, repairs or packages any preset from any terminal, with the newest Visual Studio.
- docs/building.md: how to build, run, debug and share the game, CLion setup and troubleshooting.
- cmake/VcpkgToolchain.cmake: the presets now load the vcpkg of the user's VCPKG_ROOT, even inside Visual Studio's environment, which replaces that variable with a private vcpkg.
### Changed
- Configuring without a preset stops at once with instructions, instead of a compiler or package error.
- A build folder remembers its MSVC tools, and configuring it with another Visual Studio version stops before anything changes the libraries.
### Docs
- The README's build section points to the guide, and its game controls match the main menu and mouse look.

## [#093] 2026-10-30, G4.2 Settings screen presets, search and page reset
### Added
- Quality presets in the settings screen: a row at the top of a group or category that has presets, showing Custom when the values match none. Overall presets Low to Ultra in the group Graphics (High is the defaults, G9.1 tunes them).
- A search box above the settings; picking a group ends the search.
- Reset page with a confirmation, for the settings on screen (Section 19.3's Reset category).
- CycleButton::showOffList for text outside the choices.
### Changed
- StreamingSettings and LodSettings can be registered without their managers, and the game does so at startup: with no world behind the menu, the settings screen still shows every setting.
- The settings console command also works at the menu and takes a search text.
- The settings screen's button for the keybind screen is now called Keybinds; it read the same as the Controls group.
### Fixed
- A slider's value no longer disappears under its knob.
- A streaming test failed about half the time in full AddressSanitizer runs: slow frames hit the main thread budget before the sink's budget, which is what the test measures. It now sets a generous main thread budget (6 of 6 full runs pass).
### Docs
- Correction for #092: that commit also carried hand edited default textures (dirt, gravel, stone, grass top, grass side and the grass side overlay), which its entry did not list.

## [#092] 2026-10-30, G4.1b Menu panorama image
### Added
- The menu background is a baked 360 degree image from the texture pack (ui/panorama.jpg, packs can replace it) that turns slowly: UiRenderer::setPanorama and drawPanorama draw it as one window filling quad whose pixels look up their view ray, in true perspective and in the UI's one draw call. Without an image the menu shows the sky.
- tools/panorama/bake_panorama.ps1: bakes the image from the game (six views stitched into one equirectangular image), to run again when real world generation exists. Debug.FreeCamera.Frozen keeps real input from moving the camera meanwhile.
- A small Loading world screen while a world loads (the full one is G5.3).
- JPEG images in packs.
- Tests: the panorama quad, JPEG pack images.
### Changed
- No world is loaded at the menu: the world starts on Singleplayer and unloads on Quit to title.
- Engine shaders, game assets and texture packs are copied next to the program on every build, not only when it relinks, so a changed texture always arrives.
### Performance
- Engine start to the first menu frame 371 ms; Singleplayer to a playable world 478 ms (docs/perf/ui.md).

## [#091] 2026-10-30, Free camera follows the mouse
### Changed
- In the game the mouse always turns the camera, with no button held (FreeCameraDesc::alwaysLook), so the right mouse button is free for placing blocks. The console gets the cursor back while open. The developer demo keeps right button look.
### Added
- Test: with always look the mouse steers without a button and no mouse button is bound.

## [#090] 2026-10-30, G4.1 Main menu
### Added
- The game opens on a main menu (Section 19.3): the title, Singleplayer, Multiplayer (disabled, "Coming later"), Settings, Resource Packs (disabled until G4.4) and Quit, with the version in a corner.
- The panorama: the demo world with its far terrain behind the menu, turning slowly. It streams on jobs, so the menu does not wait for it, and playing it needs no second load.
- Singleplayer enters the world directly until the world list exists (G5.1). The pause menu's Quit to title returns to the menu.
- Label::setTextScale for big titles. Console commands play and title for scripted runs.
- Tests: label text scale, launch to the menu and back to the title in the state machine.
### Changed
- Scripted runs that need the world must run the play command first.
### Performance
- Engine start to the first menu frame: 347 ms; the world behind it is ready after 1.8 s (docs/perf/ui.md).

## [#089] 2026-10-30, E12.7 Keybind widget and rebinding flow
### Added
- ui::KeybindField: one binding slot; click or Enter listens for the next key or mouse button, Escape cancels, Backspace clears, and leaving the screen stops listening.
- ui::KeybindsView (Section 17.3): every action by category with two binding slots, a warning with the other actions' names when a binding clashes, Reset per action, per group and for all (with a confirmation), and a search box by action, category or key. A key another action uses asks Swap, Unbind other, Keep both or Cancel; Cancel starts focused and Escape means Cancel.
- input::bindingLabel: a binding as the player reads it, with keys named by the current keyboard layout; InputSystem::isMouseButtonSuppressed.
- Game: a controls screen from the Settings screen's Controls button; console command controls. MenuServices passes the settings, input and UI context between menu screens. uidialog now shows the widest dialog.
- Tests: capture, cancel and clear, every conflict answer, no conflict across contexts, the warning, every reset, search keeping the focus, leaving during a capture, and widgets surviving tree changes.
### Fixed
- UiContext no longer forgets the hovered and focused widgets on every tree change: it checks which still exist, so rebuilding a list keeps the focus in the search box above it, and widgets hidden or covered by a dialog are told they lost hover and focus instead of staying highlighted.
- A mouse button that just became a binding no longer also clicks the UI under it.

## [#088] 2026-10-30, E12.6 Settings UI generated from the registry
### Added
- ui::SettingsView: the settings UI built from the SettingsRegistry (Section 16.1): top level groups in a list (Video, Graphics, World, ... in a fixed order), the chosen group's settings in a scroll view under sub category headers; each row has the name (ending in "..." when long, the whole name and description in its tooltip), an (i) badge with the description, a Reload or Restart tag for settings that do not apply live, the control for its type and a Reset button enabled while the value differs from the default. Changes are written at once (text on Enter or focus loss); refresh() follows changes made elsewhere; a filter searches names, descriptions and ids across groups; developer settings show only on request.
- Controls: ui::Toggle (switch), ui::Slider (drag, click, arrows, snapping, value written on it), ui::CycleButton (choices with click, Enter and arrows), ui::InfoBadge (an "i" with a tooltip) and ui::ScrollView (clipping, mouse wheel, scroll bar, keeps the keyboard focus in view).
- Widget hooks: drag, wheel, Left and Right adjusting a focused control, reveal for scroll views, and replaceable child measure, arrange and draw; PixelFont::ellipsize; Button::setSelected; theme color accent; UiInput pointer wheel.
- Game: the pause menu's Settings opens a settings screen generated from every registered setting, with Done (developer settings in debug builds); console command settings [group].
- Tests: toggles, sliders, cycle buttons, ellipsis, scroll view clipping, wheel and focus, and the settings view's groups, rows, writes, refresh, reset and search.
### Changed
- Button::setEnabled only redraws when the state changes.
### Performance
- SettingsView::refresh with 60 settings: 0.0048 ms per frame (docs/perf/ui.md).

## [#087] 2026-10-30, E12.5 Screen manager and screen stack
### Added
- ui::Screen and ui::ScreenManager (Section 19.2): screens build their widgets once into a full window root and keep them while covered; push, pop, replace and clear wait for the next update, so a button may replace its own screen; the top screen takes the pointer and keyboard, screens below are hidden or, under a screen with drawBelow, drawn but inert; Back pops unless the screen handles it or is the last one; options for a backdrop and the mouse cursor; exactly one onEnter per onLeave. Stat Ui.Screens.
- Widget layout direction Stack (children overlap and fill the parent), Widget::removeChild and Widget::setInteractive (inert widgets draw but ignore the pointer, focus and tooltips).
- Game: the pause menu is a screen (Back to game, Settings disabled until G4.2 with a tooltip, Quit game); Escape on it returns to the game. The widget sample is a screen too. Console command pause.
- Tests: the stack, hidden and inert screens below, drawBelow with a backdrop, Back handled, popped and kept for the last screen, a button replacing its own screen, clear, and enter and leave order.
### Changed
- The game no longer reads Back itself while paused; the pause screen handles it.

## [#086] 2026-10-30, E12.4 Focus, keyboard navigation, text fields, dialogs and tooltips
### Added
- Keyboard focus in ui::UiContext: Tab and Shift+Tab walk the focusable widgets in tree order, the arrows move to the nearest one in that direction, Enter activates, Escape closes the top dialog or is handed to the screen (takeBack). Hover moves the focus (a text field keeps it until something else is clicked); clicks focus what they hit.
- ui::UiActions: the rebindable menu actions ui.next, ui.previous, ui.up, ui.down, ui.left, ui.right, ui.activate and ui.back in the caller's menu context, plus the fixed text editing keys with the system key repeat (InputSystem::wasKeyTyped); ui::UiInput carries pointer, navigation, text and frame time.
- ui::TextField: UTF-8 text with a caret between code points, typing, Backspace, Delete, Left, Right, Home, End, click to place the caret, scrolling, blinking caret, placeholder, length limit, character filter, change and submit handlers; UiContext::wantsTextInput turns on the window's text input.
- Layers above the screen (UiContext::pushLayer, closeLayer): modal layers dim the screen with the theme backdrop and take the pointer and focus; focus returns when they close. ui::showDialog builds a dialog with a title, a wrapped message and buttons; the cancel button starts focused and answers Escape.
- Tooltips (Widget::setTooltip): after the pointer rests for 0.5 s, also on disabled buttons and on widgets focused with the keyboard, wrapped and kept inside the window.
- Theme colors field, field_border, tooltip, tooltip_border and metric tooltip_max_width; the default theme lists every value including [slices].
- Game: menu navigation and text input reach the UI; the widget sample has a text field, a disabled Multiplayer button with a tooltip and a delete confirmation dialog; console command uidialog opens a sample dialog.
- Tests: Tab order, wrap around, spatial arrows, Enter, hover and click focus, text field editing with multi byte text, limits, filters and caret clicks, dialogs with Back, Enter and focus return, and tooltips.
### Changed
- The game's menu.back action is now the engine's ui.back (Escape, menu context).

## [#085] 2026-10-30, E12.3 UI batching renderer
### Added
- ui::QuadBatch: rectangles, atlas images, nine slice frames and text in window pixels, clipped on the CPU (texture coordinates cut to match) with a clip stack, so scroll views need no scissor changes and the UI stays one draw call.
- The UI atlas: the font sheet and the packs' ui/*.png images packed into one texture (shelf packer, ui::packAtlas); UiRenderer::setResources builds it, stat Ui.AtlasBytes.
- Nine slice looks for panels and buttons from the packs: ui/panel.png, ui/button.png, button_hover.png, button_pressed.png and button_disabled.png (ui::kThemeImages), corner sizes in the theme's new [slices] table. Without an image a widget keeps the theme's flat colors, so packs can restyle widgets one at a time.
- Default UI images in textures/default/ui (16 x 16, 3 texel corners, matching the flat look: dark frame, one pixel bevel, gold hover border, rounded corners); Alex can edit them in Aseprite.
- UiContext keeps the tree's quads and rebuilds them only when something changed (layout, hover, press, text, theme, scale or atlas); Widget::invalidateVisual for look only changes; counter Ui.Rebuilds and rebuildCount().
- Tests: atlas packing, clipping of rectangles and texture coordinates, nested clips, nine slice corners and small targets, theme slices, kept quads across frames and rebuilds on hover, text and new resources.
### Changed
- DrawContext draws into a QuadBatch instead of the renderer; UiRenderer draws its frame batch (frame(), append()).
### Performance
- 162 widget menu: a still screen costs 0.0022 ms per frame (kept quads), a rebuild 0.050 ms on changes only (docs/perf/ui.md).

## [#084] 2026-10-30, E12.2 Widget tree, layout, styling and theme files
### Added
- ui::Widget: the retained widget tree with a flexbox subset (row or column, justify, align and alignSelf, gap, padding, grow, shrink, fixed sizes, min and max), laid out in UI pixels and snapped to whole pixels; changes flag the root, so layout runs only when the tree, window, scale or font changed.
- Widgets: Panel (theme background, border, padding, gap; blocks the pointer), Label (roles Normal, Muted, Title, alignment, word wrap to the given width) and Button (theme height and minimum width, hover, pressed and disabled looks with a one pixel bevel, click on press and release over it).
- ui::Theme from ui/theme.toml in the texture packs ([colors] and [metrics], every value optional, bad entries warn and keep the default); the default pack's textures/default/ui/theme.toml states the built in look.
- ui::UiContext: owns the tree and theme, picks the whole number UI scale (Interface.UiScale, 0 Auto: the largest that fits 320 x 240 UI pixels), routes the pointer (hover, press, click, safe when a click removes widgets) and draws through ui::UiRenderer. Stats Ui.Widgets, Ui.Scale, Ui.LayoutMs and Ui.Layouts.
- TexturePackManager::setFiles and file(): pack files besides images (ui/theme.toml), reloaded with the packs.
- Game: the theme comes from the pack stack and hot reloads; Debug.Ui.WidgetSample shows a sample screen with every widget; the mouse goes to the UI while the game does not capture it.
- Benchmark: layout and drawing of a 162 widget menu (benchmarks/ui_bench.cpp); vxe_benchmarks takes a group name to run one group.
- Tests: columns, rows, grow, shrink, justify, align, limits, hidden widgets, pixel snapping, wrapping labels, layout requests, theme parsing, the default theme, the automatic scale, and pointer hover, press, click, drag off, disabled buttons, captured mouse and a click that removes its own tree.
### Performance
- 162 widget menu: layout 0.025 ms (only on changes), drawing into quads 0.032 ms per frame (docs/perf/ui.md).
### Changed
- Interface.UiScale moved from the game into the engine UI and is now a whole number from 0 (Auto) to 8, so pixel art stays sharp (Section 19.1 allowed fractional scales). An older settings.toml with a fractional value logs one warning and uses Auto.
- ui::rgba moved to vxe/ui/color.hpp.

## [#083] 2026-10-30, E12.1 Pixel font pipeline
### Added
- An original pixel font (textures/default/ui/font.png): 128 x 128 sheet of 16 x 16 cells, every printable ASCII character, proportional widths, white on transparent so text takes any color. Packs replace it with their own ui/font.png.
- ui module (layer 10): ui::PixelFont builds a font from a sheet (glyph widths measured from the opaque pixels, missing characters draw a box, higher resolution sheets keep the text size), measures, lays out and word wraps UTF-8 text.
- ui::UiRenderer: rectangles and text with a drop shadow, batched into one draw call per frame from a storage buffer of quads (engine/shaders/ui/quad.vert and quad.frag), nearest sampling and whole pixel positions keep text crisp at every scale. Stats Ui.Quads and Ui.DrawCalls.
- Renderer: a Ui pass between Post and WindowOverlay (Debug.Passes.Ui), Renderer::setUi and Renderer::shaders().
- Texture packs: render::readPackImage and TexturePackManager::setImages and image() load images outside the block array (ui/font.png) with the textures, including hot reload; the validator checks ui/font.png.
- Game: loads the font from the pack stack, draws the UI each frame; Debug.Ui.FontSample shows the font at scales 1 to 4.
- Tests: UTF-8 decoding, glyph widths, measuring, layout, wrapping, sheet rules, the default font's characters, the UI renderer on the Null device, pack images and their fallback.

## [#082] 2026-10-30, G2.4 Pack validator
### Added
- render::validatePack: checks a pack folder the way the loader reads it (pack.toml, every blocks/*.png readable, a power of two wide, square or a strip of square frames, at the declared resolution; every blocks/*.anim.toml valid, with a texture and frametime 1 to 255) and against the default pack's keys (missing textures fall back, unused ones are reported).
- tools/pack_validator (vxe_pack_validator <pack folder> [--reference <pack folder>]): prints errors and warnings with their files; exit code 1 on errors. A ctest runs it on the default pack.
- Tests: the validator reports each kind of problem with its file; a broken image in a higher pack falls back to the next pack.
### Changed
- A broken or unusable image in a higher pack now falls back to the next pack that has a usable one (with a warning) instead of the magenta checker, which is left for keys no pack can provide.

## [#081] 2026-10-30, G2.3 Texture hot reload and LOD colors
### Added
- Hot reload of texture packs (Section 15.3): with Graphics.Textures.HotReload on (default in debug builds), saving any file in the packs folder loads the packs again on a job and swaps them in live; the far terrain color table follows every load (from G2.2).
- Graphics.Textures.Folder: points the game at another packs folder, for example the source tree's textures/ while editing the default pack (set Graphics.Textures.Folder C:/Code/C++/VoxelRethinked3D/textures).
- TexturePackManager takes the engine's file watcher; hotReloads() counts reloads.
- Test: saving a texture reloads it live, another folder can be chosen, and with hot reload off nothing reloads.

## [#080] 2026-10-30, Default pack hand edits
### Changed
- textures/default: stone, dirt, gravel, grass side, coal ore and gold ore refined by hand in Aseprite.

## [#079] 2026-10-29, G2.2 Texture pack loader
### Added
- render/texture_pack: pack manifests (pack.toml: name, format, resolution 16 to 256, options), pack sources from folders and from memory, pack discovery, and buildBlockTextures: each texture key from the first pack of the stack that has blocks/<key>.png, decoded with stb_image, flipped bottom row first (ADR-016), scaled with nearest filtering to the largest resolution of the stack (capped by Graphics.Textures.MaxResolution), mipmaps averaged by alpha with alpha coverage kept for cut out textures (leaves do not thin out with distance), animations from vertical frame strips plus blocks/<key>.anim.toml (frames after the key layers, so layer numbers never change), a magenta checker and a warning for anything missing or broken, and each layer's average color.
- render::TexturePackManager: builds the array on a job and swaps it in on the main thread; settings Graphics.Textures.Packs (enabled packs, highest priority first; "default" always last, then the built in pack), MaxResolution and Anisotropy, all live; stats Render.Textures.Layers, Resolution, Bytes, Warnings and LoadMs.
- Animated block textures in the chunk shader: a per layer animation table (ChunkRenderer::setTextureAnimations), frames at 20 game ticks per second.
- The game loads its block textures from the packs: the default pack is copied next to the program (textures/) and also built into it (cmake/EmbedFiles.cmake) as the last fallback; the far terrain colors come from the loaded textures.
- Tests: manifests, stack precedence, missing and broken images as warnings, mixed resolutions and the resolution limit, row order, animation frames, mip coverage of cut out textures, built in packs, and live pack switching through the manager.
### Removed
- The demo world's placeholder colors and textures.
### Performance
- Loading the 17 default textures takes 122 ms on a worker; the main thread only uploads.

## [#078] 2026-10-29, G2.1 Default texture pack, generated
### Added
- tools/texture_gen (vxe_texture_gen): paints a texture pack from recipes. Each recipe names a pattern (speckle, stone, grass_top, overhang, gravel, water, bark, rings, leaves, glass, glow, ore), a dark to light palette, an accent palette and an optional base texture painted first. Textures are deterministic and tile seamlessly. Existing PNGs are kept unless --force (hand edits survive), --only limits a run to some keys, --preview writes an enlarged sheet of every texture, --template copies the pack as it is into a new folder named "My Pack" with a list of its keys.
- tools/texture_gen/default_pack.toml: recipes for the 17 base textures at 16 x 16 (stone, dirt, grass top and side, sand, gravel, water, log side and top, leaves, glass, lumen block, snow, coal, iron and gold ore, foundation stone).
- textures/default: the generated default pack (one PNG per texture, pack.toml, pack.png icon), original art meant to be refined by hand in Aseprite.
- vcpkg dependency stb (PNG writing now, image loading for the pack loader in G2.2).
- Tests: every pattern paints at 16 and 32 pixels the same way each time and differently per key; leaves are cutout, glass see through with a solid frame; bad recipes report the problem; textures tile across the edge.
### Docs
- CLAUDE.md: the agreed order after E11 (G2 texture packs, then E12) and the 16 x 16 default resolution.

## [#077] 2026-10-29, E11.6 LOD debug views and overlay page
### Added
- Overlay page "LOD" (Section 18.2 page 6): reach, tiles per level, queued and running tiles, sampling and cache rates, premap progress, drawn triangles, tile and GPU memory, and a preview of the coverage map.
- Debug views: Debug.LOD.TileBorders (every tile's box colored by level), Debug.LOD.ShowQueue (tiles waiting and in work), Debug.LOD.Freeze (far terrain stays put while the camera moves); Debug.LOD.ColorLevels and ShowCoverage from E11.3.
- LOD quality presets in group Graphics.LOD: Low, Medium, High and Ultra (Ultra: 8 levels, 49,152 to 65,536 blocks).
- Console command preset <group> [name].
- Stats Voxel.Lod.ReachBlocks, Voxel.Lod.Level<N>.Wanted and .Ready, Render.Lod.DrawnQuads; LodRenderer::coverageTexture and DebugOverlay::setLodCoverageTexture.
- Tests: presets set the reach and Ultra passes 32,000 blocks; freezing keeps the tiles; the preset command lists and applies presets.
### Fixed
- Command line startup commands (--exec) that name settings or presets of systems starting later (a world) failed without notice; they now run again each frame for up to 10 seconds and log when they apply or finally fail.
### Performance
- Phase E11 acceptance at render distance 32: High 1506 FPS (1% low 1084), Ultra 49,152 block reach at 1331 FPS (1% low 671).
### Docs
- docs/perf/lod.md: E11.6 and the phase acceptance, and a correction of an E11.4 measurement that ran with a setting that never applied.

## [#076] 2026-10-29, E11.5b LOD visual parity
### Added
- Far terrain in the demo carries the same trees as the chunks: DemoTerrain::treeSpot and treeGrows are shared with DemoLodSampler, which places every crown block by block in levels 1 and 2 (crown spans floating over or joined to the ground) and, in coarser levels, tints grass towards leaves by the share of the cell under crowns (in eighths), raising dense forest to the crowns' height.
- LOD only materials in the demo: water as it looks over its dim floor and seven grass and leaf blends; DemoLodSampler::materialColors builds the table from the textures' average colors.
- BlockTextures::averageColors: each placeholder layer's 1 x 1 mip color.
- Shader include include/sky.glsl (sky colors, height fog, fog color) shared by the sky pass and translucent chunks; SkyRenderer::bindParams.
- Test: the demo far terrain has a crown over a tree's trunk at the crown's height and leaf tinted grass in a coarse level; the color table has every layer plus the eight LOD only materials.
### Changed
- Translucent chunks (water, glass) fog themselves with the sky pass's formula; they are drawn after that pass, so distant real water stayed clear next to fogged far water.
- The LOD shader decodes material colors with the exact sRGB curve, as the GPU does for block textures.
- The demo's far terrain colors come from the textures' average colors instead of the base colors.
### Performance
- Demo world playable 0.56 to 0.58 s warm and 1.06 s cold with tree sampling (targets 1.0 s and 2.0 s).
### Docs
- CLAUDE.md: Section 13.4 asks the column sampler for features in LOD and seamless colors; G6.1 lists the agreed camera wishes (mouse look without a button, FOV slider, zoom on C with the scroll wheel).
- docs/perf/lod.md: E11.5b.

## [#075] 2026-10-29, E11.5 Edit downsampling and dirty propagation
### Added
- lod_downsample: chunk columns into level 1 cells (runs of visible blocks over each 2 x 2 block cell, materials from the run's top block), merging new spans into a height range of old ones, four cells into one of the next level, a tile into its parent's quadrant, and limiting spans per cell by joining across the smallest gaps.
- LodManager::columnEdited: copies an edited chunk column's loaded chunks and, on an IO job, puts the edit into the level 1 tile and every level above up to the clipmap's coarsest, starting from the shown tile, the cache or the sampler; rebuilt tiles go to the cache and replace shown ones. Edits run one at a time; pendingEdits() and isSettled() include them. Stats Voxel.Lod.Edits and Voxel.Lod.EditTime.
- Tests: span limiting, column downsampling with overhangs, replacing a height range, four cells into one and a tile into its parent; an edited column reaches all six levels in the cache and on screen while the ground next to it stays.
### Performance
- One column edit through six cached levels: 11 ms on an IO thread.
### Docs
- docs/perf/lod.md: E11.5.

## [#074] 2026-10-29, E11.4 Premapping job and progress API
### Added
- LodPremapper: bakes the clipmap tiles within a radius around a spawn into the LOD cache (an IO job checks the cache, a worker samples, an IO job writes), nearest first, cancellable, with progress() (total, done, already cached, failed) for a loading screen; stats Voxel.Lod.Premap.Done, Skipped and Total.
- lodCacheHash, shared by the LodManager and the premapper; LodManager::clipmapDesc and sampleOptions are public, so a premap bakes exactly what the manager asks for.
- LodTileCache::contains and lodTileFileMatches: check a cached tile by its header only.
- The demo world caches far terrain in saves:/demo/lod, premaps the spawn area while loading, shows the premap in its progress text, and enters only once every far terrain tile is shown.
- Tests: a premap bakes the tiles within the radius nearest first and a second one finds them all cached; a cancelled premap stops with the near tiles baked; a LodManager on a premapped area loads every tile from the cache and samples nothing.
### Changed
- LodRenderSink meshes tiles at Normal priority (was Low, which the chunk burst held back).
### Performance
- Demo world playable 0.85 s after a cold start with all 304 tiles premapped, 0.51 s warm; far terrain ready at 0.31 to 0.37 s warm.
### Docs
- docs/perf/lod.md: E11.4.

## [#073] 2026-10-29, E11.3 LOD mesher and renderer
### Added
- LodMesher: tile spans to 12 byte quads (LodQuad): tops of the highest spans merged greedily, tops of lower spans and undersides of spans with a span below, the exposed parts of span sides merged along each edge, skirts at tile edges (4 cells deep, at least 16 blocks).
- render::LodRenderer: a vertex arena with budgeted staging uploads, CPU frustum culling per tile, per frame tile positions relative to the camera chunk, multi draw indirect, a material color table, a 128 x 128 wrapping coverage map that discards far terrain over chunk columns real chunks draw, and tiles skipped whole when every column under them is covered. Shaders render/lod.vert and render/lod.frag. Passes LodUpload and Lod in the renderer's frame.
- voxel::LodRenderSink (meshes tiles on workers, uploads them, keeps the coverage map from meshed chunks) and voxel::CoveringChunkSink (passes chunk meshes on and reports them for coverage).
- Settings Graphics.LOD.MaxUploadKBPerFrame, Graphics.LOD.CoverageDiscard, Debug.LOD.ColorLevels and Debug.LOD.ShowCoverage. Stats Render.Lod.Tiles, Quads, ArenaBytes, DrawnTiles, CoveredTiles, UploadBytes, UploadsOverBudget and UploadsOutOfMemory.
- The demo world shows far terrain: DemoLodSampler samples the demo terrain's height per cell, colored by the blocks on top, through LodManager, LodRenderSink and the renderer.
- Game console command camera <x> <y> <z> [yaw pitch] for repeatable captures.
- Tests: LOD quads cover every top, underside and exposed side exactly once against a block by block count; flat tiles are one top and four skirts; the renderer uploads, culls, splits big tiles into draws, keeps its budget and wraps the coverage map; the sink gets every tile of the clipmap to the renderer; coverage follows meshed chunks from the lowest surface up.
### Changed
- ChunkRenderer::brightness() so far and near terrain are lit alike.
### Performance
- Lod pass 0.21 ms GPU in the demo world with 304 tiles to 16,384 blocks; frame rate within run to run noise of LOD off (1169 and 1079 FPS standing and flying, against 1010 and 997 without LOD).
### Docs
- docs/perf/lod.md: E11.3.

## [#072] 2026-10-29, E11.2 LOD sampler interface and manager
### Added
- LodTileSampler: the interface the game's column sampler implements (sampleTile on workers with spans per cell, caves and a cancel flag; worldHash for the cache).
- LodTileSink: receives tiles to draw (the LOD renderer, E11.3).
- LodManager: follows the clipmap, loads each wanted tile from the cache on an IO thread, samples it on a worker at low priority when the cache misses, saves sampled tiles back, and hands ready tiles to the sink. Nearest rings first, finer levels first within a ring. Tiles that leave the clipmap stay shown until the tiles taking over their ground are ready, so moving never opens holes.
- Settings Graphics.LOD.Enabled, Levels, HalfExtentTiles, MaxSpansPerCell, CavesInLod and MaxJobsInFlight, all live; spans and caves are part of the cache hash, so changing them resamples every tile while the old ones stay shown.
- Stats Voxel.Lod.Tiles, Queued, Running, Retiring, MemoryBytes, Sampled, FromCache and SampleTime.
- LodTileCache load and save with a given world hash; LodClipmapDesc equality.
- Tests: every clipmap tile sampled once and shown; the cache serves a second run and goes stale on option or generator changes; 300 camera moves never uncover shown ground; enabling, disabling, more levels and other sizes; cancelled jobs when tiles leave.
### Performance
- LodManager::update costs 29 ns per frame when settled; all 304 default tiles are ready 9.3 ms after the first update with a trivial sampler.
### Docs
- docs/perf/lod.md: E11.2.

## [#071] 2026-10-29, E11.1 LOD data structures
### Added
- LOD tiles (Section 13.2): LodTileKey (level, tile position, cell and tile size, quadtree parent and children), LodSpan (bottom and top Y, top and side material, sky light), LodTile with flat span storage and 16 bit cell offsets, LodTileBuilder for filling a tile cell by cell, setCell for edits, validLodSpans.
- LOD cache files (Section 13.4, 21): lodWorldHash from seed, generator version and preset hash; encodeLodTile and decodeLodTile (header with world hash and checksum, LZ4 payload of span counts and varint deltas); stale files report VersionMismatch, damaged ones ParseError, never reading out of bounds.
- LodTileCache: one file per tile at l<level>/t.<x>.<z>.vxl, atomic writes, misses for missing, stale and damaged files; stats Voxel.Lod.Cache.Hits, Misses, Stale, Damaged, BytesRead and BytesWritten.
- LodClipmap (Section 13.1): squares of tiles per level around the camera that move two tiles at a time, with a hole of whole tiles where the finer level lies; update() reports added and removed tiles.
- Tests: tile keys below zero and the quadtree, span rules, edits against building, cache file round trips at the int32 extremes, truncated and damaged files, the cache with stale and damaged files, clipmap coverage (every column within reach drawn by exactly one tile, neighbors at most one level apart) and update differences.
- benchmarks/lod_bench.cpp.
### Performance
- A hills tile: 76.5 KB in memory, 13.9 KB on disk; encode 0.23 ms, decode 0.18 ms, cache load 0.29 ms, cache save 1.84 ms. Clipmap update 14 ns when nothing changes, 14.6 us when the finest squares move.
### Docs
- docs/perf/lod.md.

## [#070] 2026-10-29, E10.4 Light in meshes
### Added
- The mesher puts the sky and block light of the open cell in front of each face into its quad (Section 10.4 word1 bits 22 to 29, until now a placeholder of full sky). Faces merge only where their light is the same, unless Graphics.Terrain.MergeAcrossLight is on (Section 10.2).
- The chunk shader lights quads: the brighter of sky light times the daylight and slightly warm block light, each level a fifth darker than the one above, never darker than Graphics.Lighting.Brightness (new setting, default 0.08). ChunkRenderer::setDaylight; the renderer sets it every frame from the sun's height (full by day, a quarter at night, 1 with the sky off).
- Tests: faces take the light of the cell in front and merge only where it is the same; the brute force mesher comparison checks every covered face's light, with light patches and merging across light; a torch's light reaches its chunk's uploaded mesh within a frame of placing and removing it.
### Fixed
- Test snapshots built by hand are lit like open sky, as unlit chunks are, so the GPU render tests see the same brightness as before.
- The upload budget streaming test held back every mesh until three waited, instead of relying on mesh jobs finishing together (it failed once under load).
### Performance
- A torch shows in its chunk's mesh 4.3 to 4.4 ms after it is placed and is gone 4.2 to 4.3 ms after it is removed. GPU frame time at the demo spawn unchanged (0.299 ms).
### Docs
- docs/perf/light.md: E10.4 and the phase E10 acceptance.

## [#069] 2026-10-29, E10.3 Sky light
### Added
- Sky light (Section 14.1 stage C, ADR-019): ChunkLighter (was BlockLighter) computes a chunk's sky and block light together from its 48^3 neighborhood. Full sky light enters every open column at the neighborhood's top, falls straight down through clear blocks without loss, is dimmed by blocks with light opacity, and spreads sideways and downward into overhangs and caves losing at least one level per block.
- Open sky from the column summaries: a column is open if no loaded block stands above the neighborhood, unless the loaded chunks of that chunk column do not reach the generator's surface estimate and the estimate is above the neighborhood (deep underground: dark).
- Streaming: chunks of one opaque block are lit dark and chunks of open air with no light source in reach fully sky lit without a job; all others run a light job. Bounded relight: an edit relights the 27 chunks around it and the chunks below down to Graphics.Lighting.SkyRelightDepth (default 4 chunks), as does a loaded chunk with blocks above lit chunks.
- ADR-019: light is computed per chunk from its neighborhood.
- Tests: sky and block light against brute force floods with random open sky; the streamed world against floods over the whole loaded world (sky and block); a roof shading the ground and the sky coming back when it is removed; a torch in a closed room. A light benchmark (benchmarks/light_bench.cpp).
### Changed
- Light jobs fill their 48^3 box row by row through a byte per state (opacity and glow), 485 to 228 us per chunk.
- Streaming tests wait for relighting before meshing where an edit is involved.
### Performance
- Demo world flight at 300 blocks per second: playable after 446 to 485 ms, streaming main thread time 0.14 to 0.18 ms per frame, 1% low 520 to 633 FPS; about 58% of chunks need a light job (surface, water, caves).
### Docs
- docs/perf/light.md: E10.3 measurements.

## [#068] 2026-10-29, E10.2 Block light
### Added
- voxel::BlockLighter (Section 14.1 stage B): the block light of a chunk from its 48 x 48 x 48 neighborhood (the chunk and its 26 neighbors; a light source reaches at most 15 blocks, so nothing farther matters). A flood fill from every source at once, level by level from 15 down, losing at least one level per block and more through blocks with a higher light opacity; fully opaque blocks stop light but still glow when they emit. Computing a chunk from scratch makes placing and removing a source the same operation and leaves no stale light behind.
- voxel::neighborhoodEmitsLight and ChunkStorage::anyState: whether any of the 27 chunks holds a light source, from palettes only.
- Lighting in the streaming manager: a Lighting stage between loading and meshing. Chunks are lit once their 26 neighbors are loaded and meshed once they and their 26 neighbors are lit, so a mesh never has to be redone because a neighbor's light arrived. Ranges grow by one ring: lit within render distance + 1, loaded within + 2. Chunks with no source in reach get block light 0 without a job; the others go to light jobs that copy their neighborhood, and results from blocks that changed meanwhile are thrown away and computed again. Block edits light the edited chunk and its 26 neighbors again.
- VoxelWorld: Chunk::blocksVersion (moves only with the chunk's own blocks), editedChunks (chunks whose blocks were edited, for relighting), replaceBlockLight (new block light, sky light kept).
- Settings Graphics.Lighting.BlockLight and World.Streaming.MaxLightJobsInFlight. Stats Voxel.Light.ChunksLit, Voxel.Light.Skipped, Voxel.Light.JobTime and Voxel.Streaming.Lighting; the overlay page "Streaming and Jobs" shows the Lighting stage.
- StreamingManager::isLit.
- Tests: the flood fill against a brute force relaxation on random neighborhoods; streamed chunks against a flood over the whole loaded world with sources on chunk borders and corners (no seams); placing and removing a torch (lit within 1.8 ms, dark within 1.5 ms, nothing left behind); turning block light off and on.
### Changed
- Streaming loads one more ring of chunks (render distance + 2) so lit chunks have all their neighbors; tests updated for the larger range.
### Fixed
- The frame limiter accuracy test measures up to three times: parallel test processes now and then delay one sleep by several milliseconds, which made it fail twice in full ctest runs.
### Performance
- Demo world flight at 300 blocks per second: streaming main thread time 0.08 to 0.10 ms per frame (0.06 to 0.08 before); playable after 314 to 343 ms (255 to 306 before).
### Docs
- docs/perf/light.md: E10.2 measurements.

## [#067] 2026-10-29, E10.1 Light storage
### Added
- voxel::ChunkLight (Section 14.2): sky and block light of a chunk's 4096 blocks, 4 bits each in one byte (packLight, skyLightOf, blockLightOf). Uniform chunks (open sky above the ground, dark deep below) keep their one value in the object; any other chunk stores 4096 bytes under MemoryTag::Chunks. Writes switch to full storage by themselves, compact() and assign() switch back when every block is lit alike. Chunks start as open sky, matching the mesher's placeholder light until the light engine (E10.2, E10.3) computes real light.
- Chunks carry light: Chunk::light, VoxelWorld::insertChunk with light, getLight, setLight (marks the chunk and the neighbors whose snapshot border holds the block, not for saving since light is derived) and replaceLight (marks only neighbors whose border light changed, so relighting a chunk does not remesh 26 others).
- Snapshots copy light with the blocks (ChunkSnapshot::light, lightAt); missing neighbors get a given fill light. When the chunk and all its neighbors are lit alike the whole box is one fill.
- Chunk payloads can carry light (flag bit 0 of the payload, as reserved in #066): one byte for uniform light, 4096 for full. Chunk sources can hand light to streaming (ChunkProduct::light), for chunks read from saves.
- Stats Voxel.World.FullLightChunks and Voxel.World.LightBytes.
- Tests: packing and mode switches with memory accounting, equality across modes, world reads and writes with exactly the right neighbors marked, snapshot light against the world block by block with missing and mixed neighbors, light stats, payloads with light and truncated light sections, light from a chunk source through streaming. A snapshot benchmark with full light everywhere.
### Performance
- Snapshots with uniform neighbors 2.61 to 3.02 us (target under 5 us), all air 1.11 to 1.65 us, mixed terrain unchanged at 5.83 us, full light everywhere 7.24 us.
### Docs
- docs/perf/world.md: E10.1 light memory and snapshot times.

## [#066] 2026-10-29, E9.8 Region file IO
### Added
- io::RandomAccessFile: reads and writes at explicit offsets (Windows ReadFile and WriteFile with offsets, POSIX pread and pwrite), sync (FlushFileBuffers, fsync), so several threads can read while one writes elsewhere in the file.
- voxel::encodeChunk and voxel::decodeChunk: the chunk payload of Section 21. Each payload lists the names of the states its chunk uses (in order of first use, so the same chunk always gives the same bytes), then 4096 bit packed palette indices. Names the game no longer knows load as a fallback state and are counted. Malformed payloads fail with an error, never read out of bounds.
- voxel::BlockStateNames, the interface through which the game names states; vr::CatalogStateNames answers it from the BlockCatalog.
- voxel::RegionFile (Section 21, ADR-018): 4096 chunks per file in 512 byte sectors. A commit writes compressed payloads and a chunk table into sectors the last commit does not use, syncs, then writes a checksummed superblock into the other of two slots and syncs again; opening takes the newest valid superblock. Payloads and the table carry XXH3 checksums. Readers on any thread run alongside a commit.
- voxel::RegionStore: the region files of a world (r.<x>.<y>.<z>.vxr), kept open up to Server.Save.MaxOpenRegionFiles, one commit per region for a batch of saved chunks, zstd level Server.Save.CompressionLevel (default 3). Stats Voxel.Regions.OpenFiles, ChunksRead, ChunksWritten, BytesRead, BytesWritten, Commits, CommitTime.
- Tests: offsets in files, payload round trips from 1 to 4096 states, unknown names and malformed payloads, region positions below zero, commits, replacing, removing and space reuse, a simulated crash at every byte of a commit, damaged superblocks and payloads, reads from four threads during commits, a writer process killed 12 times mid commit (vxe_region_crash_writer, started through SDL's process API), the region store across regions with an open file limit, and demo terrain chunks through the codec and region files by catalog name.
- Benchmarks: payload encode and decode, commit and read (benchmarks/region_bench.cpp).
### Performance
- 4 bit terrain chunk: encode 9.4 us, decode 10.4 us, read from a region file 5.7 us; a 64 chunk commit with both syncs 2.7 ms (43 us per chunk). Demo terrain takes about 0.8 KB per chunk on disk.
### Docs
- ADR-018: region files commit copy on write behind two superblocks; chunks carry their own name palettes (Section 9.3's per region palette becomes per chunk, compressed per file).
- docs/perf/regions.md: E9.8 measurements and the crash tests.

## [#065] 2026-10-29, E9.7 Streaming without renderer hitches
### Changed
- The chunk draw list changes in place instead of being rebuilt for every chunk change. It has a region per pass with room to grow; a chunk adds or removes only its own entries (removing moves the region's last entry into the gap), the translucent region is sorted far to near again only when the camera changes chunk or a chunk with translucent quads changes, and only changed ranges are copied to the GPU through a 1 MB per frame staging ring. The culling shader takes a start and a count per region. ChunkRenderer::drawFirst tells where a pass's draws start.
- The cave search runs on a job when the renderer has a job system (ChunkRenderer::setJobSystem, CaveCuller::setJobSystem; the engine loop sets it). The main thread copies the chunk list and keeps using the previous result; until the result for the camera's current chunk is in, every chunk counts as visible, so a stale result never hides anything.
- The Null device performs buffer copies, so tests can read back what staging copies put into a buffer.
### Fixed
- Settings registered after the settings file was loaded (world streaming and the mesh settings register when a world starts) now take their saved value. Before, their keys stayed "unknown" and were written a second time at save, and the next start rejected the whole file ("cannot redefine existing boolean").
### Added
- Tests: the draw list against a full rebuild through 300 rounds of random uploads, replacements and removals; the cave search on a job against the one on the calling thread, including the everything visible window; settings registered after loading.
### Performance
- Flight at 300 blocks per second, demo world, render distance 16, release: draw list work per chunk change 0.004 ms on average instead of a 0.55 ms rebuild (up to 3.5 ms); cave work on the main thread 0.008 ms instead of 0.51 ms (up to 1.8 ms); 1% low FPS 558 to 741 instead of 359 to 449; allocations per frame 1 instead of 14 to 54. Standing still unchanged (about 1970 FPS, 1% low 1183 to 1319).
- Streaming main thread time stays under 1 ms except for 0 to 5 updates in 20,000 (most 1.2 to 2.7 ms), each one a single snapshot or job schedule call stalled for 0.2 to 1.9 ms; a probe that only reads the clock sees the same stalls while the machine is busy, so they come from the system, not from streaming work.
### Docs
- docs/perf/streaming.md: renderer costs before and after, whole frame numbers and the main thread stall analysis.

## [#064] 2026-10-29, E9.7 Streaming manager
### Added
- voxel::StreamingManager (Section 11): decides which chunks exist around the camera and drives each through the pipeline: request, load or generate on workers, insert into the VoxelWorld, mesh on workers once all 26 neighbors are there, upload within the sink's budget, unload. Chunks load within render distance + 1 (so every meshed chunk has its border), are meshed within the render distance, and stay for World.Streaming.UnloadHysteresisChunks more before they go, so a meshed chunk never sees a neighbor arrive and is meshed once. Remeshing follows edits (the world's changed chunk list), mesh setting changes (Graphics.Terrain) and new block registry views.
- Priorities (Section 11.3): offsets sorted by distance with vertical distance counting double, scanned from the nearest when the camera enters another chunk; a heap of the nearest unrequested chunks ordered by World.Streaming.LoadOrder (Spiral, Distance, ViewFirst: chunks ahead of the view and the movement first). Mesh jobs run at a higher job priority than loads.
- Cancellation (Section 11.4): job slots with cancel flags; results of chunks that left the range and meshes of outdated snapshots are thrown away.
- Budgets: World.Streaming.MaxLoadJobsInFlight, MaxMeshJobsInFlight, MaxChunks, MainThreadBudgetMs (every main thread loop stops when the time is up and continues next frame) and burst mode (World.Streaming.BurstSeconds, Section 11.6) that multiplies the job, time and upload budgets by 4 at world entry until everything in range is ready.
- voxel::ChunkSource (the game's generator or save reader, run on workers) and voxel::ChunkMeshSink with voxel::ChunkRenderSink for the chunk renderer; ChunkRenderer::setUploadBudgetScale for burst mode.
- VoxelWorld::changedChunks: chunks that changed after they were meshed, each listed once until meshed again.
- Stats Voxel.Streaming.*: chunks per stage, loaded, meshed, uploaded, unloaded, cancelled and stale meshes per frame, main thread time, and the latency from request to first mesh on the GPU as a histogram. A new overlay page "Streaming and Jobs" (Section 18.2 page 4) shows them with per second rates, plus how busy each job worker is and the queues per priority.
- The game's demo world is now endless on X and Z: the demo terrain streamed around the camera, 8 chunk layers tall. The game enters it once every chunk within 8 chunks of the spawn is meshed. Console command `autofly <blocks per second>` flies the camera by itself for measurement runs.
- Tests: ranges and meshing once, the top and bottom of the world, moving with hysteresis, edits on chunk borders, outdated snapshots, cancellation, upload budgets, job limits and burst mode, remeshing after a setting change, render distance changes, streaming without a sink, unloading everything, stats, and a fast flight timing test.
### Changed
- Job queues skip empty queues without taking their mutex. Idle workers swept every queue mutex of every priority, so a thread scheduling a job sometimes waited over a millisecond.
- The game streams the world while in LoadingWorld (the renderer runs in that state now).
### Performance
- Demo world, render distance 16, 14 workers: playable (8 chunk radius meshed) 255 ms after entering LoadingWorld, everything in range (about 10,100 chunks) 461 to 521 ms.
- Flight at 300 blocks per second, 20,000 frames: streaming main thread time 0.062 to 0.079 ms average, at most 0.74 to 0.78 ms, no frame over 1 ms (before the job queue change: at most 1.11 to 4.65 ms). About 5,000 chunks loaded and 4,700 meshed per second; 64 ms median from request to mesh on the GPU.
### Docs
- docs/perf/streaming.md: E9.7 measurements, including the renderer costs while chunks stream in (draw list rebuild and cave search per chunk change) that the next commit addresses.
- README: the streamed demo world, autofly and the new overlay page.

## [#063] 2026-10-29, E9.6 GPU culling
### Added
- Chunk culling (Section 8.3) in a new Visibility compute pass. Every non empty face group of every resident chunk is a candidate draw and goes through four tests: frustum (Graphics.Culling.Frustum, with Graphics.Culling.FrustumPadding), face direction (Graphics.Terrain.FaceDirectionCulling, not for translucent), caves (Graphics.Culling.CaveCulling) and Hi-Z occlusion (Graphics.Culling.Occlusion).
- GPU path (render/chunk_cull.comp): survivors are appended with an atomic counter that the opaque and cutout draws read as their count (indirect count draws), so the CPU never waits; translucent groups keep their far to near order with culled ones drawn with no instances. CPU path for drivers without indirect count draws, or with Graphics.Culling.Gpu off: the same tests except occlusion, written into a per frame ring. Without the Visibility pass every candidate is drawn.
- render::CaveCuller: breadth first search from the camera's chunk through the chunks' 15 bit face connectivity, never reversing a direction; unknown chunks count as open. Runs only when the camera changes chunk or chunks change.
- render::HiZPyramid and render/hiz_reduce.comp: a depth pyramid of farthest depths built from the scene depth in a new HiZ pass; next frame's culling tests each chunk box against the level where it spans at most 2 x 2 texels. Power of two levels keep the texel to pixel mapping exact for any window size.
- Debug.FreezeCulling keeps the culling camera and the pyramid and draws the frozen view as yellow frustum lines.
- Culling counts per reason in the overlay (a new Overview line) and as Render.Culling.* stats, read from the GPU a few frames late through CpuRead buffers without stalling.
- The engine loop logs the recent frame rate, 1% low, GPU time and the culling counts at exit, so measurement runs with --frames need no screen reading.
- Tests: the cave search (open space, a closed shell, a tunnel around a bend, search time), the CPU path on the Null device (frustum, direction, walls of empty but solid meshes, live switches, freezing, drawing without culling), pyramid sizes, and a GPU test through render::Renderer that checks GPU and CPU counts match exactly and that occlusion hides a chunk behind a wall.
### Changed
- ChunkGeometry carries the mesh's connectivity and tight bounds; the bounds are packed into the w of the chunk metadata, so GPU bytes per chunk stay the same. A mesh without quads now draws nothing but still tells cave culling how the chunk connects; remove() forgets the chunk. The demo world sends its empty meshes too.
### Fixed
- Error contexts no longer read "Chunk renderer: : ..." (withContext adds the colon itself; 25 call sites).
### Performance
- Demo world spawn view, 1280x720, RTX 4060, release: GPU frame time 0.83 ms without culling, 0.42 ms with frustum, direction and cave culling, 0.31 ms with occlusion; 4,038 of 31,237 face groups drawn. Average FPS 1120 without culling, about 1990 with GPU culling (CPU limited from there); 1% low 420 to 578 before, 1000 to 1336 after.
- Cave search for render distance 32 (65 x 16 x 65 chunks): 0.46 ms when the camera changes chunk, about 1.1 ms after chunk changes.
### Docs
- docs/perf/render.md: E9.6 measurements.

## [#062] 2026-10-29, E9.6 RHI storage images and readback buffers
### Added
- CommandList::bindStorageImage binds one mip level of a Storage texture to a compute shader (imageLoad, imageStore). Images get their own binding space, set * 2 + slot, because OpenGL only guarantees 8 image units; shaders use the new VXE_IMAGE_BINDING macro. The depth pyramid for occlusion culling builds its levels this way.
- BufferUsage::CpuRead: persistently mapped buffers the CPU reads after a fence says the GPU copy into them is done, so GPU results (culling counts for the overlay) reach the CPU without stalling. readBuffer stays the slow path for tests.
- Validation rejects CpuWrite plus CpuRead on one buffer and storage images in depth, sRGB or BGRA formats. The Null device checks image binds (Storage usage, existing level, slot).
- Tests: Null checks for image binds and readback mapping; a GPU test that reduces one mip level into the next through storage images and reads a copy back through a CpuRead buffer.

## [#061] 2026-10-29, Demo world in the game
### Added
- vr::DemoTerrain (game worldgen, temporary until G3): deterministic terrain from value noise with lowlands and higher hills, grass, dirt, beaches, lakes at sea level 62, snow above 100, ores and gravel pockets in the stone, caves, a foundation stone floor, and oak trees gathered per chunk from 7 x 7 cells so chunks never need their neighbors (Section 12.2).
- vr::DemoWorld (game client, temporary until G0.2, G3 and E9.7): a 32 x 8 x 32 chunk field around the origin, generated and meshed on the job system while the game shows LoadingWorld, then uploaded to the chunk renderer within the per frame budget, nearest first. Flat placeholder colors per texture key until the default texture pack (G2).
- voxel_rethinked now starts into the demo world: about 0.4 s from LoadingWorld to InGame in release (8192 chunks generated in 331 ms, 4747 meshed in 56 ms on 14 workers), then 4735 chunks and 2.1 million quads on the GPU. The overview line shows the loading progress.
- Tests: terrain determinism and seed dependence, floor, surface and water rules, tree crowns across chunk borders, and the loading phases headless.
### Docs
- README: the demo world.

## [#060] 2026-10-29, G1.2 Block registry loader
### Added
- vr::BlockCatalog (game common module): loads the block definition files, gives every state an id (air 0, then file name order, so the same files give the same ids), names states ("oak_log[axis=y]") and maps names and ids both ways for save palettes (Section 9.3), resolves face textures (all, side, top, bottom, one face; the most specific wins; variants override), gives texture keys their array layers in first use order and builds the engine's BlockRegistryView.
- Invalid files are reported all at once, each problem with its file and field: TOML syntax with line and column, unknown fields and faces, bad names, layers and light values, missing textures, duplicate blocks and states.
- The game loads the catalog in its LoadingCoreAssets state from assets:/data/blocks and publishes it to a voxel::BlockRegistry; without valid blocks it stops with the report. The build copies game/assets next to voxel_rethinked.
- Tests: every shipped file loads (15 blocks, 18 states with air, 17 texture keys) and matches the engine table, id order and name lookups, and the error report for broken files.

## [#059] 2026-10-29, G1.1 Block data files
### Added
- game/assets/data/blocks/: one TOML file per block (Section 9.3, ADR-009): stone, dirt, grass block, sand, gravel, water, oak log (three axis states), oak leaves, glass, lumen block (a light source), snow block, coal, iron and gold ore, and foundation stone (the world floor). Each gives the stable name, display name, render layer, light values, same type culling, biome tint and texture keys per face; stone.toml documents the format.

## [#058] 2026-10-29, G0.1 Game application shell
### Added
- render::Renderer: the engine's standard frame in one frame graph (chunk uploads, opaque and cutout chunks, sky and fog, translucent chunks, debug lines, post processing, a window overlay hook), with GPU frame timing. Applications set the camera with beginFrame; the main loop records, submits and presents.
- runApplication now creates the graphics device, renderer, frame pacer, stat registry, console command registry and the debug tools (debug UI, F3 overlay, developer console) for windowed programs, and exposes them in EngineContext (device, renderer, stats, commands, overlay, console, inMenu). EngineConfig gains renderer, debugTools, shaderFolder and startupCommands. Frame pacing (Video.FpsLimit and friends) replaces the idle frame cap when rendering.
- The game build copies the engine shaders to shaders/ next to voxel_rethinked.
- Game: vr::GameStateMachine with the states and transitions of Section 20.1 (plus backing out of a failed world start and, until the menus exist, a quick start from LoadingCoreAssets to the world).
- Game: settings Gameplay.FieldOfView and ViewBobbing, Controls.MouseSensitivity, InvertY, ToggleSprint and ToggleCrouch, Interface.UiScale, HudOpacity and ShowHud, Accessibility.ScreenShake and FovEffects, Server.TickRate, AutosaveIntervalMinutes and SimulationDistance.
- Game: input contexts Global, Gameplay and Menu with the default actions of Section 17.5 (movement, interaction, hotbar 1 to 9 and wheel, inventory, pause, HUD, free camera, screenshot, fullscreen, menu back).
- voxel_rethinked now renders: it starts straight into a free flight over a debug grid under the sky, with the F3 overlay (state and position on the Overview), the console (new command "state"), Escape to pause and resume, F11 for borderless fullscreen and a SavingAndStopping pass on window close. --exec runs console commands at startup.
- Game code moved into the vr_game library; game unit tests in vr_tests (state machine, settings, actions without conflicts).
### Changed
- Input: an action counts as pressed only when one of its inputs went down this frame. Before, an action whose context came back while its key was still held (Escape closing the pause menu) saw a new press and paused again at once; a shortcut also no longer fires when its modifier goes down after its key.
### Docs
- README: running the game and the demo.

## [#057] 2026-10-29, E9.5 Chunk renderer
### Added
- vxe::OffsetAllocator in core: O(1) range allocation for the GPU vertex arena, after Sebastian Aaltonen's OffsetAllocator design (256 small float size bins, two level bit masks, neighbor merging). Written in house because vcpkg has no port; when the fast path fails it also checks the bin below, so a request for exactly the free space left succeeds.
- render::ChunkRenderer (Section 8.2): chunk quads in one vertex arena (OffsetAllocator in 8 byte quads), a shared index buffer for 4096 quads, a chunk metadata buffer read through gl_BaseInstance, and one indirect multi draw per pass (opaque, cutout, translucent) over all non empty face groups. Uploads go through a staging ring with a per frame budget; replaced and removed chunks free their ranges once the GPU finished the frames that drew them. Translucent chunks draw far to near, reordered when the camera enters another chunk.
- Shaders render/chunk.vert (vertex pulling of the packed quads, AO flip and winding per face, directional face shading, AO strength) and render/chunk.frag (texture array, cutout and translucent variants).
- render::createPlaceholderBlockTextures: a mipmapped 16 x 16 texture array per color until texture packs (G2).
- Settings World.Streaming.MaxUploadKBPerFrame, Graphics.Terrain.AoStrength and Debug.Terrain.Wireframe; stats Render.Chunks.Resident, Quads, ArenaBytes, BytesPerChunk, Draws, UploadBytes, UploadsOverBudget and UploadsOutOfMemory.
- vxe_rhi_demo --terrain: a 32 x 8 x 32 field of terrain chunks, meshed and drawn through the chunk renderer.
- Tests: allocator bins, merging, limits and 50,000 random operations against a byte map; renderer draw lists, metadata, the upload budget over frames, deferred frees, a full arena and the translucent order on the Null backend; on the GPU, pixels of a chunk at the origin and 10 million blocks away (identical) and every face head on with and without the AO flip.
### Performance
- Terrain field: 6592 meshed chunks at 1886 GPU bytes each (target 3 KB), uploaded in 3 frames, 0.64 ms GPU and 0.76 ms CPU per frame at 720p without culling.
### Docs
- docs/perf/render.md: E9.5 section.

## [#056] 2026-10-29, E9.4 Binary greedy mesher
### Added
- voxel::Mesher (Section 10): binary greedy meshing of a chunk snapshot. Opaque row masks give visible faces with shifts and ANDs; cutout and translucent blocks hide faces against opaque neighbors and against their own state when it culls its own type; a greedy merge over 16 x 16 face planes grows rectangles of faces with the same state, texture, AO and light. Reuses its scratch memory, one Mesher per worker thread.
- voxel::PackedQuad: the 8 byte quad of Section 10.4 (position, size, face, AO flip bit, texture layer, AO per corner, sky and block light) with accessors, and faceAxes() for the U and V axes per face.
- voxel::ChunkMesh: quads in 18 face groups (opaque, cutout, translucent, times 6 directions), the 15 bit cave culling connectivity mask from a flood fill over rows of open blocks, tight bounds, and the snapshot's position and version.
- Per vertex voxel AO with the diagonal flipped through the darker corner pair (Section 10.3). Light is full sky light and no block light until E10.
- voxel::sortBackToFront for translucent quads.
- Settings Graphics.Terrain.GreedyMerge, MergeAcrossLight and FancyLeaves (RequiresRemesh) through voxel::MesherSettings; the voxel module now depends on config.
- Stats Voxel.Mesher.Chunks, Quads and Time.
- Tests: single blocks, merging, border culling through the snapshot, water, glass and leaves rules, AO values and flips, the connectivity of walls, tunnels, pockets and bends, reuse, settings and stats, sorting, a checkerboard, and 40 random chunks checked face by face (coverage, texture and AO) against a brute force mesher.
- Meshing benchmarks: all air, all stone, random 50%, checkerboard, a surface chunk and the average over the terrain sample.
### Performance
- Terrain sample: 0.0224 ms per chunk on average (budget 0.06 ms), 225 quads and 1802 bytes per non empty chunk. All air 1.9 us, surface chunk 26 us, worst case checkerboard 482 us.
### Docs
- docs/perf/meshing.md.

## [#055] 2026-10-29, E9.3 Voxel world and snapshots
### Added
- voxel::VoxelWorld (Section 9.5): chunks from a pool allocator, a hash map from ChunkPos to chunk, insert (or replace), remove, find, block get and set by world position, and iteration. Reads of unloaded chunks return a given state; writes to them are refused.
- voxel::Chunk: blocks, a version, dirty flags (mesh, save) and a last access tick. Versions are stamps from one counter of the whole world and move on for every change a snapshot of the chunk would see: its own blocks, the border blocks of its 26 neighbors, and neighbors loading or unloading. Edits on a border update up to 7 neighbors.
- voxel::ColumnSummary: per chunk column the highest loaded non air block of each block column, kept up to date on every insert, removal and edit (falling back to the next block down when the highest one is removed), the loaded chunk heights and a surface estimate from the generator.
- voxel::ChunkSnapshot: the chunk plus a one block border from its 26 neighbors (18^3 states, 11.4 KB) with the version and a mask of the neighbors that were loaded; unloaded neighbors fill with a given state. VoxelWorld::createSnapshot fills one from compile time tables of border cells.
- ChunkStorage::decompressTo into a strided box, so the chunk is decoded straight into the inside of a snapshot.
- Stats: Voxel.World.Chunks, UniformChunks, Palette1Chunks to Palette8Chunks, RawChunks, BlockBytes, Columns, Snapshots and SnapshotTime.
- Tests: insert, find, remove and replace, version changes for inside, face, edge and corner edits and for loading and unloading, dirty flags, column summaries step by step and against a brute force scan after 3000 random operations, snapshots against the world block by block with mixed, uniform and missing neighbors, the int32 grid edge, storage counts and stats, strided decompression in every mode, and the snapshot time.
- World benchmarks (snapshots with uniform, mixed and missing neighbors, edits) in vxe_benchmarks.
### Performance
- Snapshot with uniform neighbors 2.59 us (target under 5 us), mixed neighbors 5.45 us, setBlock 8.5 ns.
- Palette decompression 2.3 us to 1.4 to 1.5 us per chunk with a decoder templated on the index width.
### Docs
- docs/perf/world.md; docs/perf/storage.md updated.

## [#054] 2026-10-28, E9.2 Chunk storage
### Added
- voxel::ChunkStorage (Section 9.4): the 4096 states of a chunk in uniform, palette (1, 2, 4 or 8 bit indices that never cross a 64 bit word) or raw mode, switching on writes. A 16 byte object with one heap block (Chunks memory tag) holding the indices and the palette.
- get and set by LocalPos or block index, fill, assign (builds the smallest storage from a full array in one pass), decompressTo (whole words at a time), compact (drops unused palette entries, shrinks the index width, back to uniform, raw back to palette), deep copies and moves, and memory accounting (dataBytes, memoryBytes).
- benchmarks/: the vxe_benchmarks target with nanobench (new vcpkg dependency) and the chunk storage benchmarks; option VXE_BUILD_BENCHMARKS.
- A synthetic terrain sample shared by tests and benchmarks.
- Tests: uniform size, every mode transition, random writes with up to 300 states against a reference, compaction, assign, copy, move and freed memory, and the Section 2 memory targets.
### Performance
- Uniform chunks 16 bytes (target 32); mixed chunks of the terrain sample 2003.4 bytes of block data on average (target 2048). get 1.9 ns, set 7.1 ns, palette decompression 2.3 us per chunk, assign 6.6 to 7.7 us per chunk.
### Docs
- docs/perf/storage.md.

## [#053] 2026-10-28, Ignore Tracy capture files
### Changed
- .gitignore ignores *.tracy, so profiler captures saved anywhere in the repository (not only in captures/) never get committed.

## [#052] 2026-10-28, E9.1 Block registry interface
### Added
- The voxel module (layer 12) with the block registry interface of Section 6.3: BlockStateId (16 bit, air is 0), RenderLayer, BlockFace (+X, -X, +Y, -Y, +Z, -Z) and BlockRenderInfo (layer, emitted light, light opacity, same type culling, texture layer per face, tint index).
- BlockRegistryBuilder: air at id 0, ids handed out in order, light values checked (0 to 15), a clear error when all 65,536 ids are used.
- BlockRegistryView: an immutable table that worker threads read without locks; unknown ids read as air.
- BlockRegistry: holds the current view and swaps it atomically on publish, numbering views by generation; jobs keep the view they took until they finish. Stat Voxel.BlockRegistry.States.
- Tests: ids and lookups, rejected light values, the id limit, publishing while old views stay valid, and four reader threads racing 200 publishes (repeated 200 times in release, all consistent).

## [#051] 2026-10-28, E8.4 Post and present
### Added
- render::PostProcess (Section 8.1 pass 9): one fullscreen pass from the HDR scene to the window with exposure, a tonemapping curve, optional FXAA on the tonemapped image and sRGB encoding (shader render/post.frag). Settings, all live: Graphics.Post.Tonemapper (None, Reinhard, Aces; default Aces, Narkowicz's fit with its 0.6 reference exposure), Graphics.Post.Exposure and Graphics.Post.Fxaa.
- render::tonemap, encodeSrgb and decodeSrgb: the shader's curves on the CPU, for tests and for colors picked on screen.
- render::FramePacer (Section 8.5): Video.PresentMode (Immediate, Vsync, Adaptive, Mailbox) applied when it changes, and the caps Video.FpsLimit, Video.FpsLimitUnfocused (default 30) and Video.FpsLimitInMenus (default 120); the lowest cap that applies wins. Stat Frame.LimiterWait.
- vxe_rhi_demo renders the scene in RGBA16F, tonemaps it in the Composite pass and paces frames with the FramePacer (window focus tracked; --vsync sets Video.PresentMode).
- Tests: tonemapping and sRGB curves, the GPU post pass against the CPU reference with live curve and exposure changes, the cap rules, and the limiter at 60, 144 and 240 FPS with varying frame work.
### Changed
- The sky's colors are given as they look on screen and decoded to linear, so the HDR pipeline and the tonemapper keep their look.
### Performance
- Limiter accuracy: 60.008, 144.001 and 240.000 FPS for caps of 60, 144 and 240 (2% required); 143.7 FPS in the demo with a 144 cap.
- Post processing at 1280x720 on the RTX 4060: the demo's Composite pass went from 0.028 ms (plain blit) to 0.098 ms.
### Docs
- docs/perf/render.md: E8.4 measurements.

## [#050] 2026-10-28, Give CPU budget tests more room on CI machines
### Fixed
- The E7.1 stat registry budget test failed in the Windows MSVC relwithdebinfo CI job of #048 (0.069 ms measured against 0.05 ms; 0.0073 ms locally). The stat registry and overlay budget tests keep their exact budgets on real machines and allow ten times as much where the CI variable is set, like the frame limiter tests since #046.

## [#049] 2026-10-28, E8.3 Sky and fog
### Added
- render::SkyRenderer (Section 8.1 pass 6): one fullscreen pass over the scene color that samples the scene depth. Where nothing was drawn it outputs the analytical sky (horizon to zenith gradient, a sun disk with a glow, darker haze below the horizon); over geometry it blends exponential height fog in closed form, colored like the sky towards the horizon so distant terrain fades into it.
- render::computeSkyParams: the sun's path from the time of day (rises east at 6, sets west at 18, tilted towards -Z) and day, dusk and night colors from the sun's height.
- Settings, all live: Graphics.Sky.Enabled, TimeOfDay, DayLengthMinutes (0 stops the clock; setting TimeOfDay by hand restarts the cycle there), SunSize, FogEnabled, FogStart, FogDensity, FogHeightFalloff and FogBaseHeight. Stats Render.Sky.TimeOfDay and Render.Sky.SunElevation.
- Shaders render/sky.vert and render/sky.frag.
- vxe_rhi_demo draws a Sky pass after the cube.
- Tests: sun direction and colors over the day, live settings and the day cycle, and on the GPU (VXE_GUI_TESTS=1) a blue noon sky, thick fog turning a red scene into the horizon color, and fog switched off live.
### Performance
- Sky pass at 1280x720: 0.042 ms GPU on the RTX 4060.
### Docs
- docs/perf/render.md: E8.3 measurements.

## [#048] 2026-10-28, E8.2 Camera, frame constants, debug draw
### Added
- render::Camera: double precision world position, yaw and pitch, reversed Z infinite projection. view() only rotates; positions become camera relative in double (relative()) or on the GPU from the camera's chunk and fraction (renderPosition()), per ADR-011.
- render::FreeCameraController: W, A, S, D, Space and Left Shift fly, Left Ctrl is fast, the right mouse button looks, the wheel changes the speed. Actions in a "Free camera" input context; settings Debug.FreeCamera.Speed, FastMultiplier, Sensitivity and FieldOfView, all live.
- render::FrameConstants and FrameConstantsBuffer: per frame shader data at set 0, slot 0 (matrices, camera chunk and fraction, forward, viewport, time), and shaders/include/frame.glsl with the matching block and cameraRelative(chunk, localPosition).
- render::CpuWriteRing: fenced round robin of persistently mapped buffers for data written every frame.
- render::DebugDraw: world space lines, boxes, chunk boxes and grids, made camera relative in double before they become floats, drawn in one call with vertex pulling (shaders debug/lines.vert and .frag). Settings Debug.Draw.ChunkBorders (F3+G), Debug.Draw.ChunkBorderRadius and Debug.Draw.DepthTest; stats Render.DebugDraw.Lines and Render.DebugDraw.Dropped; a fixed line limit, no allocations per frame.
- vxe_rhi_demo is flown with the free camera over a debug grid, with a DebugDraw pass in its frame graph, the camera position on the overview and --origin BLOCKS to start far from the origin. Startup --exec commands now run after every setting is registered.
- Tests: camera directions and projection, frame constants far from the origin, the constant ring, free camera input, debug shapes, chunk borders through F3+G, the line limit and its stats, no allocations, and bit identical lines and pixels with the camera at the origin and 10 million blocks away (GPU part with VXE_GUI_TESTS=1).
### Performance
- DebugDraw pass in the demo, about 440 lines at 1280x720: 0.133 ms GPU on the RTX 4060. Recording allocates nothing.
### Docs
- docs/perf/render.md: E8.2 measurements and the precision check.

## [#047] 2026-10-28, E8.1 Frame graph lite
### Added
- The render module (layer 9) with render::FrameGraph (Section 8.1). Passes are added in execution order through a PassBuilder and declare transient textures (sized relative to the output or fixed), color, depth and window attachments with load and store ops, sampled and storage texture reads and writes, and how they use imported buffers.
- compile() rejects reads before writes, sampling a texture the pass renders into, loading undefined contents, raster passes without attachments, window plus texture attachments, attachments on compute or transfer passes, wrong depth formats and bad or duplicate names, with messages naming the pass and resource.
- Transient textures are created on the first execute and recreated when the output size changes. Textures with equal format, size and usage share one GPU texture when their passes do not overlap (Debug.FrameGraph.Aliasing, live).
- Barriers after shader storage writes, planned once at compile, including writes late in one frame read early in the next (Hi-Z to culling).
- Every pass has a live toggle Debug.Passes.<name> (except passes created with canDisable = false). A disabled pass still clears the attachments it would have cleared.
- Every pass runs in a debug group and is timed on the GPU with a timestamp pair read four frames late (stat Gpu.Pass.<name>). Stats Render.FrameGraph.TransientMemory, Render.FrameGraph.Barriers and Render.FrameGraph.PassesRun.
- The overlay's GPU page lists every Gpu.Pass stat with its average and maximum over 60 frames and the total.
- operator|= for RHI flag enums.
- vxe_rhi_demo renders through a frame graph with the passes Cube, Composite and DebugUi; the frame graph owns its offscreen targets.
- Tests: every validation rule, pass order and render pass contents, resizing, aliasing on and off, barrier planning across frames, disabled passes, and drawing plus GPU timing on the OpenGL device (VXE_GUI_TESTS=1).
### Performance
- execute() with 11 passes: 0.0010 ms per frame in release (0.0105 ms in debug), 0 allocations in 1000 frames. Demo passes on the RTX 4060: Cube 0.061 ms, Composite 0.033 ms, DebugUi 0.011 ms.
### Docs
- docs/perf/render.md with the frame graph measurements.

## [#046] 2026-10-28, Check frame limiter accuracy only on real machines
### Fixed
- The frame limiter tests from #045 failed on the Windows CI runners (156.9 and 189.5 FPS measured for a 200 FPS cap, 136.4 for 144): shared CI machines stall threads for milliseconds. On CI the tests now check that the cap is never exceeded and that "set Video.FpsLimit 144" clearly lowers the rate; the 5% accuracy checks run on real machines (local: 200.0 and 144.0). The limiter itself is unchanged.
- test::timingIsReliable() (false when the CI variable is set) and test::environmentFlag() in the test helpers.

## [#045] 2026-10-28, E7.4 Developer console UI
### Added
- debug::DeveloperConsole: an ImGui console over the console commands (set, get, reset, find, list, help and any command a system registers). The Grave key opens and closes it; while open it pushes a "Console" input context that blocks gameplay input. Enter runs the line, Up and Down walk the history (repeats are kept once), Tab completes the word being typed (a single match is filled in, several are listed and their common start is filled in). Input, output and errors have their own colors.
- Settings Debug.Console.MaxLines and Debug.Console.HistorySize, the action debug.console.toggle and the stat Debug.Console.Commands.
- platform::FrameLimiter for Video.FpsLimit (Section 8.5): keeps a fixed frame schedule and waits with SDL's precise delay (sleep, then a short spin), so the average rate matches the cap closely without burning a core.
- vxe_rhi_demo registers Video.FpsLimit, reads it every frame and runs the console; --console opens it at startup and --exec COMMAND runs console commands at startup.
- Tests: command echo, errors and stats, history walking, completion, the Grave key and context blocking, drawing on the Null device, the limiter at 200 FPS (measured 200.0), and the acceptance test: "set Video.FpsLimit 144" typed into the console changes a running loop from 500 to 144 FPS (measured 499.9 and 144.0).

## [#044] 2026-10-28, Make the allocation counter test immune to allocation elision
### Fixed
- "Every global operator new is counted" failed in the Linux Clang release and relwithdebinfo CI jobs: C++ allows optimizers to remove the allocations of new expressions and std::allocator, and optimized Clang did that for the test's small objects, so fewer than 3 allocations were counted. The test now calls ::operator new directly, which cannot be elided, and checks for exactly 3. The counter itself was correct.

## [#043] 2026-10-28, E7.3 Overlay framework and first pages
### Added
- debug::DebugOverlay, the F3 performance overlay (Section 18.2): a compact Overview (FPS, 1% and 0.1% lows, CPU and GPU time, tracked memory, allocations and draw calls per frame, lines added by other systems), refreshed at Debug.Overlay.UpdateRate so it stays readable.
- Pages as dockable ImGui windows: Frame (ImPlot CPU and GPU graph of the last 1000 frames, 50/90/99/99.9 percentiles, histogram, hitch log of frames over Debug.Overlay.HitchFactor times the median), GPU (capabilities, per frame draw calls and state changes, memory, invalid commands, driver messages), Memory (per tag current and peak, allocations per frame), Input (context stack, active actions, recent raw input), Settings diff (changed settings with reset buttons) and All stats (filterable, with a CSV capture button). Systems add pages with addPage().
- Keys in an always active Debug input context: F3 toggles on release unless it was held for F3+Left or F3+Right, which switch pages.
- Settings Debug.Overlay.Visible, Page, Opacity, Scale, UpdateRate, CsvSeconds and HitchFactor.
- Stats Frame.CpuTime and Frame.GpuTime (1000 frames of history), Memory.Allocations, Gpu.DrawCalls, Gpu.StateChanges, Gpu.BufferMemory, Gpu.TextureMemory and Debug.Overlay.DrawTime.
- debug::StatCsvRecorder: records every stat once per frame for Debug.Overlay.CsvSeconds into captures/stats-<date>-<time>.csv, written on a background thread.
- DebugUiDesc::layoutFile: ImGui window layouts are loaded at creation and saved at destruction, never during frames.
- vxe_rhi_demo runs the overlay (--overlay-page N opens it on a page); the ImGui and ImPlot demos moved behind --imgui-demo.
- Tests: frame time summaries, F3 and chord handling through simulated keys, every page drawing on the Null device with valid commands, changed settings, frame and GPU stats, a CSV capture end to end, the overlay's frame budget, and two ImPlot line regression tests (vertex UVs on the Null device, drawn pixels on the GPU).
### Fixed
- ImPlot lines were nearly invisible: ImGui 1.93 stores baked line texture coordinates as (u0, u1, v, 1/thickness) while ImPlot 1.0 reads (u0, v0, u1, v1), so lines sampled random atlas texels. The debug UI now builds its atlas without baked lines, and both libraries draw anti aliased lines as geometry.
### Performance
- Overlay update: 0.0005 ms hidden, 0.0043 ms in compact mode (budgets 0 and 0.2 ms). Recorded in docs/perf/debug.md together with the E7.1 numbers.

## [#042] 2026-10-28, E7.3 checkpoint: global allocation counting
### Added
- vxe::memory::globalAllocationCount(): every global operator new since startup, for the overlay's Memory page (allocations per frame). Core replaces the global operator new and delete, forwarding to malloc and free with one relaxed atomic increment. Sanitizer builds keep their own allocator and report the count as unavailable.
- InputSystem::contextName, for the overlay's Input page.
### Changed
- clang-tidy excludes Visual Studio, vcpkg and third party headers (MSVC's "VC/Tools" folder matched the tools/ header filter).

## [#041] 2026-10-28, E7.2 ImGui and ImPlot integration
### Added
- Dear ImGui 1.93 (docking) and ImPlot 1.0 from vcpkg, in a new debug module (layer 11) that links them PUBLIC for developer panels.
- debug::ImGuiRenderer: draws ImGui through the RHI instead of ImGui's OpenGL backend. Vertices are pulled from a storage buffer (debug/imgui.vert reads 5 words per vertex), indices are 16 bit, and both live in a 3 frame ring of persistently mapped buffers guarded by fences. Implements ImGui's 1.92+ texture protocol (create, update, destroy, Alpha8 expanded to RGBA8). Scissors are flipped to the RHI's lower left origin.
- debug::DebugUi: owns the ImGui and ImPlot contexts, feeds them platform events (scancodes mapped to ImGuiKey, mouse, wheel, text, focus), and routes input: while ImGui wants the mouse or keyboard it pushes a "Debug UI" input context that blocks gameplay contexts below it.
- Window::setTextInputEnabled, since SDL only delivers typed text while text input is on; the UI asks for it only while a text field is active.
- vxe_rhi_demo shows the ImGui demo window, the ImPlot demo and a CPU, GPU and draw call readout on top of the cube (--no-ui turns them off).
- Tests: key mapping, the demo windows over 8 frames on the Null device with no invalid commands and no leaks, input capture and release, typed text reaching a text field, and a GPU test that reads back exactly 160,000 drawn pixels for a 400x400 window with 0 driver errors.
### Changed
- Keyboard navigation is off in the debug UI: with it, ImGui claims the keyboard whenever a debug window has focus, which would block gameplay keys while the overlay is open.
### Performance
- vxe_rhi_demo in release with both demo windows open: 0.40 ms CPU and 0.10 ms GPU per frame (0.27 ms and 0.03 ms without the UI).

## [#040] 2026-10-28, E7.1 Stat registry
### Added
- vxe::stats::StatRegistry in core (vxe/core/stats.hpp): counters, gauges, timers and histograms registered by dotted name (the prefix is the overlay category), up to 2048 stats.
- Lock free recording from any thread: each thread writes its own slots and marks them in a dirty bitset; endFrame merges only touched slots, so its cost follows the stats actually used. Gauges and histograms use shared atomics.
- Per frame values with preallocated history rings (no allocation per frame), average, maximum, per second rates from the frame durations, and quarter octave histograms with percentiles.
- ScopedStatTimer, and the registry's own Debug.Stats.MergeTime timer.
- VXE_SANITIZER_ACTIVE compile definition in sanitizer builds, so timing assertions can skip them.
- Tests: registration rules, counters summed across 8 threads, a stress test that closes frames while 4 threads record (no count lost, 290 repeated runs), gauges, timers, histograms, history, rates, and the 500 stat budget.
### Performance
- endFrame with 500 stats touched every frame and 5 recording threads: 0.0073 ms in release on the dev machine (budget 0.05 ms), 0.039 ms in debug.
### Docs
- ADR-017: the stat registry lives in core so lower layers can record stats; overlay pages stay in debug.

## [#039] 2026-10-28, Fix a racy job system shutdown test
### Fixed
- The test "Destroying the job system runs every queued job first" read the counter again after incrementing it, so with two workers both increments could land before either read and no job scheduled the extra one (500 instead of 501, seen once under TSan in CI). It now uses the value fetch_add returns, which is unique per job. The job system itself was correct.

## [#038] 2026-10-27, E6.6 RHI hardening
### Added
- rhi::DeferredReleaseQueue: destroyed objects wait until the GPU has completed the frame that may still use them, the rule a Vulkan backend needs, shared by every backend.
- The OpenGL backend inserts a fence per presented frame, polls them without waiting in beginFrame and present, and deletes deferred buffers, textures, samplers, shaders and programs once their frame completed. Handles become invalid immediately.
- Device::waitIdle (drains the GPU and releases everything pending) and DeviceStats::pendingDestroys.
- Device::liveResources, the leak report: every object not destroyed yet, by name. Both devices log it when destroyed; the demo fails its scripted run when it is not empty.
- Automatic names ("unnamed buffer 3") for objects created without a debug name, used for GL labels and the leak report; samplers keep their names; the engine VAO and cached framebuffers are labeled.
- Tests for the release queue, automatic names, the Null leak listing, and GL deferral (pending until frames complete or waitIdle). Every GL test now ends with an empty leak report.

## [#037] 2026-10-27, E6.5 GL command execution, sync, queries, swapchain
### Added
- gl::CommandReplayer: full command list replay for the OpenGL backend. Render passes to the window or to cached framebuffer objects, clears (float, integer, depth, depth stencil), LoadOp and StoreOp DontCare invalidation, viewport and scissor, uniform, storage and texture binds through the set * 16 + slot rule, index buffers on one engine VAO, draw, drawIndexed, multi draw indirect and indirect count (core 4.6 or ARB), compute dispatch, buffer copies, barriers mapped to glMemoryBarrier bits, debug groups and timestamps.
- Every command is checked before it reaches OpenGL (live handles, usage flags, offset alignment, ranges, binding limits, pass rules); invalid ones are counted and skipped. Unclosed passes and debug groups are closed and counted at the end of a list.
- Reversed Z clip space: glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE) at device start (ADR-010).
- Timestamp queries that never wait (createTimestampQuery, timestampNanoseconds) and rhi::GpuFrameTimer, a 4 frame ring that reads results 3 frames later.
- Device::readTexture and TextureUsage::TransferSrc for tests and screenshots; capabilities report uniform and storage offset alignment; DeviceStats counts draw calls and dispatches.
- shader_source helpers folderShaderReader and createShaderFromFile, shared by the checker, the demo and the tests.
- Engine shaders debug/cube (an indexed 8 corner cube with derivative based flat shading) and debug/blit (fullscreen triangle copy).
- tools/rhi_demo (vxe_rhi_demo): a spinning cube rendered offscreen with depth, copied to the window, with the triangle on top and GPU and CPU frame times in the title. --frames N runs a scripted check. Measured on an RTX 4060, release: 0.27 ms CPU and 0.05 ms GPU per frame, 0 invalid commands, 0 driver errors.
- GPU tests with pixel readback: reversed Z depth rejection, clipping below depth 0, identical images from direct, indirect and indirect count draws (count 0 draws nothing), compute fill with barriers and copies, the GPU frame timer, and exactly 8 invalid commands skipped without driver errors. Null device tests for timestamps, the timer and texture readback.
### Changed
- The OpenGL backend is split into gl_resources, gl_commands and gl_device.
- readBuffer copies into a host side readback buffer and waits on a fence, instead of reading the GPU buffer directly, which made NVIDIA move the source buffer to system memory.
- Shaders for the Null backend get VXE_OPENGL, since it follows OpenGL's binding model.
### Fixed
- The Linux Clang asan job failed to link vxe_shader_check because UBSan's vptr check needs RTTI; glslang is now built with its rtti feature.
### Docs
- ADR-016: RHI coordinate conventions (lower left origin, bottom row first, 0 to 1 depth, window shows values as written).

## [#036] 2026-10-27, E6.4 OpenGL shaders and pipelines
### Added
- rhi/shader_source: the GLSL preprocessor shared by runtime and build. It resolves #include "path" relative to the shader root, honors #pragma once, rejects cycles, unsafe paths and #version in files with file:line messages, writes #version 460 core plus backend and stage defines, and emits #line directives so every line maps back to its file.
- annotateShaderLog rewrites NVIDIA, AMD, Mesa and glslang compiler logs into file:line messages.
- Engine shaders in engine/shaders: include/bindings.glsl with the VXE_BINDING macros (Section 7.3) and the debug triangle and fill buffer shaders for the RHI smoke tests.
- tools/shader_check (vxe_shader_check): runs the engine preprocessor and glslang 16.4 on each shader and prints clickable absolute file:line errors. cmake/Shaders.cmake checks every .vert, .frag and .comp file as part of the build, so an invalid shader fails the build.
- CTest cases that expect the checker to fail on a broken shader, a broken include file and a missing include, each with the right file and line.
- OpenGL shaders and pipelines: compile errors returned with file names (for example include/colors.glsl:3), program linking with shaders detached so they can be destroyed early, and immutable pipelines holding all fixed function state.
- gl::StateCache: redundant state filtering for program, polygon mode, culling, front face, depth clamp, depth test, write and compare, and per attachment blending. Measured on an RTX 4060: 29 state calls for the first pass over 8 binds, 13 for the same binds once the state is known.
- Pipeline binds replayed by submit (the rest of command replay follows in E6.5); repeated binds of the bound pipeline are skipped. DeviceStats counts pipeline binds, skipped binds and state changes, on both backends.
- GPU tests (VXE_GUI_TESTS=1) for engine shaders, pipelines, state filtering, readable compile and link errors, and stage checks.
### Changed
- ShaderDesc carries the file names behind each source number, and the debug callback no longer repeats compiler messages that already come back as errors.
- Raster, depth and blend state are comparable; the alpha blend comment now describes straight alpha correctly.

## [#035] 2026-10-27, E6.3 OpenGL device, buffers, textures, samplers
### Added
- engine/third_party/glad: the glad2 2.0.8 loader generated for OpenGL 4.6 core plus bindless textures, indirect parameters, gl_spirv, anisotropic filtering, KHR_debug and the NVX and ATI memory info extensions, with the exact regeneration command. Built as C++ and linked only by rhi_gl, privately.
- vxe_rhi_gl module (layer 8) with createGlDevice: loads GL through SDL, requires OpenGL 4.5, detects indirect count draws, bindless textures, SPIR-V, timestamps, texture limits, anisotropy and video memory, and logs them at startup.
- KHR_debug output with synchronous delivery by default; driver errors and warnings go to the new rhi log category and are counted in DeviceStats. A startup marker message proves the callback works before debug output is reported as active.
- Direct State Access resources: immutable buffers (CpuWrite buffers persistently and coherently mapped), 2D and 2D array textures with per mip and per layer uploads for every RHI format, samplers with anisotropy clamped to the GPU, fences, present and present modes, object labels for debuggers.
- Device::readBuffer for slow test and debug readback, and DeviceCapabilities fields for debug output, video memory and the API version.
- A GPU test (VXE_GUI_TESTS=1) that checks capabilities, buffer and mapped memory round trips, uploads of all formats and mips, samplers, fences and present against the real driver with zero driver errors or warnings.
### Changed
- Upload and readback rules moved into rhi validation and are shared by both backends: writes need TransferDst, reads need TransferSrc, as in Vulkan.
- Window::glGetProcAddress returns a function pointer, so loading GL needs no cast between function and object pointers.
- The GUI test switch moved into the shared test helpers.

## [#034] 2026-10-27, E6.2 Null backend
### Added
- The full Null device: CpuWrite buffers get real persistent CPU memory for mappedRange, updateBuffer and uploadTexture check ranges, mip levels, layers and data sizes like a GPU backend, and textures beyond the reported limits are refused.
- Pipelines are checked against their shaders: every shader must exist and sit in the right stage.
- submit walks every command, counts it and flags invalid ones (destroyed resources, draws outside a render pass, compute or copies inside one, nested or unclosed passes) in a new DeviceStats::invalidCommands.
- Device stats with object counts and buffer and texture memory, present counting, immediately signaled fences, and a leak report naming every live resource when the device is destroyed.
- Tests for stats and memory sizes, mapped memory, upload checks, valid and invalid command lists, shader stage checks, fences and device limits.
### Fixed
- GCC 14 rejected the narrowing into the 24 bit handle index (-Wconversion); handles are now built through Handle::make, which masks the index.

## [#033] 2026-10-27, E6.1 RHI interface and handles
### Added
- vxe_rhi module (layer 7): typed 32 bit handles (24 bit index, 8 bit generation) and a HandlePool that turns use after destroy into a null lookup.
- RHI types shaped like Vulkan and D3D12 (ADR-003): formats, opt in usage and barrier flags, raster, depth (reversed Z GreaterOrEqual by default), blend and topology state, and descriptions for buffers, textures, samplers, shaders, pipelines, render passes and texture uploads.
- CommandList: a non virtual, flat byte buffer of plain command structs (render passes, binds, draws, indirect and indirect count draws, dispatch, copies, barriers, debug groups, timestamps) with an iterator for backends; reset keeps the memory.
- The Device interface with capabilities, stats, resource creation and destruction, uploads, mapped ranges, fences, submit and present.
- Validation of every description, called by backends on creation, plus mip count and level size helpers.
- vxe_rhi_null module (layer 8) with the Null device skeleton (validated handles, stubs) and createNullDevice.
- Tests for handle typing and staleness, generation wrap, command recording and replay of every payload, validation rules, size helpers and the Null device.
### Docs
- ADR-015: backends expose their own create functions; the factory that picks one lives in render.

## [#032] 2026-10-27, E5.5 Bindings, persistence, conflicts, capture
### Added
- vxe/input/keybinds.hpp: readable binding text ("W", "Ctrl+S", "F3+G", "Mouse Left", "Wheel Up", "key:Z", "Scancode 300") in both directions, forgiving about case and spaces, and keybinds.toml with a commented line per action (name, category, defaults), up to three bindings, unknown actions kept.
- Controls.BindByKeyName (advanced setting): keybinds saved by key label in the current layout instead of physical position, resolved back to the matching key when loaded.
- Conflict detection: same physical input in the same context or an always active one; stacked contexts never conflict because the stack already decides. Swap, unbind and reset (action, category, all) helpers.
- Capture for the keybind screen: the next key with its held modifiers, mouse button or wheel direction; Escape cancels, Backspace clears, a modifier pressed and released alone is captured as itself, and the captured input stays silent until released.
- Platform: layout independent scancode names and lookups by name or by layout label.
- The engine loop registers the input settings and loads and saves config:/keybinds.toml next to settings.toml.
- Tests for binding text round trips, hand written text, TOML round trips, a hand edited file with warnings and unknown actions, conflicts and resolutions, every capture case and the setting.
### Changed
- runApplication loads and saves the player's files through two small helpers.

## [#031] 2026-10-27, E5.4 Raw input and actions
### Added
- vxe_input module (layer 6): InputSystem with per frame raw state (keys by scancode with press edges, mouse buttons, position, summed raw motion, wheel, typed text) and actions of three types: Button (pressed, held, released), Toggle and Axis (wheel notches, or 1 while a key is held).
- Bindings for keys with required modifiers, chords such as F3+G, mouse buttons and wheel directions; up to three per action, with defaults kept for resetting.
- Input contexts on a stack: from the top down a context claims the physical inputs of its active actions, a blocking context (the console) hides everything below it, and always active contexts (Global) are never blocked.
- Taps that go down and up between two frames still count once, key repeats are not new presses, and losing window focus releases everything so no key stays stuck.
- The engine loop updates input from the frame's OS events and exposes it in EngineContext.
- Tests for edges, taps, repeats, toggles, axes, context claiming and blocking, inactive contexts, modifiers, chords, mouse state, focus loss and rebinding.

## [#030] 2026-10-27, E5.3 Console commands
### Added
- vxe/config/console.hpp: CommandRegistry with command descriptions, a quote aware tokenizer, case insensitive command names, "did you mean" hints and autocomplete for the word being typed.
- Settings commands: set (strings take the rest of the line), get, reset with a single id, a category wildcard (Graphics.LOD.*) or everything (*), find over ids and display names, list by category and help.
- parseSettingValue and displaySettingValue for typed text (on, off, yes, no, 1, 0 for booleans, option names in any case), shared with the future console and settings screens.
- Autocomplete covers command names, setting ids (any case), categories, enum options and on or off.
- Tests for tokenizing, every command, helpful failures, wildcard resets, search, completions and custom commands.

## [#029] 2026-10-27, E5.2 Settings persistence and presets
### Added
- vxe/config/settings_file.hpp: settings saved as TOML with one table per category and a comment above every setting (name, description, default, range or options, when it takes effect), enums by name, written atomically; loaded with toml++ through the normal validated set.
- Loading is forgiving: wrong types and impossible values become warnings that name the key, integers are accepted for float settings, enum names ignore case, and invalid TOML fails with its line and column.
- Unknown keys are kept verbatim and written back on save, so older and newer versions do not erase each other's settings.
- Presets: definePreset, applyPreset, presetNames and currentPreset (Custom when nothing matches).
- Global and per world scopes save and load through separate files.
- The engine loop loads config:/settings.toml after onInit, writes the commented defaults on first start, and saves after onShutdown.
- Tests: round trip into a fresh registry, a hand edited file with comments, clamped values, wrong types, case insensitive enums and unknown tables, invalid TOML, files on disk, presets and scopes.

## [#028] 2026-10-27, E5.1 Settings registry
### Added
- vxe_config module (layer 5) with SettingsRegistry: bool, int, float, enum and string settings with display name, description, apply mode, scope, visibility, range and step, enum options and length limits; the category comes from the dotted id.
- Setting<T> handles read with one relaxed atomic load from any thread. Writes from any thread are clamped to range, NaN, unknown enum values, wrong types and too long strings are rejected, and the value is visible immediately.
- Change callbacks with RAII subscriptions run only in dispatchChanges() on the main thread; several changes of one setting in a frame become one callback (oldest to newest), and a change back to the start value is dropped.
- Generic access by id (value, set, reset, resetCategory by prefix, isDefault, find, all, categories) for the console, persistence and the generated settings screen.
- The engine loop owns the registry, exposes it in EngineContext and dispatches setting changes every frame.
- Tests for defaults and metadata, clamping, rejection, dispatch timing and merging, subscription lifetime, writes from another thread, generic access, category resets and repeated registration.
### Changed
- clang-tidy no longer asks for braced return values (modernize-return-braced-init-list).

## [#027] 2026-10-27, E4.5 Application main loop
### Added
- vxe_app module (new top layer 13): Application (onInit, onFrame, onQuitRequested, onShutdown), EngineContext (platform, window, jobs, file system, file watcher, event bus and the frame's OS events) and runApplication.
- runApplication starts the log, platform, runtime folder mounts (config and saves created, assets and textures when present), file watcher, event bus, window and job system, runs the frame loop (OS events, file watcher, main thread tasks, event bus, onFrame) with an idle frame cap, and shuts down in a fixed, documented order.
- The game runs inside the engine loop: VoxelRethinkedApp, with --frames N and --headless on the command line.
- Tests: lifecycle order with a job reporting back on the main thread, a simulated window close ending the loop through onQuitRequested, and a failing onInit returning 1 while still shutting down.
### Docs
- ADR-015 gains layer 13 (app), because Application needs modules above core.

## [#026] 2026-10-27, E4.4 Platform window
### Added
- vxe_platform module (layer 4) on SDL3 3.4.18, with SDL private to the module: Platform (single instance, optional video for headless use), event translation into engine events (quit, resize, pixel size, focus, minimize, display scale, keys by scancode with modifiers, mouse buttons, motion, wheel, text), key names in the current layout and the executable folder.
- Window: windowed, borderless and exclusive fullscreen, high DPI with logical and pixel sizes, relative mouse mode, and an OpenGL 4.6 core context (4.5 fallback) with an optional debug flag. The context and loader are handed to the future OpenGL backend; no GL call happens in the platform module.
- Tests: headless start, single instance, synthetic SDL events translated correctly, and (with VXE_GUI_TESTS=1) a real window that opens with OpenGL 4.6 core debug, resizes, switches borderless, exclusive fullscreen and windowed, toggles relative mouse mode and closes.
- CI installs the X11 and Wayland development packages SDL3 needs on Linux.
### Changed
- vcpkg.json takes SDL3 without its default features: X11 and Wayland on Linux only, no ibus (which would build a whole D-Bus stack).

## [#025] 2026-10-27, E4.3 Catch files created in new folders on Linux
### Fixed
- Linux has no recursive watches, so files written into a brand new sub folder before efsw watched it were missed (Linux CI failure; copying a texture pack folder would hit it). Watches now enable efsw's synthetic events, which report the files already in a newly watched folder; the per file debounce merges any duplicates.

## [#024] 2026-10-27, E4.3 File watcher
### Added
- vxe/io/file_watcher.hpp: FileWatcher over efsw (hidden behind a private implementation), recursive or flat watches, unwatch, and poll() that delivers events on the calling thread.
- Changes are collected per file and delivered once the file has been quiet for the debounce time (200 ms by default), so an editor's burst of writes becomes one reload.
- efsw's thread is stopped before the state it writes to is destroyed.
- Tests that change real files: creation, modification in sub folders, removal, a burst of five writes giving one event, and no events after unwatch. Repeated 20 times in release and under ASan without a failure.

## [#023] 2026-10-27, E4.2 Async IO and compression
### Added
- vxe/io/binary.hpp: BinaryWriter and BinaryReader with little endian on every platform, IEEE float bits, LEB128 and ZigZag varints and length prefixed strings. The reader fails safely with a sticky error flag instead of reading out of bounds.
- vxe/io/compression.hpp: zstd (saves, default level 3) and LZ4 (caches) with allocating and caller buffer variants, size checked decompression and per thread zstd contexts.
- vxe/io/file_io.hpp: readFile and writeFile (atomic temp then rename by default), and AsyncFileIo running reads and writes on the IO threads with completion on the main thread, a worker or the IO thread.
- The io module now depends on jobs; zstd and LZ4 are linked privately.
- Tests for byte order, every value type, varint extremes, malformed input, compression round trips on compressible, random and empty data, corrupt data, buffer reuse, atomic replacement and async completion targets.
### Changed
- clang-tidy skips clang-analyzer-optin.core.EnumCastOutOfRange, which fires inside the MSVC standard library.

## [#022] 2026-10-27, E3.3 Fix job statistics read right after a wait
### Fixed
- The waiting for dependencies count dropped only after the released job was queued, and the completed count rose only after a job counted as done, so a thread that had just waited could read stale numbers (Linux GCC CI failure). Both are now updated before the job can be observed as queued or done. Repeated 200 times in release and debug and 40 times under ASan without a failure.

## [#021] 2026-10-27, E4.1 Virtual file system
### Added
- vxe_io module (layer 3) with Vfs: named mounts (assets, textures, config, saves) over real folders, read only or read write, optionally created on mount, behind a shared mutex for concurrent lookups.
- normalizeRelativePath: separators unified, dot parts removed, and anything that could leave the mount or alias another file rejected (climbing above the root, absolute and drive paths, colons and alternate streams, control and reserved characters, Windows device names, trailing dots and spaces). The same rules apply on every platform.
- resolve, resolveForWrite (refuses read only mounts), exists and list (sorted, non throwing iteration).
- Tests for normalization, every rejection rule, mount resolution, access control, listing and remounting.

## [#020] 2026-10-27, E3.3 Job stats
### Added
- JobSystem::stats(): jobs run and busy time per compute worker, per IO thread and for helping threads, queue depth per priority for both pools, jobs waiting for dependencies, completed and cancelled totals, and jobs per second since the previous call.
- Counters are per thread, cache line aligned and relaxed, so recording them takes no locks.
- Tests for per thread attribution, busy time, per priority depth while a worker is blocked, cancelled and waiting counts, and the rate between calls.
### Performance
- 1,000,000 job stress in release: 0.42 to 0.43 s with the two clock reads per job (0.46 s before), so the timing has no measurable cost.

## [#019] 2026-10-27, E3.2 Dependencies and cancellation
### Added
- JobOptions (priority, cancel token, pool), constructible from a bare Priority so existing calls stay short.
- scheduleAfter: a job queued when all of its JobHandle dependencies finish, without holding a thread while it waits. A finished dependent always implies finished dependencies.
- CancelSource and CancelToken (vxe/jobs/cancel.hpp): jobs cancelled before they start are skipped but still complete; running jobs can check the token and return early.
- runOnMainThread and runMainThreadTasks: tasks from any thread run on the owner thread in posting order, double buffered so tasks may post more tasks.
- IO threads (Pool::Io, 2 by default): IO jobs never run on compute workers or in wait(), so blocking disk work cannot stall CPU work. Without IO threads they fall back to the compute pool.
- Shutdown drains continuations and IO jobs too.
- Tests for chains, diamonds, finished and empty dependencies, 200 way fan out and fan in, cancellation before and during a job, main thread order and nesting, IO isolation and shutdown. Repeated 200 times in release and debug and 40 times under ASan without a failure.
### Fixed
- A finishing job released its dependents before counting itself as done, so a dependent could finish while its dependency still reported unfinished (found by the repeated release runs).

## [#018] 2026-10-27, E3.1 Job scheduler
### Added
- vxe_jobs module (layer 2): JobSystem, a work stealing thread pool with five priorities (Critical to Background). Each worker owns a ring buffer queue per priority (LIFO for its own jobs, FIFO for thieves), other threads submit to shared queues, and the highest priority job anywhere runs first.
- JobCounter for allocation free batches and JobHandle (with a keep alive so dropped handles are safe). wait() runs other jobs while waiting, so nested waits cannot deadlock.
- Idle workers spin briefly, then sleep with a lost wakeup free handshake. The destructor drains every queued and running job before joining.
- vxe/core/inline_function.hpp: move only void() callable with fixed inline storage (48 bytes for jobs), so scheduling never allocates.
- Tests: exactly once execution, 1,000,000 job stress (0.46 s release, 0.97 s ASan), nested waits on 2 workers, strict priority order, handle lifetime, shutdown drain and move only captures. Repeated 50 times in debug and release without a failure.
### Performance
- 1,000,000 tiny jobs scheduled from one thread and run: 0.46 s in release (about 0.46 microseconds per job, dominated by the shared queue mutex). Recorded as the baseline for a lock free queue later.

## [#017] 2026-10-27, E2.3 Projection helpers
### Added
- vxe/math/projection.hpp: perspectiveReversedInfinite (reversed Z, depth 0 to 1, infinite far plane), cameraRelativeView, CameraPosition (chunk plus fraction split exactly from double positions) and relativeToCamera for chunks and blocks with integer differences first.
- Tests: near maps to 1, depth falls towards 0 without reaching it, surfaces 1 block apart at 32,000 blocks get different depths, and camera relative positions stay exact 10 million blocks out.
- doctest printers for ChunkPos, BlockPos and LocalPos in the test helpers.

## [#016] 2026-10-27, E2.2 Fix sign conversion in the raycast
### Fixed
- raycastBlocks indexed std::array with an int axis, which fails -Wsign-conversion on Linux CI (MSVC does not warn). Axes are now usize, with explicit casts for glm's int indices.

## [#015] 2026-10-27, E2.2 Geometry
### Added
- vxe/math/face.hpp: Face in the engine wide order (+X, -X, +Y, -Y, +Z, -Z) with opposite, axis, sign and normal helpers.
- vxe/math/aabb.hpp: Aabb with empty, expand, contains, intersects, translated and inflated.
- vxe/math/frustum.hpp: frustum planes extracted from a view projection matrix, including the degenerate far plane of the reversed Z infinite projection, and a positive vertex box test.
- vxe/math/raycast.hpp: Amanatides and Woo voxel traversal in double precision with a templated solidity predicate, returning block, entry face, distance and point.
- Tests for frustums from standard and reversed infinite projections, box culling, and raycasts along axes, diagonals, negative coordinates, 30 million blocks out, from inside a block and with a zero direction.
### Changed
- Test code may access optionals after doctest REQUIRE (engine/tests/.clang-tidy).

## [#014] 2026-10-27, E2.1 Coordinates and vectors
### Added
- vxe_math module (layer 1) with glm 1.0.3, built with GLM_FORCE_DEPTH_ZERO_TO_ONE and GLM_FORCE_INTRINSICS as public definitions so every file sees one glm layout.
- vxe/math/vector.hpp: Vec, IVec, DVec, Mat4 and Quat aliases.
- vxe/math/coords.hpp: ChunkPos (int32), LocalPos (0 to 15, Y major index), BlockPos (int64), toChunk, toLocal, toBlock, chunkOrigin, blockAt for double positions with clamping, chebyshevDistance without overflow, std::hash and std::formatter specializations.
- Tests for negative coordinates, int32 grid extremes, all 4096 local indices, NaN and huge entity positions, hash spread over 4096 neighbors and log formatting.

## [#013] 2026-10-27, E1.6 Profiling macros
### Added
- vxe/core/profile.hpp: VXE_PROFILE_SCOPE, VXE_PROFILE_FUNCTION, VXE_PROFILE_FRAME, VXE_PROFILE_PLOT and VXE_PROFILE_MESSAGE on Tracy, compiled out unless VXE_ENABLE_PROFILING is on. profiling::setThreadName names threads for Tracy and for debuggers.
- A "profiling" vcpkg feature with the on demand Tracy 0.14.1 client, enabled only by the relwithdebinfo preset.
- Log messages appear in the Tracy timeline. EventBus::dispatch and the game startup are profiled.
- CI jobs for relwithdebinfo on Windows and Linux. The vcpkg cache key now includes the preset.
### Docs
- README explains how to connect Tracy.

## [#012] 2026-10-27, E1.5 Event bus and time
### Added
- vxe/core/event_bus.hpp: typed deferred event bus. publish() from any thread, dispatch() on the owner thread in publish order, RAII Subscription. Handlers may subscribe or unsubscribe while dispatching, and events published during a dispatch wait for the next one.
- vxe/core/time.hpp: steady clock aliases and conversions, Stopwatch, ScopedTimer, and FrameTimer producing FrameTime with clamped and raw deltas.
- Tests for ordering, type routing, subscription lifetime and moves, changes during dispatch, publishing from 4 threads, and frame time clamping with injected time.

## [#011] 2026-10-27, E1.4 Containers, hashing, string ids
### Added
- vxe/core/containers.hpp: HashMap and HashSet on ankerl::unordered_dense, plus StringHash and StringMap for string_view lookups without temporaries.
- vxe/core/hash.hpp: XXH3 hashBytes, hashString and hashValue (padding free types only), constexpr hashCombine and FNV-1a. xxHash is linked privately.
- vxe/core/string_id.hpp: StringId, a 64 bit id hashed at compile time with the _sid literal or at runtime. Debug builds keep every name in a registry and stop on hash collisions.
- vxe/core/small_vector.hpp: SmallVector with inline storage for N elements, tracked heap spill, and std::vector compatible names.
- Tests with reference hash values (XXH3 and FNV-1a), StringId lifetimes, map lookups and SmallVector growth, copies, moves and self push_back.

## [#010] 2026-10-27, E1.3 Memory
### Added
- vxe/core/memory.hpp: memory tags with lock free per tag counters (current bytes, peak, allocation and free counts, one cache line per tag), tracked aligned allocate and deallocate, recordAllocation for external memory, and TrackingAllocator for standard containers.
- LinearAllocator: arena with markers, rewind, reset, peak tracking and typed arrays for trivial types.
- PoolAllocator: O(1) create and destroy on growing pages with an intrusive free list. Pointers stay stable while it grows.
- ScratchScope: a 4 MiB per thread scratch arena created on first use, released when the scope ends, nestable.
- Tests for tag totals, container tracking, alignment, arena overflow, pool slot reuse and per thread scratch arenas.
### Changed
- clang-tidy no longer flags computed array indices, unchecked operator[] or missing math parentheses, and ignores macros for boolean simplification.

## [#009] 2026-10-27, E1.2 Result and Error
### Added
- vxe/core/error.hpp: ErrorCode and Error with code, message and the source location where it was created, plus withContext() and describe().
- vxe/core/result.hpp: Result<T, E> and Result<void, E> on std::variant, with asserted value() and error() access, operator* and ->, valueOr(), and the VXE_TRY macro to pass errors up.
- Shared test helpers (throwing assert handler) and Result tests, including move only values and wrong side access in debug.
### Changed
- toString(Level) is now nameOf(Level), because doctest picks up toString through ADL to print values.
- Engine tests build with DOCTEST_CONFIG_USE_STD_HEADERS so failures can print std::string_view.

## [#008] 2026-10-27, E1.1 Types, asserts, logging
### Added
- vxe/core/types.hpp: fixed width aliases (u8 to u64, i8 to i64, f32, f64, usize, isize).
- vxe/core/assert.hpp: VXE_ASSERT (debug only, condition not evaluated when off), VXE_VERIFY (always), VXE_UNREACHABLE, and a replaceable assert handler. The default handler logs the expression, message and location, breaks into an attached debugger and aborts.
- vxe/core/log.hpp: log categories with atomic levels, a category registry, compile time checked std::format messages, a reusable per thread format buffer, and console plus logs/latest.log outputs. spdlog (built on std::format) stays private to log.cpp.
- The game logs its version to logs/latest.log next to the executable and turns escaping exceptions into a logged error.
- Unit tests for asserts in debug and release, category filtering, the registry and log file creation.
- cmake/triplets/x64-windows-static-md-asan.cmake: vcpkg packages for the Windows asan preset are built with ASan, because MSVC refuses to link ASan code with libraries built without it.
### Changed
- The clang-tidy CMake option is replaced by a tidy target that reads compile_commands.json, because CMake's MSVC wrapper made clang-tidy report exceptions as disabled.
- clang-tidy ignores macros for cognitive complexity and allows do { } while (false) macros.

## [#007] 2026-10-27, E0.4 Fix include check on CMake 3.x
### Fixed
- The include check and rule tests run as cmake -P scripts, which start with old policy defaults on CMake 3.x, so if(IN_LIST) failed on the Linux runners. Both scripts now set cmake_policy(VERSION 3.28). Verified locally with CMake 3.31.6.

## [#006] 2026-10-27, E0.4 CI
### Added
- GitHub Actions workflow: clang-format 22.1.3 check, plus build and test on Windows MSVC (debug, release), Linux Clang 18 (debug, release, asan, tsan) and Linux GCC 14 (debug), with warnings as errors.
- vcpkg is cloned at the pinned baseline and its binary cache is kept between runs with actions/cache.
### Changed
- An empty CXX or VCPKG_DEFAULT_TRIPLET environment variable now counts as unset when picking the Windows defaults.
- UBSan in the asan preset no longer recovers, so undefined behavior fails the run.

## [#005] 2026-10-27, E0.3 Module dependency enforcement
### Added
- cmake/VxeModule.cmake with vxe_add_module(): one layer table for all engine modules, strictly lower layer dependencies only, an allow list for net, glad and OpenGL linkable only PRIVATE by rhi_gl, no engine to game links.
- cmake/CheckIncludes.cmake: rejects includes of engine modules outside the dependency closure and graphics API headers outside rhi_gl, at configure time and again at build time when files change. The game is checked too.
- vxe::vxe convenience target linking every engine module.
- 13 CMake script tests (cmake/tests) proving every rule fires with the right message.
### Changed
- vxe_core is created through vxe_add_module(). The game and vxe_tests link vxe::vxe.
### Docs
- ADR-015 records the layer order.

## [#004] 2026-10-26, E0.2 Tooling
### Added
- .clang-format (LLVM based, 4 spaces, 110 columns, grouped includes) and .clang-tidy (naming rules from the coding standards).
- cmake/Format.cmake with format and format-check targets and a VXE_ENABLE_CLANG_TIDY option. Finds the LLVM tools bundled with Visual Studio.
- .editorconfig and .gitattributes (LF in the repository, binary assets never converted).
### Changed
- .gitignore now also covers IDE folders, CMakeUserPresets.json and the runtime folders logs, captures, saves and screenshots.
- The game prints its version with iostream instead of printf (clang-tidy vararg finding).
### Removed
- .idea project files from the repository (they stay on disk for CLion).
### Docs
- docs/adr with ADR-001 to ADR-014 and an index.
- README mentions the format targets and the ADR folder.

## [#003] 2026-10-26, E0.1 Repository and build skeleton
### Added
- CMakePresets.json with debug, release, relwithdebinfo, asan and tsan presets (Ninja, vcpkg toolchain).
- vcpkg.json manifest with a pinned baseline. Only doctest for now, other libraries arrive with the step that uses them.
- cmake/Warnings.cmake with high warning levels and an opt in warnings as errors switch.
- cmake/Sanitizers.cmake for AddressSanitizer and ThreadSanitizer, including the MSVC ASan runtime copy.
- Targets vxe_core (engine library), voxel_rethinked (game, prints its version) and vxe_tests (doctest, no tests yet).
- README build instructions.
### Changed
- Root CMakeLists.txt now targets CMake 3.28+, C++20 without extensions, MSVC on Windows and one shared bin folder.
- main.cpp moved to game/src/app/main.cpp.
- .gitignore extended with build output and vcpkg folders.

## [#002] 2026-10-26, README
### Docs
- README.md updated to its final state.

## [#001] 2026-10-26, Initial commit
### Added
- CLion starter project, MIT license and README.
