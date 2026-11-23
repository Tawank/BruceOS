# Internet Radio

Internet radio for BruceOS, a native ELF app with an LVGL screen. Named
`netradio` to leave room for radios on other hardware (FM modules).

```bash
python3 native_apps/tools/build_apps.py --target elf --idf-target esp32s3 --app netradio
```

Copy `native_apps/examples/netradio_esp32s3.elf` to `/apps` on the device. The
firmware must be built once first: the app compiles against its LVGL
headers (`native_apps/lvgl.cmake`).

## Using it

- Pick a station to play it. The top shows the station, what it is doing
  (Connecting, Buffering, Playing, or why it stopped), the volume and a level
  bar.
- The bar at the bottom: play/stop, search, volume down, volume up, and save
  (`+`) or remove the station playing.
- Search asks for a name and looks it up in the free
  [Radio Browser](https://www.radio-browser.info/) directory, most played
  first. Pick a result to listen, `+` to keep it. Back returns to the saved
  stations, and from there leaves the app (which stops playback; switch apps
  instead to keep listening).

## Stations file

Saved stations are an M3U file on the SD card,
`/sdcard/netradio/stations.m3u`, written with four stations the first time. It
can be edited by hand:

```
#EXTM3U
#EXTINF:-1 codec="mp3",SomaFM Groove Salad
https://ice1.somafm.com/groovesalad-128-mp3
#EXTINF:-1,Any station
http://example.com/stream.aac
```

`codec` (mp3, aac, ogg, flac) tells the player the format; without it the
URL's extension decides, else MP3. Search results carry it from the
directory.

## How it plays

`audio__url_play()` in Core streams the URL over Core's HTTP client (Wi-Fi
or mobile data, redirects followed) into the same decoder as files: MP3
always, AAC/OGG/FLAC when their codec library is in `/lib`. About 4 s are
buffered ahead. HLS-only stations are left out of search results, since
they are playlists of segments rather than one stream. Station titles sent
in the stream (ICY metadata) are not shown.

Settings (app config): `stations.file`, and `directory.url` (default
`https://all.api.radio-browser.info`).
