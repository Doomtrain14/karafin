# gfx: renderer, text, images

Namespace `karafin::gfx`. Depends on `core` only. **All GL work happens on the main thread**; image
decode/resize/blur run on `karafin::workers()`. Include `gfx/gfx.hpp` for everything.

Coordinates are **logical pixels** in the 1280x720 space, y down. The display scale (1.0 handheld,
1.5 docked at 1080p) is applied by the projection, so the same layout is crisp in both modes. Text
is rasterized at the real pixel size; image requests are sized in framebuffer pixels
(`logical * ui::displayScale()`).

## Files

| File | What |
| --- | --- |
| `types.hpp` | `Vec2`, `Color` (`Color::hex(0x8B6CFF)`, `withAlpha`, `lerp`, `lighter/darker`), `Rect`, `Insets`, `Affine`, `ImageFit` |
| `renderer.hpp` | `Renderer`: batched GLES3 2D renderer, `Gradient`, `ImageStyle` |
| `texture.hpp` | `Texture` (GL texture, `shared_ptr`-owned), `RenderTarget` (FBO for transitions / group opacity) |
| `text.hpp` | `TextStyle`, `TextOptions`, `TextLayout`, `text::layout/measure/lineHeight/capHeight` |
| `image.hpp` | `ImageCache`, `ImageRequest`, `ImageRef` / `ImagePtr` (async loading, LRU, disk cache) |
| `pixels.hpp` | CPU `Pixmap` ops: `decodeImage` (JPEG/PNG/WebP), `resize`, `fitTo`, `blur`, `premultiply`, `dominantColor` |
| `icons.hpp` | `Icon` enum + `drawIcon()` vector icon set (SDF primitives, no textures) |

## Renderer

One uber-shader + one dynamic VBO. Every primitive is a quad carrying its own SDF shape parameters,
so shapes, text and images batch together; up to 8 textures are bound per batch. Batches break only
on clip changes, render-target changes, 9+ distinct textures, or a full buffer. AA is computed from
screen-space derivatives, so everything stays crisp under scale animations. Blending is
premultiplied; API colors are straight alpha.

```cpp
gfx::Renderer& r = gfx::Renderer::instance();   // owned by ui::Root; widgets get it in draw(r)

r.fillRect(rect, color);                         // no AA, fastest
r.fillRect(rect, gfx::Gradient::vertical(top, bottom));   // scrims
r.fillRoundRect(rect, radius, color or gradient);
r.strokeRoundRect(rect, radius, width, color);  // inner border
r.drawShadow(rect, radius, blur, color, offset, spread);  // soft gaussian-like shadow / glow
r.fillCircle(c, radius, color);  r.strokeCircle(c, radius, width, color);
r.drawLine(a, b, width, color);                  // round caps
r.fillTriangle(center, size, rotation, color, cornerRadius);
r.drawArc(center, radius, width, startAngle, sweep, color);  // radians, 0 = 3 o'clock, clockwise
r.fillShimmer(rect, radius, baseColor, phase01);              // skeleton placeholder

r.drawImage(tex, rect, {.fit = gfx::ImageFit::Cover, .radius = 12, .tint = Color::white(alpha)});
r.drawTexture(tex, rect, uvRect, tint, radius);
r.drawText(*layout, topLeft, color);             // or r.drawText("str", pos, style, color, options)

// State stacks (balanced push/pop):
r.pushClip(rect);  r.popClip();                  // nested scissor (intersection)
r.pushAlpha(a);    r.popAlpha();                 // multiplies
r.pushTranslate(dx, dy); r.pushScale(s, pivot); r.pushTransform(affine); r.popTransform();
r.pushTarget(renderTarget, clear, clearColor); r.popTarget();   // offscreen rendering
r.isVisible(localRect);                          // culling helper
```

Frame bracket (done by `ui::Root`): `beginFrame(display)` -> draw -> `endFrame()`. `beginFrame`
never trusts state from the previous frame: it binds the default framebuffer and re-applies all
GL state (`resetState()`).

### External GL (mpv render API)

```cpp
auto ext = r.beginExternal();      // flush() + bind VAO 0; ext.fbo / ext.width / ext.height / ext.flipY
// mpv_opengl_fbo fbo{int(ext.fbo), ext.width, ext.height, 0};
// int flip = ext.flipY ? 0 : 1;   // flip only for the default framebuffer
mpv_render_context_render(ctx, params);
r.endExternal();                   // == r.resetState()
```

