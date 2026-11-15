# `src/player/`: video player and subtitle manager

Namespace `karafin::player`. Depends on `core`, `gfx`, `ui`, `jellyfin`, and `downloads` (through
`offline.hpp`, compiled against `downloads/manager.hpp` when present). **Main thread only.**

## Public API (`player.hpp`)

```cpp
#include "player/player.hpp"

player::PlayRequest req;
req.itemId = item.id;
req.startPositionTicks = std::nullopt;  // nullopt = resume from UserData; 0 = start over
req.audioStreamIndex = std::nullopt;    // Jellyfin MediaStream index, nullopt = auto (player.audio_lang)
req.subtitleStreamIndex = std::nullopt; // Jellyfin index, -1 = off, nullopt = auto (player.sub_mode)
req.mediaSourceId = "";                 // "" = server default
req.offline = false;                    // true = play the completed download (no network needed)
player::play(req);                      // pushes the player screen (Zoom transition)
```

```cpp
player::shutdown();   // app quit: first thing in onStop (main thread), before ui::Root::onStop,
                      // DownloadManager::shutdown and http::Client::stop
```

* The caller asks "Resume or start over". The player only applies `startPositionTicks`.
* `play()` may be called from any thread; it hops to the main thread.
* If a player is already on top, it is replaced.
* Online playback needs `jellyfin::currentClient()` to be signed in.
* Offline playback finds the completed download by item id (any server).
* Pressing B with the OSD hidden, reaching the end, or a fatal error pops the screen. Playback
  is reported as stopped, or the local progress is saved for offline items.
* `shutdown(timeoutMs = 2000)` (app quit) stops mpv, saves offline progress or sends the final
  Stopped report and waits for the answer (Stopped reports of players closed just before are
  re-sent the same way). A report that doesn't get through in time goes to
  `jellyfin::ReportQueue` and is sent on the next launch. It then releases the platform locks and
  the download throttle, frees the render context and waits for mpv to finish, all within the
  same budget. Safe to call without a player; the screens destroyed afterwards report nothing.

## Settings keys

| Key | Type | Default | Meaning |
| --- | --- | --- | --- |
| `player.audio_lang` | string | `""` | Preferred audio language (ISO 639-2, e.g. `jpn`). `""` = file/server default |
| `player.sub_lang` | string | `"eng"` | Preferred subtitle language. Also the default in "Download subtitles" |
| `player.sub_mode` | string | `"default"` | `default`: subtitles in `sub_lang` when the audio is in another language, otherwise forced tracks, otherwise tracks flagged default in `sub_lang`. `always`: always a full `sub_lang` track if there is one. `forced`: only forced tracks (in `sub_lang` or the audio language). `off` |
| `player.sub_scale` | float | `1.0` | Subtitle size (0.3 to 3.0). The Options menu writes it too |
| `player.sub_pos` | int | `100` | Subtitle vertical position (mpv `sub-pos`, 100 = bottom). The Options menu writes it too |
| `player.sub_bg` | bool | `false` | Dark box behind subtitles. Read when the player starts |
| `player.autoplay_next` | bool | `true` | Count down and play the next episode automatically |
| `player.skip_segments` | string | `"ask"` | Intro/recap/preview/ad/credits segments: `ask` shows a Skip button, `auto` skips them with a toast, `off` ignores them |
| `player.hwdec` | bool | `true` | Hardware decoding (`nvtegra-copy`; software fallback is automatic) |
| `player.seek_step` | int | `10` | Seconds per D-pad step (3 to 120). L/R jump 3 steps |

The player also reads `http.verify_tls` (core) for TLS verification of the stream.

Picking a track by hand sets a session preference that also applies to the next episode. For
example, choosing Japanese audio and English subtitles carries over to the following episodes.

## Controls

| Input | Action |
| --- | --- |
| A | Play/pause. Also: commit a seek preview, press the visible Skip button, or play the up-next episode |
| B | Cancel the seek preview. Otherwise hide the up-next card, then hide the OSD, then exit (stops immediately, no confirmation) |
| D-pad ←/→ (or stick) | Seek preview by `seek_step`. Holding accelerates to 3x, 6x, then 12x. Committed 0.65 s after release, or with A |
| ZL / ZR | Previous / next chapter. Without chapters: −/+ 3 × `seek_step` |
| L / R | −/+ 3 × `seek_step` |
| Y | Audio & Subtitles sheet: audio track, Off, subtitle tracks, Download subtitles, Check for downloaded subtitle |
| X | Options sheet: speed (0.5x to 2x), subtitle delay (±0.1 s / ±1 s), subtitle size, subtitle position, aspect ratio & zoom, chapters, playback info |
| − | Toggle the playback info overlay (play method, container, codec, resolution and fps, decoder, bitrates, dropped frames, cache) |
| ↑ / ↓ | Show the OSD |
| + | App quit confirmation (ui default) |
| Touch | Tap toggles the OSD and activates the on-screen buttons (back, ±seek, play/pause, next, Audio & Subtitles, Options, Skip, up-next card). Tap or drag on the seek bar to scrub, with a preview |
| Swipe vertically on the left / right | Adjust handheld screen brightness / playback volume, with a vertical level indicator on the same side. Gestures starting on buttons or the seek bar are excluded. Volume ranges 0-200% (mpv `volume-max`; above 100% the audio is amplified, marked on the indicator); brightness 5-100% |

