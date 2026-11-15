# Maps

Slippy map viewer for BruceOS. Works offline from tiles on the SD card and,
when Wi-Fi is known, downloads missing tiles and keeps them on the SD card.

```bash
python3 native_apps/tools/build_apps.py --target elf --idf-target esp32s3 --app maps
```

## Tests

Host unit tests compile the app sources against fakes of the SDK calls
(`test/fakes.c`) and run under AddressSanitizer:

```bash
make -C native_apps/examples/maps/test
```

## Controls

| Action      | Keyboard           | Encoder                       | Touch |
|-------------|--------------------|-------------------------------|-------|
| Pan         | arrows             | click to switch wheel to pan  | drag  |
| Zoom        | `+` / `-`          | wheel                         |       |
| Menu        | Enter              | long press                    |       |
| Exit        | Back               | menu > Exit                   |       |

The menu has *Go to coordinates*, *Online tiles* on/off and zoom.

## Tiles

Tiles are 256x256 PNGs in the usual `z/x/y` layout:

```
/sdcard/maps/tiles/<z>/<x>/<y>.png
```

Any tool that exports an OSM-style tile folder works for offline use.
Downloaded tiles go to the same folder, so everything viewed once also
works offline later.

## Settings

Stored with `app_config` in the app's own config file:

| Key            | Default                                           |
|----------------|---------------------------------------------------|
| `view.lat_e6`  | last centre, degrees x 10^6                       |
| `view.lon_e6`  | last centre, degrees x 10^6                       |
| `view.zoom`    | last zoom                                         |
| `tiles.online` | `true`                                            |
| `tiles.dir`    | `/sdcard/maps/tiles`                              |
| `tiles.url`    | `https://tile.openstreetmap.org/{z}/{x}/{y}.png`  |

The default tile server is fine for personal use, but its
[usage policy](https://operations.osmfoundation.org/policies/tiles/) forbids
bulk downloading and heavy app traffic. Point `tiles.url` at your own server
or a provider with an API key before distributing the app.

Map data © OpenStreetMap contributors.