| Call | Semantics |
| --- | --- |
| `flush()` | Submits the pending batch (one draw call). Mid-frame safe. |
| `beginExternal()` | `flush()`, binds VAO 0 (foreign code can only edit the default VAO) and returns the current target: `fbo` (0 = screen or a transition `RenderTarget`), pixel `width`/`height`, `flipY` (true for offscreen targets, whose rows are stored top-down in texture space). |
| `endExternal()` / `resetState()` / `restoreState()` | Re-applies everything the renderer relies on: program, VAO + vertex layout, VBO/IBO, unbinds `GL_PIXEL_UNPACK/PACK_BUFFER`, resets `GL_UNPACK_*` (row length, skips, alignment 4), blend on with `ONE, ONE_MINUS_SRC_ALPHA` + `FUNC_ADD`, color mask, depth/stencil/cull/polygon offset/alpha-to-coverage/rasterizer discard off, depth mask off, sampler objects 0-7 unbound, scissor on with the current clip, framebuffer + viewport + projection of the current target, and forgets the texture-unit cache. |
| `forgetTexture(id)` | Called by `Texture`'s destructor (not by user code): flushes quads that still sample `id` and drops it from the unit cache, since GL recycles ids. |

Rules: no `Renderer` draw calls between `beginExternal` and `endExternal`; the foreign renderer
must leave `ext.fbo` bound or not care (we rebind). Offscreen targets: never sample a
`RenderTarget` while it is the current target (`pushTarget` unbinds it from all units).
`App::glProcAddress` is the `get_proc_address` for `mpv_opengl_init_params`.

## Text

Bundled Inter (Regular, Medium, SemiBold, Bold, InterDisplay Bold for titles) in `romfs:/fonts`
(OFL, license next to the fonts). Missing glyphs fall back to the Nintendo shared fonts
(`plGetSharedFontByType`: Standard, Chinese Simplified/Ext/Traditional, Korean, Nintendo Ext).
Shaping uses HarfBuzz (kerning, ligatures; forced LTR). Glyphs are rasterized by FreeType (light
hinting, 4 subpixel x-positions) into shared 1024x1024 R8 atlas pages. New glyphs are written to a
CPU copy of the page and uploaded as one dirty rectangle per page before the next draw call.

```cpp
gfx::TextStyle st{.size = 20, .weight = gfx::FontWeight::SemiBold, .lineHeight = 1.25f, .tracking = 0};
gfx::TextOptions opt{.maxWidth = 300, .maxLines = 2, .align = gfx::TextAlign::Left, .ellipsis = true};
gfx::TextLayoutPtr l = gfx::text::layout("Hello", st, opt);  // cached; cheap to call every frame
l->width; l->height; l->lineCount; l->truncated; l->firstBaseline;
gfx::Vec2 size = gfx::text::measure("Hello", st, opt);
float capH = gfx::text::capHeight(st);  // for optical vertical centering
```

- `maxLines = 1` (default) is single line; with `maxWidth > 0` it truncates with an ellipsis.
- `maxLines = 0` (unlimited) or `n > 1` wraps at spaces / CJK boundaries within `maxWidth`.
- Layout width is the widest line; `align` aligns lines relative to each other. Callers position
  the block (e.g. `ui::Label` centers it in its frame).
- Use the type scale in `ui/theme.hpp` (`theme::type::title2`, `body`, `caption`, ...).

## Images

```cpp
gfx::ImageRequest req;
req.url = "https://server/Items/<id>/Images/Primary?maxHeight=450&tag=<tag>";  // or romfs:/ sdmc:/
req.headers = {{"Authorization", auth}};
req.width = int(148 * ui::displayScale());   // decode target in framebuffer px (0 = source size)
req.height = int(222 * ui::displayScale());
req.fit = gfx::ImageFit::Cover;              // decides the decode size (never upscales)
req.blur = 0;                                // gaussian sigma (backdrops: decode small + blur)
req.cacheKey = "";                           // optional stable identity (default: url)

gfx::ImagePtr img = gfx::ImageCache::instance().load(req);   // shared per identical request
if (const gfx::Texture* t = img->texture()) r.drawImage(*t, rect, {.radius = 12});  // touches LRU
img->state();          // Idle / Loading / Ready / Failed
img->dominantColor();  // placeholder / ambient tint (valid when ready)
img->readyTime();      // for fade-ins (ui::ImageView does this for you)
img->onSettled([](gfx::ImageRef& r) { ... });
gfx::ImageCache::instance().fromPixmap("key", pixmap);   // CPU-generated images
gfx::ImageCache::instance().trimMemory();                // e.g. before playback
```