Up-next uses a 10-second countdown, including at EOF for short episodes or late seeks. Automatic
credits skipping also uses this card rather than bypassing the timer. Pausing, buffering,
seeking, recovery, loss of focus and open menus freeze the timer. A / tapping the card plays now;
B / tapping Hide cancels autoplay through EOF, unless the viewer seeks back out of the end zone.
With `player.autoplay_next=false`, the card has no countdown and only plays on request.
The timer continues across EOF, where mpv's automatic keep-open pause does not stop it.
Brightness and automatic-brightness control are restored on HOME, exit, or screen hide.
The player consumes SDL touch events through `Screen::onTouchEvent`, rather than polling SDL's
finger list alongside the widget gesture handler. Motion follows the original contact despite
the Switch driver's motion-slot IDs; duplicate downs are ignored and invalid coordinates cancel
the gesture. Open menus, hidden screens and focus loss cancel in-progress drags.

The upper-left controls show the item's transparent Jellyfin Logo (or inherited series logo).
While loading, if unavailable, or during offline playback, the normal text title remains.
Below the title, tags show how the stream is played (Direct Play / Direct Stream / Transcoding /
Downloaded) and what mpv is decoding: resolution, video codec with bit depth, and audio codec
with channel layout (`media_tags.hpp`). They are re-read once a second while the OSD is visible,
so audio track changes and transcode fallbacks show up.

## Files

| File | Contents |
| --- | --- |
| `player.hpp/.cpp` | public `PlayRequest` + `play()` |
| `mpv.hpp/.cpp` | `Mpv`: core options, OpenGL render context (`App::glProcAddress`), wakeup and render-update callbacks posted to the main thread, non-blocking event drain, async commands, `mpv_node` helpers. The core is destroyed on a reaper thread |
| `player_screen.hpp/.cpp` | `PlayerScreen`: item and PlaybackInfo resolution, direct play with transcode retry, mpv events, reporting, platform locks, input, skip segments, up-next |
| `player_osd.cpp` | OSD drawing (top bar, seek bar with buffered range and chapter marks, trickplay preview, controls, spinner, skip button, up-next card, info overlay) and touch |
| `player_menus.cpp` | track, options, speed, delay, size, position, aspect, and chapter sheets. Remote subtitle search and download (language picker, results sheet with loading state) |
| `tracks.hpp/.cpp` | `TrackManager` (the subtitle manager): Jellyfin MediaStreams merged with mpv's track-list, preferred-language auto-selection, Jellyfin index ↔ mpv id mapping |
| `languages.hpp/.cpp` | ISO 639 table (B/T/2-letter variants), names for the language picker |
| `sub_fonts.hpp/.cpp` | libass font sets in `cache/subfonts/<set>`: Inter plus the Nintendo shared font for Japanese, Korean, and Chinese |
| `offline.hpp/.cpp` | bridge to `downloads::DownloadManager` (offline item, next offline episode, local progress, streaming throttle) |
| `swipe.hpp`, `up_next.hpp` | host-tested gesture classification/level bounds and cancellable countdown timer |

## How playback works

1. **Item.** `client->item(id)` provides the title, chapters, trickplay, runtime and the resume
   position. Media segments and the next episode are fetched in parallel. The next episode is
   the following non-virtual episode of the season, or the first episode of the next season.
2. **Plan.** `playbackPlan()` returns DirectPlay/DirectStream (static URL) or Transcode (HLS).
   Tracks are auto-selected. A transcode whose tracks differ from the choice is requested again
   with explicit `AudioStreamIndex`/`SubtitleStreamIndex`.
3. **mpv.** `loadfile <url> replace start=<s>,aid=<n>,sid=<n|no>`. Embedded tracks use
   `PlaybackPlan::mpvTrackId()`. External Jellyfin subtitles are added lazily with
   `sub-add <url> select`; offline subtitle files are all added up front.
   * If direct play fails before or while loading, the player retries once with direct play
     and direct stream disabled (a transcode).
   * If the stream breaks after playback started (an mpv error, or EOF more than 20 s and 3%
     before the end, which is what a dropped connection looks like), the player shows
     "Reconnecting", waits up to 30 s for the network, and reloads at the last position: the
     same URL and play session for direct play, a new PlaybackInfo for transcodes. Up to 3
     attempts; 90 s of normal playback resets the count. Then a normal error (or, for an early
     EOF, a normal end).
