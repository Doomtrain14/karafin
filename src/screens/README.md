# `src/screens/`: application pages

Namespace `karafin::screens`. Depends on `core`, `gfx`, `ui`, `jellyfin`, `downloads` and
`player/player.hpp`. Main thread only. `core/main.cpp` starts at `SplashScreen` and hosts the
download manager lifecycle (`start()` in `onStart`, `shutdown()` in `onStop`).

## Navigation

```
Splash ── last session valid ──────────────▶ Main (online)
   │ ├─ 401 ───────────────▶ Login(server)
   │ ├─ unreachable ─▶ [Try again | Offline downloads ─▶ Main (offline) | Change server]
   │ └─ nothing saved ─────▶ ServerSelect ─▶ Login ─▶ Main (online)
Main (online):  Home · Libraries · Search · Downloads · Settings   (TabBar, L/R, B = back to tabs)
Main (offline): Downloads · Settings                               (+ menu = reconnect)
pushed on top:  Library grid, Detail (movie/episode), Series, Offline series, Player
```

`session.hpp` owns the transitions: `enterApp(client)`, `enterOffline(client)`, `restartSession()`,
`goToServerSelect()`, `goToLogin(serverId)`, `signOut()`. `enterApp` installs the client's 401
handler (back to sign-in) and connectivity handler (toasts). Every transition dismisses open
modals first: sheets and dialogs hold callbacks into the screens being destroyed.

B on the main screen: inside a section it returns to the tab bar, on another tab it goes to the
first tab, on the first tab it opens the quit confirmation.

## Files

| File | Contents |
| --- | --- |
| `splash_screen` | logo animation, session restore (asks for the profile code first when that profile has one; B goes to its sign-in page), applet-mode warning (`ui.applet_warning_hidden`), offline / error panel |
| `server_screen` | saved servers (X menu: sign in, remove), LAN discovery (re-scans every 8 s), add by address (swkbd), offline downloads entry |
| `login_screen` | remembered accounts (X menu: forget), public users, other user, password via swkbd, Quick Connect; profiles that ask for their lock code show a lock badge and ask before signing in (by any route) |
| `main_screen` | header (`Wordmark`, account chip), TabBar, lazily created `SectionView`s with fade/slide; `showAppMenu()` is the + menu (tabs, reconnect when offline, controls guide, quit) that browsing screens set as `onAppMenu` |
| `wordmark` | the "karafin" brand image (`romfs/wordmark.png`, `wordmark-ink.png` for white highlights) sized like Karastream's; main header, server page, + menu title and Settings > About |
| `home_section` | hero carousel + Continue Watching, Next Up, Recently Added per library, My Libraries; skeleton rows; quiet refresh on return; failed hero / Recently Added rows become inline errors with Try again |
| `libraries_section` | library tiles + Favorites; locked libraries show a lock (accent + "Locked" badge while locked, dimmed while unlocked); the X menu adds lock options (lock, unlock, lock again, remove lock) and Scan library after a confirmation (administrators) |
| `library_lock` | per-profile lock with a 4 or 6 step D-pad code (`lock.profiles` keyed `<serverId>/<userId>`: code as salted iterated SHA-256, locked libraries, ask-on-open, wrong-code count and lockout end; 1.0.4's global `lock.code` / `lock.libraries` move into the first profile signed in): gates opening locked libraries and, optionally, the profile itself; lists locked titles (cached until locks or the profile change) so Home, Search and library grids leave them out; per-library unlock until it is locked again, Lock now, a profile switch or restart; 5 wrong codes pause entry for 30 s, doubling per further 5 up to 5 min, kept across restarts |
| `library_screen` | sparse-paged virtual grid over `/Items`, sort / filter + genre (active ones shown as a chip by the count) / A-Z jump (keeps the filters) from the X menu; failed pages retry with backoff (3 s doubling, max 30 s) while visible; settings `ui.library_sort`, `ui.library_desc`; administrators get a Scan library button; an unlocked locked library gets a Lock again button; while titles are locked, grids that can mix libraries (people, favorites, collections, folders) list matching ids first, drop locked ones and page by id |
| `detail_screen` | movie / episode page: badges, Resume + From start (or Play), audio, subtitle and version pickers, favorite, watched, download, episode / cast / similar rows; a cut-off overview is focusable (A or tap opens the full text) |
| `series_screen` | series page: next-up play, seasons TabBar (L/R) or picker, season actions, episode rows, series / season download; a cut-off overview opens the full text with A or a tap |
| `search_section` | swkbd search (A on the field, or Search / Clear search in the X menu), debounced grouped results, partial-failure warning row with Try again, recent searches (`ui.recent_searches`) |
| `downloads_section` | storage bar, overall speed and time left while downloading, in-progress rows (pause / resume / retry / cancel, pause all, resume all), downloaded movies and shows, delete watched |
| `offline_series_screen` | downloaded episodes of one show by season |
| `settings_section` | Account, Playback, Subtitles, Downloads, Interface, About (keys owned by jellyfin / player / downloads / ui) |
| `common` | formatting, image requests, `bindCard`, `openItem`, `playItem` / `playAt` / `playSeries` / `playOffline` |
| `item_meta` | metadata and media chips (4K, HDR10, DV, codecs, audio, CC), track titles |
| `keyboard` | `showKeyboard` / `requestText` wrapper around libnx swkbd (text, URL, password) |
| `widgets/` | `ItemCard`, `HeroCarousel`, `RowStack`, `StatusView`, `ErrorRow`, `Skeleton(Row)`, `ActionButton`, `ActionRow`, `ChipRow`, `AvatarTile`, `BusyModal`, `QuickConnectModal`, `DownloadButton`, `EpisodeRow`, `PendingDownloadRow`, `OfflineItemRow`, `StorageBar`, `PinPad` (D-pad / touch-arrow code entry with lockout countdown), `OverviewText` (overview that opens the full text when cut off) |

## Conventions

* Every async call goes through `Call<T>::then(cb, token_)`; screens cancel their tokens in the
  destructor, so callbacks may capture `this`. Widget callbacks that rebuild their own parent
  are deferred with `timeline().after(0, ...)`.
* Playback: the caller decides resume vs. start over and always passes `startPositionTicks`
  (saved position or 0). `playItem` asks "Resume / Start over" when there is a saved position
  (cards' X menu Play, Continue Watching, episode rows, series Play); the detail page shows both choices as
  buttons instead. Played items start from 0. Track pickers pass Jellyfin stream indices
  (`nullopt` = player decides).
* Returning from the player: Home, Library, Detail and Series refresh user data in `onShow` /
  `onSectionShown`, and once more 2 s later (the player's stop report is sent asynchronously).
  Rows whose items are unchanged are rebound in place (`updateCardsInPlace`), so focus, scroll and
  loaded artwork stay put.
* Downloads: `DownloadButton` (item or season/series group) and the Downloads section subscribe
  to `DownloadManager` events with `downloads::Subscription` and update in place.