Pipeline: memory -> disk cache (`sdmc:/switch/karafin/cache/img/<x>/<hash>.bin`, original bytes
behind a 24-byte header with length + checksum, written via `.tmp` + rename; entries that fail
validation, including the old headerless format, are deleted and refetched; kept under
`ui.image_disk_cache_mb`, default 256, by a running size estimate saved in `img/state`: the
oldest-first trim scans every file only when the estimate goes over budget or the last scan is
over a day old) -> HTTP -> worker decode (libjpeg-turbo with DCT
downscaling, libwebp with scaled decoding, libpng streamed row by row (interlaced files pass by
pass) and area-averaged straight to the display size, stb_image for GIF/BMP at full size) -> premultiply
-> area-average downscale -> optional blur (on a copy shrunk by up to sigma, max 4x, with sigma
reduced to match; the GPU's bilinear filter upscales it when drawn) -> main-thread upload.
Large decodes share a 48 MiB process-wide budget (they queue rather than all holding full-size
buffers at once); GIF/BMP and lossless WebP (whose format needs the whole image in memory) are
capped at 3072x3072, JPEG / lossy WebP / PNG at
4096x4096. Decoded images waiting for upload are capped at 32 MiB: workers wait before decoding
(giving up after 3 s, on cancellation or at `shutdown()`, leaving the image Idle to reload when
drawn), and decoded results nobody references or draws any more are dropped before upload.
At most 6 loads in flight (4 in applet mode), most recently
drawn first; queued loads nobody references any more are skipped, and HTTP fetches that only the
cache still references (e.g. a recycled grid cell) are cancelled after ~0.5 s. Uploads go into
textures allocated empty, in bands of rows (<= 512 KiB per `glTexSubImage2D`), limited per frame to
2 MiB / 4 completed images / 2 ms of main-thread time (`setUploadBudget`; at least one band per
frame); an image becomes Ready when its last band is in, so a 1080p backdrop spreads over a few
frames instead of stalling one. Network / 5xx
failures are retried (up to 3 times, growing backoff) when the image is drawn again. Textures not
drawn in the current frame are evicted when over `ui.image_cache_mb` (default 192 MiB, 64 MiB in
applet mode); evicted refs reload transparently from the disk cache the next time `texture()` is
called. A CPU boost is held (acquired/released on a worker) while loads are active and for 3 s
after. No mipmaps: request images at the size they
are drawn (`logical * ui::displayScale()`); `ui::ImageView` does this automatically.

Threading: the entry map, LRU and textures are main-thread only; workers and the http thread only
see copies of the request and post results back with `main_thread::post` to the (never
destroyed) cache, which drops them after `shutdown()` or if the entry was reloaded meanwhile.

## Icons

`gfx::drawIcon(r, gfx::Icon::Play, rect, color, weight)`. Available: Play, Pause, Stop, SkipNext,
SkipPrevious, FastForward, Rewind, SeekForward/SeekBack (circular "10"), Download, Check, Close,
Plus, Minus, Chevron{Left,Right,Up,Down}, Back, Subtitles, Audio, Settings, Search, Home, Library,
Movie, Tv, User, Server, Refresh, Trash, Info, Warning, Star, Heart, More, Menu, List, Lock, Wifi,
Filter, Clock, Exit, Speed. Strokes overlap at joints, so prefer opaque colors.

## Known limitations

- No bidi: RTL scripts are shaped LTR. No color emoji.
- Rotated clips use the axis-aligned bounds; there is no rounded-rect clip (draw rounded images with
  `ImageStyle::radius` instead).
- Text under a non-identity scale (e.g. a focused card at 1.08x) is bilinear-scaled from the atlas
  and slightly softer than at rest.
- Display scale changes (dock/undock) clear the layout cache and the glyph atlas; a
  `TextLayoutPtr` kept across the change still draws (at its old raster size) - re-request layouts
  every frame instead of caching them in widgets (`text::layout` is cached and cheap).