4. **Reporting.**
   * `reportPlaybackStart` on file load.
   * Progress every 10 s, and on Pause/Unpause, seeks, track changes and speed changes.
   * `reportPlaybackStopped` with the position on exit, next episode, or stream restart.
     Unreachable servers queue it automatically.
   * Offline: `saveLocalProgress` every 10 s and on pause. On exit it is called with
     `queueServerReport = true`.
5. **Track changes while transcoding.** Switching to an audio track or image subtitle that isn't
   in the HLS output restarts the transcode at the current position with the new indices.
6. **Remote subtitles.**
   1. Language picker, then `searchRemoteSubtitles`. Results show provider, format, rating,
      downloads, HI, forced, machine-translated, and "Exact match" for hash matches.
   2. `downloadRemoteSubtitleAndRefresh`, then `sub-add` the new stream URL and select it, without
      restarting playback.
   3. If the server hasn't indexed the file yet, a toast says so. The player re-checks after 6,
      15 and 30 s, and the menu offers "Check for downloaded subtitle".
   4. Permission errors (cached policy check first, then 403), missing providers and empty
      results get explicit messages.

### mpv configuration (highlights)

* Output and decoding:
  * `vo=libmpv`, `ao=hos`
  * `hwdec=nvtegra-copy`, falling back to `nvtegra` and then `auto-copy` when an option value is
    rejected. With `player.hwdec=false`, `hwdec=no`. All of it is in `configureHwdec()` in
    `mpv.cpp`.
  * `hwdec-codecs=h264,hevc,vp8,vp9,mpeg1/2,mpeg4,vc1,wmv3`
  * `vd-lavc-threads=3`, `vd-lavc-skiploopfilter=nonref`, `vd-lavc-dr=no` (direct rendering
    needs the render API's advanced control, which stays off because the main thread makes
    synchronous mpv calls)
* Rendering and sync:
  * bilinear scalers, no dithering or debanding (GPU budget)
  * `video-sync=audio`, `video-timing-offset=0` (rendering never blocks, so frames are taken at
    their display time), `opengl-glfinish=yes` (avoids sporadic GPU crashes on this GL stack)
  * `hr-seek=yes`, `keep-open=yes`, `ytdl=no`, `config=no`
  * The video is rendered every frame into the renderer's current target (FBO 0 flipped, or a
    transition target). Blending and scissoring are disabled for mpv;
    `Renderer::endExternal()` restores the UI state afterwards.
* Cache:
  * `demuxer-max-bytes=80MiB`, `back=16MiB`, `readahead=60s`
  * in applet mode: 24 MiB / 4 MiB / 20 s
* Network:
  * `network-timeout=20`, `user-agent=Karafin/<ver> (Nintendo Switch)`
  * `http-header-fields` with the Jellyfin `Authorization` header, passed as an mpv node list so
    its commas survive
  * `tls-ca-file`: the user CA at `sdmc:/switch/karafin/cacert.pem` if it exists, otherwise the
    romfs bundle. `tls-verify` follows `http.verify_tls`.
* Subtitles: `sub-font-provider=none`, `sub-fonts-dir`/`sub-font` per subtitle language,
  `sub-ass-override=scale`, `sub-scale`, `sub-pos`, `sub-margin-y=34`, outline 2.6 with a soft
  black shadow. `player.sub_bg`: `sub-back-color=#B0000000` with 4 px padding. mpv 0.36 then
  draws a box behind each line by itself; newer mpv needs `sub-border-style=background-box`,
  which is set too (0.36 rejects it).

### Platform

* `AwakeLock`: held while playing, loading, seeking or buffering. Released while paused, so the
  console can sleep.
* `CpuBoost`: held only while starting, seeking or buffering, and during software decoding. The
  boost mode lowers the GPU clock, which the video renderer needs.
* Losing focus (HOME menu, sleep, overlay applets) pauses playback. Coming back within 30 s
  resumes it; after longer (sleep) it stays paused.
* Streaming playback calls `DownloadManager::setThrottled(true)`. Exit and offline playback
  reset it.
* `ImageCache::trimMemory()` runs when the player appears.

## Known limitations

* Not tested on a device.
* This libmpv build has no nvtegra GL interop, so hardware-decoded frames are copied back to RAM
  (`nvtegra-copy`) and uploaded. If that hwdec is unusable, mpv decodes in software; the info
  overlay shows which.
* libass has no font provider, so there is no glyph fallback between fonts. The subtitle font is
  chosen by the track's language:
  * Inter for Latin, Cyrillic and Greek; the Nintendo shared font for jpn, kor and chi
  * Chinese fonts are skipped in applet mode
  * CJK text in a track tagged with another language shows boxes unless the file embeds fonts
* Trickplay needs Jellyfin 10.9+ with trickplay images generated. Media segments need 10.10+.
  Neither is available offline.
* The direct-play → transcode retry triggers only on mpv load errors (`END_FILE` error). A file
  that opens but decodes badly is not detected.
