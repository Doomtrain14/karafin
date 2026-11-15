# `src/downloads/`: offline downloads

Namespace `karafin::downloads`. Depends on `core` and `jellyfin` only (no gfx/ui).
Umbrella include: `#include "downloads/downloads.hpp"`. Everything goes through the singleton
`DownloadManager::instance()`.

## Files

| File | Contents |
| --- | --- |
| `types.hpp` | `State` (+ `stateName`, `canTransition`), `DownloadInfo`, `LocalSubtitle`, `GroupSummary`, `StorageStats`, `OfflineItem/Season/Series/Library`, `Event`/`EventType`, `EnqueueResult`, `makeKey()` |
| `manager.hpp` | `DownloadManager` (queue control, settings, snapshots, offline library, local progress, observers) and the RAII `Subscription` |
| `downloads.hpp` | umbrella include + `selfTest()` |
| `manager.cpp` | queue state, persistence, threads, events, enqueue |
| `job.cpp` | one transfer: probe, bundle assets, ranged download, finalize, retry/backoff |
| `library.cpp` | offline library grouping, metadata cache, local progress and report queueing |
| `detail/` | `Queue`/`Task` (pure state + JSON), `PartWriter` (buffered `.part` writes), `BandwidthLimiter`, helpers; `sdcard.*` is the only libnx-specific part (concatenation files, exFAT driver query) |

## Lifecycle (app host)

```cpp
// AppDelegate::onStart (after App started workers + HTTP):
downloads::DownloadManager::instance().start();
// AppDelegate::onStop (before the HTTP client stops):
downloads::DownloadManager::instance().shutdown();
```

`start()` loads `queue.json` and starts the threads. Unfinished downloads come back **Paused**.
With `downloads.auto_resume = true` they come back **Queued** instead, unless the user had paused
them. Any control call also starts the manager lazily. `shutdown()` stops the transfers (their
`.part` files are kept), saves the queue and joins the threads. Always call it from `onStop`: a
transfer that is still running when curl is torn down is undefined behaviour.

## Keys, states and snapshots

* A download is identified by `key = makeKey(serverId, itemId)` (`"<serverId>:<itemId>"`), and
  `DownloadInfo::key` carries it.
* States: `Queued → Probing → Downloading → Completed`.
  * `Paused` is reached from Queued or any active state, and `resume()` moves it back to Queued.
  * `WaitingForNetwork`: a network or server error, retried with backoff.
  * `Failed(error)`: `retry()` moves it back to Queued.
  * `isActive(state)` is true for Probing, Downloading and WaitingForNetwork.
* `DownloadInfo` holds the following. Snapshots are copies, so they are cheap and safe on the
  main thread.
  * `bytesDone`, `bytesTotal` and `progress()` (0..1)
  * `bytesPerSecond` (smoothed), `etaSeconds` (-1 when unknown)
  * `error`: the user-facing message for Failed and WaitingForNetwork
  * `attempt` and `retryInSeconds`, while WaitingForNetwork
  * display fields: `name`, `type`, `seriesName`, `seasonName`, `indexNumber`, `parentIndexNumber`, ...
  * local progress: `positionTicks` and `played`

## Detail screen: download button and inline state

```cpp
auto& dm = downloads::DownloadManager::instance();

// Show state (movie / episode):
if (auto d = dm.find(item)) {            // by BaseItem (serverId, else current client's server)
    switch (d->state) { /* Completed -> "Downloaded", Downloading -> d->progress(), Failed -> d->error ... */ }
}
// Series / season button:
downloads::GroupSummary g = dm.summary(item.serverId, item.id);   // item = series or season
// g.total, g.completed, g.active, g.queued, g.paused, g.failed, g.progress(), g.allCompleted()

// Download (movie, episode, season or whole series):
dm.enqueue(item, [this](downloads::EnqueueResult r) {             // main thread
    if (!r.ok()) toast(r.error);           // e.g. the "Allow media downloading" permission message
    else toast(std::format("Downloading {} item(s)", r.added));
});

// Live updates while the screen is open:
sub_ = downloads::Subscription([this](const downloads::Event& e) {
    if (e.info.itemId == item_.id || e.info.seriesId == item_.id || e.info.seasonId == item_.id) refreshDownloadButton();
});
```

