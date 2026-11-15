# ui: widgets, focus, animation, screens

Namespace `karafin::ui`. Depends on `core` and `gfx`. **Main thread only.** Include `ui/ui.hpp`.
All sizes are logical pixels (1280x720); take colors, spacing, radii, type and motion from
`ui/theme.hpp` so the app stays coherent.

## Host and frame loop

`ui::Root` is the `AppDelegate` (see `core/main.cpp`):

```cpp
app.run(std::make_unique<ui::Root>([] { return std::make_unique<SplashScreen>(); }));
```

Each frame: SDL events -> input dispatch -> `timeline()` -> image cache -> layout -> `update(dt)`
-> focus validation -> render (screen stack, modals with dim backdrop, hint bar, toasts).
Layout is incremental: `setFrame` size changes and `setNeedsLayout()` flag the path to the root
and clean subtrees are skipped, so the per-frame layout pass is nearly free. A display scale
change (dock/undock) re-measures the whole tree once.

Shorthands: `ui::root()`, `ui::nav()`, `ui::focus()`, `ui::timeline()`, `ui::present(modal)`,
`ui::toast(text, kind, icon, seconds)`, `ui::displayScale()`, `ui::now()`.
Setting `ui.show_stats` (bool) shows a performance overlay: fps, worst frame, CPU frame time,
GPU time (timer queries), time blocked in swap, slow-frame count, texture uploads, text shaping,
draw calls / quads / offscreen passes, image memory and a 2-second frame-time graph (green = one
vsync, amber = two, red = more; white = CPU part). Counters come from `core/perf.hpp`; with
`app.log_level` = `debug`, frames over 25 ms are logged with the same breakdown.

## Screens and navigation

```cpp
class DetailScreen : public ui::Screen {
public:
    explicit DetailScreen(std::string id) {
        auto* play = add<ui::Button>("Play", gfx::Icon::Play, ui::ButtonStyle::Primary);
        play->setFrame({56, 400, 160, 46});            // or use Box layouts
        play->onActivate = [this] { ... };
        setRememberedFocus(play);                     // initial focus
        addShortcut(ui::Key::Y, [this] { toggleWatched(); });
        setHints({{ui::Key::Y, "Watched"}});
        api.fetch(id, guard([this](auto&& result) { populate(result); }));  // safe async
    }
    void onShow() override {}       // became top (after transition) - refresh here
    void onHide() override {}       // covered / popping
    bool onBack() override { return false; }  // true = consume B
};

ui::nav().push(std::make_unique<DetailScreen>(id));                     // Transition::Push (slide)
ui::nav().push(std::make_unique<PlayerScreen>(...), ui::Transition::Zoom);
ui::nav().pop();                                                          // default for unhandled B
ui::nav().replace(std::make_unique<X>(), ui::Transition::Fade);
ui::nav().resetTo(std::make_unique<HomeScreen>(), ui::Transition::Fade);  // after login
ui::nav().popTo(screen); ui::nav().popToRoot();
```

Transitions: `None`, `Push` (slide + fade, pop reverses), `Fade`, `Zoom` (scale + fade), `Up`.
Both screens render into offscreen targets during the transition (correct group opacity); the
outgoing screen is captured once on the first frame and then only composited. The targets are
kept between transitions (released after 3 s idle in applet mode). Pushing/popping a non-opaque screen animates
only that screen over the (static) one below.
`Screen::setOpaque(false)` lets the screen below show through (e.g. a player OSD over video);
`setShowsHintBar(false)` hides the hint bar (player). Unhandled `+` opens a quit confirmation.

`sound.hpp/.cpp` provides short, synthesized navigation cues through an independent SDL audio
device. `Root` plays them for controller navigation, select/back, and touch taps.
`ui.navigation_sounds` (bool, default true; Settings > Interface) takes effect immediately.
`Screen::setNavigationSounds(false)` suppresses cues for a screen. The audio device is opened
lazily and closed on UI shutdown. On Switch, SDL audio and mpv's `ao=hos` share the process's
single audren renderer, so `PlayerScreen` calls `sound::suspend()` before mpv starts (closing the
device) and `sound::resume(...)` on destruction; cues, including in player menus, stay silent
until every mpv core has finished tearing down.

## Views