* `enqueue` checks the user's download permission with a fresh `/Users/Me` call. When offline it
  falls back to the cached policy. A Season or Series expands to every non-virtual episode via
  `client->episodes()`.
* Items already queued or downloaded are skipped, and Failed ones are re-queued.
* `isDownloaded(itemId)` is a quick check for poster badges.
* `enqueue(client, item, cb)` takes an explicit client. The default uses `jellyfin::currentClient()`.

## Downloads screen

| Need | Call |
| --- | --- |
| "In progress" list | `dm.pending()`: queue order, everything not Completed |
| "Downloaded" list | `dm.completed()`: newest first, or `dm.loadLibrary(cb)` for grouped browsing |
| Pause / resume one | `dm.pause(key)`, `dm.resume(key)` |
| Pause / resume all | `dm.pauseAll()`, `dm.resumeAll()` |
| Cancel an unfinished download (deletes the partial file) | `dm.cancel(key)` |
| Delete a download (any state) | `dm.remove(key)`. `dm.removeGroup(serverId, seriesOrSeasonId)` deletes a whole series or season. |
| Delete watched | `dm.removeWatched()` returns how many were deleted |
| Retry | `dm.retry(key)`, `dm.retryFailed()` |
| Storage bar | `dm.storage()` → `downloadedBytes`, `partialBytes`, `reservedBytes` (still to download), `freeBytes`, `totalBytes` |
| Settings | `setConcurrency(1..4)`, `setAutoResume(bool)`, `setKeepScreenOn(bool)` (main thread, stored in settings) |

Events (`subscribe`/`Subscription`, always delivered on the **main thread**):

| Event | Meaning |
| --- | --- |
| `Added` | A new download was queued |
| `Updated` | The state, error or metadata changed |
| `Progress` | Bytes, speed or ETA changed. Throttled to about 4 per second per item. |
| `Removed` | The download was cancelled or deleted. `info` is the last snapshot. |

Bulk operations emit one event per item. A simple screen can re-read `pending()` on every event.
Events are buffered and delivered together once per frame, in order. If a newer `Progress` arrives
for a download before delivery, it replaces the earlier undelivered one.

The offline entry point from the server/login screen works without any client. Snapshots and
`library()` read only the local queue and files.

### Offline library

```cpp
dm.loadLibrary([this](downloads::OfflineLibrary lib) {   // built on workers(), delivered on main
    for (auto& m : lib.movies) addCard(m.info.name, m.posterPath);           // local JPEG paths
    for (auto& s : lib.series) {                                             // s.posterPath, s.backdropPath
        for (auto& season : s.seasons)                                       // sorted by season number
            for (auto& ep : season.episodes) addEpisode(ep);                 // sorted by episode number
    }
});
```

`OfflineItem` contains:

* `info`: a `DownloadInfo`
* `item`: a `jellyfin::BaseItem` parsed from the stored metadata, with `userData` set to the local
  progress. Overview, people, media streams, chapters and so on all work offline.
* `mediaPath`, `posterPath`, `backdropPath`, `seriesPosterPath`: each is `""` when not bundled
* `subtitles`: `LocalSubtitle` entries with an absolute `path`, `index`, `language`, `title`,
  `format`, `isDefault`, `isForced` and `isHearingImpaired`

`OfflineSeries::item` and `OfflineSeason::item` are parsed from the series and season JSON.
`library()` is the synchronous version. It reads each `metadata.json` once and then caches it.

## Player: playing a downloaded file

```cpp
auto off = dm.offlineItem(key);            // nullopt unless Completed (also: dm.find(item)->key)
mpv.loadfile(off->mediaPath);
for (auto& s : off->subtitles) mpv.subAdd(s.path, s.title, s.language);   // or `sub-files` option
mpv.seek(jellyfin::ticksToSeconds(off->positionTicks()));
// embedded tracks: off->item.mediaSources / mediaStreams (same data as online)

// Progress: every ~10 s, on pause and at the end
dm.saveLocalProgress(key, positionTicks);                  // still watching
dm.saveLocalProgress(key, positionTicks, /*finished*/true); // reached the end
auto next = dm.nextOfflineEpisode(key);                     // offline autoplay
```

* `saveLocalProgress` writes the position to `queue.json` and to the `UserData` in `metadata.json`.
  Watching 90% or more, or passing `finished`, marks the item played and clears the position.
* With `queueServerReport = true` (the default) it also queues `ReportQueue::enqueueStopped` (and
  `enqueuePlayed` the first time the item becomes played). The file writes run on `dl-io`. When
  the current client is the same server and user and is online, the queue is flushed once the
  saves have stopped for 3 s (1 s after an item becomes played), so a burst of saves gives one
  Stopped report. Pass `queueServerReport = true` only for the final save of a session (the
  player does). Otherwise the jellyfin module flushes on the next session validation.
* If the player plays a local file while online **and** reports to the server itself, pass
  `queueServerReport = false` so the item isn't reported twice.
* `setLocalPlayed(key, bool)` marks an item watched or unwatched from the Downloads screen.
* **Throttle**: when the player streams from the server (not for local files), call
  `dm.setThrottled(true)` when playback starts and `dm.setThrottled(false)` when it stops. Downloads
  are then capped at `downloads.throttle_kbps` KiB/s in total (default 1024, minimum 128).

## How a transfer works

1. **Probing**: resolve a `jellyfin::Client` for the item's server and user. That is the current
   client if it matches, otherwise one built from the `AccountStore` token. Then:
   * On the first attempt, `offlineMetadata` is written to `metadata.json`, and poster, backdrop,
     series poster and every external subtitle (`externalSubtitleDownloads`) are fetched.
   * `probeDownload` gives the size, ETag, Last-Modified and Accept-Ranges.