`ui::View` is the retained tree node: owns children (`add<T>(args...)` returns `T*`), has a `frame`
(relative to the parent's content origin), `layout()` to position children, `draw(r)` /
`drawOverlay(r)` in local coordinates, and `update(dt)` (call the base). Presentation properties
are animatable and do not affect layout: `alpha`, `scale` (around `setPivot`), `offset`.
`setZ()` orders siblings (focused cards raise themselves). `setBackground(color, radius)`,
`setClipsChildren(true)`, `setVisible(false)`.

Async safety: wrap callbacks with `guard(fn)` (runs only while the view is alive) or hold a
`ui::Ref<T>` (weak handle). `lifetime()` is the alive token for `Timeline` owners.

## Layout

```cpp
auto* col = add<ui::Box>(ui::Axis::Vertical);
col->setPadding(Insets::all(24)).setSpacing(12).setAlign(ui::Align::Start).setJustify(ui::Justify::Start);
col->add<ui::Label>("Title", ui::theme::type::title2);
auto* row = col->add<ui::Box>(ui::Axis::Horizontal);
row->add<ui::Spacer>(0, 1);                 // flexible gap
child->setPreferredSize(w, h);              // -1 = automatic (measure())
child->setFlex(1); child->setMargin({...});
```

Custom views override `measure(available)` (natural size) and `layout()`.

## Focus and input

- Buttons are `ui::Key` (physical Nintendo buttons: every SDL controller button is resolved
  through its mapping back to the switch driver's raw button, so the positional SDL layout never
  leaks through): A confirm (right), B back (bottom), X top, Y left, L/R tabs, ZL/ZR page,
  + menu/quit, - secondary. Joysticks without a mapping are read from raw SDL_JOY* events.
  Directions come from the D-pad or left stick (dominant axis, hysteresis) and repeat with
  acceleration (0.35 s delay, 110 ms -> 45 ms).
- Routing: focused view -> ancestors -> scope (top modal, else top screen) via `onInput(e)`;
  `Screen::addShortcut(key, fn)` runs after that. Unconsumed directions move focus; A calls
  `activate()` / `onActivate` on the focused view.
- `setFocusable(true)`, `onFocus()/onBlur()`, `onFocusWithin(descendant)` (ancestors),
  `requestFocus()`, `ui::focus().setFocus(view)`.
- Containers steer D-pad moves with `navigateFocus(from, dir)` (return a view, `focusBlocked()`, or
  null for spatial search). `setRememberFocus(true)` restores the last focused descendant when
  focus re-enters the container (rows, grids, menus, screens and modals do this).
- Hints: `view->setHints({{Key::Y, "Mark watched"}})` - collected from the focused chain plus the
  scope; "Select" (when `onActivate` is set) and "Back"/"Close" are added automatically.
- Touch: tap focuses + activates (`onTap(local)` to customize; `onTouchDown/Cancel` for press
  feedback); drags scroll the nearest `ScrollView` on that axis with rubber-banding and inertia.
- A screen can consume SDL touch events with `Screen::onTouchEvent`; `Root` routes them there
  before widget gestures, only when no modal is open. The player uses this for taps, seeking and
  brightness/volume swipes, keeping one gesture owner rather than also polling SDL fingers.

## Animation

```cpp
ui::Animated<float> v{0.0f};                         // also Animated<Vec2>, Animated<Color>
v.springTo(1.0f, ui::theme::motion::focus);          // interruptible, keeps velocity
v.tweenTo(0.0f, 0.25f, ui::ease::outCubic);          // fixed duration
v.update(dt); v.value(); v.animating();
view->scale.springTo(1.08f, ui::theme::motion::focus);   // View presentation props auto-update

ui::timeline().animate(0.4f, ui::ease::emphasized, [=](float t) {...}, [=] {...}, lifetime());
ui::timeline().after(1.5f, guard([this] {...}));
```

Springs are solved analytically (`SpringSpec{response, damping}`; presets `theme::motion::focus`,
`standard`, `snappy`, `scroll`, `gentle`, `bouncy`). Easings: `linear`, `in/out/inOut Quad/Cubic`,
`outQuart`, `outQuint`, `outExpo`, `inOutSine`, `outBack`, `emphasized`, `cubicBezier(...)`.

## Widgets (`ui/widgets/`)

| Widget | Use |
| --- | --- |
| `Label(text, style, color)` | text; `setMaxLines(n)` wraps to the frame width, `setAlign`, `color.springTo` |
| `IconView(icon, color, size)` | vector icon |
| `ImageView` | `setUrl(url, headers)` / `setRequest(req)` / `setImage(ptr)`; shimmer while loading, fade-in, `setRadius`, `setFit`, `setFallbackIcon`, `onLoaded`; decode size auto = frame x display scale, re-requested when docking |
| `Button(label, icon, ButtonStyle)` | `Primary` / `Secondary` / `Ghost` / `Icon`; focus scale + white focus fill, press dip |
| `PosterCard(CardShape, showTitles)` | `Poster` 2:3 / `Wide` 16:9 / `Square`; `setImageUrl`, `setTitle`, `setSubtitle`, `setProgress(0..1 or -1)`, `setWatched`, `setBadge("3")`, `setPlaceholder(color, icon)`; focus scale-up, glow from the art's dominant color, focus ring, shine |
| `RowCarousel(title)` | titled horizontal row; add cards to `items()`; leading-edge spring scroll; per-row focus memory; `onNearEnd` for lazy paging |
| `ScrollView(axis)` | add to `content()` (a `Box`); spring scroll-into-view (`setAnchor`, `setRevealInsets`, `setRevealWholeChild`), touch drag/fling, ZL/ZR paging, `scrollTo`, `onScroll` |
| `Grid(cellSize, spacingX, spacingY)` | virtualized grid: `count`, `createCell`, `bindCell(view, index)`, `onNearEnd` -> `notifyAppended()`, `reload()`, `focusIndex(i)` |
| `ListMenu` | `setItems({ListItem{title, subtitle, detail, icon, checked, enabled, onSelect}}, focusIndex)`; gliding white highlight |
| `SideSheet::make(title, items, focusIndex, subtitle)` | right-hand sheet with a `ListMenu` (track selection); `present()` it |
| `Dialog(DialogSpec)` / `Dialog::confirm(...)` / `Dialog::alert(...)` | modal card with icon, title, wrapped message, buttons; B = `cancelIndex` |
| `ui::toast(text, ToastKind::Success, icon, seconds)` | stacked glass pills above the hint bar |
| `ProgressBar` | `setValue(v)` springs; `setBuffer`, `setIndeterminate` |
| `Spinner(size, color)` | rotating arc |
| `TabBar(titles)` | capsule with sliding pill; selection follows focus; `bindShoulderButtons(screen)` for L/R; `onSelect` |
| `Backdrop` | full-bleed hero art that crossfades on change, ambient tint from the dominant color, bottom/left scrims; pair with `ImageRequest::blur` |

Custom modals derive from `ui::Modal` (full-screen view; animate content with `appear()` 0..1;
`dismiss()`; `setDim`, `setDismissOnBack`, `onDismissed`).

Hint glyphs: `drawButtonGlyph(r, Key::A, center, size, fill, ink)` draws Switch-style button
glyphs anywhere (e.g. in the player OSD).

## Theme tokens (`ui/theme.hpp`)

- `theme::color::*`: `background`, `backgroundRaised`, `surface`, `surfaceHigh`, `surfaceHighest`,
  `skeleton`, `textPrimary/Secondary/Tertiary`, `textOnAccent`, `textOnLight`, `accent`,
  `accentBright`, `accentDeep`, `accentGlow`, `success`, `warning`, `danger`, `info`, `separator`,
  `stroke`, `focusRing`, `scrim`, `glassFill`, `buttonSecondary(Focused)`, `progressTrack`;
  `theme::accentGradient`.
- `theme::space::{xxs 4, xs 8, sm 12, md 16, lg 24, xl 32, xxl 48, xxxl 64}`.
- `theme::layout::{marginX 56, marginTop, hintBarHeight 44, rowSpacing, cardSpacing, focusOverscan}`.
- `theme::radius::{xs 4, sm 8, md 12, lg 16, xl 24, pill}`.
- `theme::type::{hero, largeTitle, title1, title2, title3, headline, body, callout, subhead, caption, footnote, overline, button}`.
- `theme::motion::{focus, standard, snappy, scroll, gentle, bouncy}` springs and `fast/normal/slow/screenTransition/imageFade` durations.
- `theme::card::*` (poster 148x222, wide 268x151, radius 12, focusScale 1.08), `theme::button::*`,
  `theme::shadowRest/shadowFocus`.