2. **Validation**: the resume offset is the `.part` size, but never more than `syncedBytes` (the
   last fsync'ed size, persisted in `queue.json`): after a crash or power loss the file can be
   longer than the data that really reached the card. An existing `.part` is discarded when the
   ETag, Last-Modified or size changed, or when the server doesn't do ranges. Then:
   * The free space must cover the remaining bytes, plus what the other running transfer still
     needs, plus 256 MiB, or the download fails with "Not enough free space... X needed, Y free".
3. **Downloading**: `GET /Items/{id}/Download` with `Range: bytes=N-` plus `If-Range` (the ETag
   when it is strong, otherwise the Last-Modified date; Jellyfin sends Last-Modified, no ETag).
   * Files over 4 GiB - 1 (or of unknown size) are created as a Horizon **concatenation file**
     (`FsCreateOption_BigFile`): an archive-bit folder of chunks that the file system and stdio
     treat as one file. That is how Horizon stores big files on FAT32 cards, so they download on
     FAT32 and exFAT alike. If the file system refuses, a plain file is used; if additionally the
     console has no exFAT driver (`fsIsExFatSupported`), the card must be FAT32 and the download
     fails up front with a message recommending exFAT.
   * Writes go into a 2 MiB buffer, with an fsync every 64 MiB or 20 s (which also advances
     `syncedBytes`).
   * A 200 response instead of 206 is detected from the response size. If nothing was flushed yet,
     the bytes received so far become the start of the file. Otherwise the `.part` is truncated
     and the download restarts from 0. A 206 must carry a Content-Range that starts at the requested
     offset (and has the expected total); a missing / mismatched Content-Range or ETag discards the
     `.part` and restarts from 0.
4. **Errors**:
   * Server unreachable (DNS, connect, timeout, dropped connection, 502/503/504 from a proxy) goes
     to **WaitingForNetwork** with backoff 2, 4, 8 ... 120 s and keeps waiting for as long as it
     takes. While the console has no network at all it polls every second and retries as soon
     as the connection is back (wake from sleep, Wi-Fi returns).
   * Other transient failures (5xx, TLS/protocol errors, early EOF) back off the same way but the
     download fails after 8 consecutive ones. An attempt that gained 1 MiB or more resets both
     counters.
   * Images and subtitles are optional: a server error leaves that file out instead of retrying.
     Failing to write one to the SD card is a storage error (logged as `storage write failed`)
     and fails the download like any other SD write error.
   * Failing immediately:
     * 400: "the server doesn't allow downloading this item" (`BaseItem::CanDownload` is false)
     * 401: "session expired, sign in again"
     * 403: the "Allow media downloading" message from `ApiError` (`ErrorContext::Download`)
     * 404: "no longer available"
     * SD write errors: ENOSPC gives "SD full"; EFBIG or crossing 4 GiB gives the FAT32 message
   * 416 deletes the `.part` and restarts from 0. More than 3 restarts in one job fail with
     "file keeps changing".
5. **Completed**: an existing final file is first renamed to `<file>.old`, then `<file>.part` is
   renamed into place and the backup deleted (restored if the rename fails; the `.part` is never
   deleted). The `download.json` manifest is written (one retry) before the task becomes
   Completed; if that fails the download fails with the SD error, and a retry finds the finished
   file and only redoes this step.

A core `platform::AwakeLock` is held while any job is Probing or Downloading, so the console
never auto-sleeps mid-download. It also blocks screen dimming when `downloads.keep_screen_on` is
true; `setKeepScreenOn` swaps the lock immediately if a download is running.

## Persistence layout

```
sdmc:/switch/karafin/downloads/
  queue.json                      {"version":1,"items":[{serverId,userId,itemId,state,bytesDone,...}]}
  <serverId>/<itemId>/
    media.<ext>                   original file (ext from Content-Disposition / path / container)
    media.<ext>.part              while unfinished
    metadata.json                 {"version":1,"serverId","userId","savedAt","item":{..},"series":{..}|null,"season":{..}|null}
    download.json                 manifest (same fields as a queue entry) - rebuilds queue.json if it is lost
    poster.jpg  backdrop.jpg  series_poster.jpg (episodes)
    subtitles/<index>.<lang>[.forced][.sdh].<fmt>
```

* `queue.json` is written atomically (`fs::writeFileAtomic`) on the `dl-io` thread. It is saved
  about 0.2 s after a state change and at most every 10 s while only the byte counts change.
* On load, sizes come from the files: `.part` sizes (capped at `syncedBytes`) for unfinished
  items. A Completed item whose media file is missing becomes Failed, and `retry()` downloads it
  again.
* A `queue.json` or `download.json` that doesn't parse is logged and moved to `<name>.corrupt`
  (replacing an older one) so it is kept but not parsed again.
* After `shutdown()` nothing starts the threads again except an explicit `start()` (screens torn
  down after `onStop` may still save progress); such late changes are written inline / by the
  destructor.

## Settings

| Key | Default | Meaning |
| --- | --- | --- |
| `downloads.concurrency` | `1` | parallel transfers (1..4) |
| `downloads.auto_resume` | `false` | resume interrupted downloads on launch |
| `downloads.keep_screen_on` | `true` | keep the screen from dimming while downloading |
| `downloads.throttle_kbps` | `1024` | KiB/s cap while `setThrottled(true)` (min 128) |

## Threads

* `dl-1` through `dl-4` run transfers with blocking `http::Client::perform` and `Call<T>::wait`. Only
  `concurrency` of them are started; `setConcurrency` adds threads when raised. Lowering it keeps the
  extra threads idle (at most `concurrency` transfers run at a time).
* `dl-io` handles queue saves, folder deletions and metadata patches.
* Event delivery, `enqueue` callbacks and `loadLibrary` callbacks run on the main thread.

## Self-test

`downloads::selfTest(&summary)` needs no network. It covers:

* the state machine table and Queue operations (FIFO, skip-running, pause/resume/retry, remove)
* the `queue.json` round trip, including restore rules with and without auto-resume and invalid documents
* the extension, Content-Range, backoff, ETag, played-threshold and UserData patch helpers
* the throttle bucket math
* the `.part` writer (buffering, resume offset, rebase, truncate) in `cacheDir()/dl-selftest`

Failures are logged under the `downloads-test` tag.

## Limitations

* Not tested on a device yet.
  * Concatenation files on the SD card (create, append, rename `.part` → final, mpv reading,
    delete) need verifying on hardware. If creation fails, FAT32 cards hit the write-error
    fallback at the 4 GiB boundary with the exFAT message (Horizon has no file-system type query).
* Original files only. Transcoded downloads and music are out of scope.
* Downloads can't run while the app is closed (a homebrew limit). They resume on the next launch.
* Local progress isn't merged with newer server-side progress. Queued Stopped reports overwrite the
  server's position (see the jellyfin README).
* A detail screen sees a series' download state only for episodes already in the queue.
  `summary()` doesn't know about episodes that were never enqueued.
