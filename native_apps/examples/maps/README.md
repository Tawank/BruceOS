# Maps

Slippy map viewer for BruceOS. Works offline from tiles on the SD card and,
when online, downloads missing tiles and keeps them on the SD card.

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

| Action | Keyboard  | 3 buttons / encoder          | Touch          |
|--------|-----------|------------------------------|----------------|
| Zoom   | `+` / `-` | Prev / Next                  | `+` / `-` buttons |
| Pan    | arrows    | Prev / Next after switching  | drag           |
| Mode   |           | hold Select                  |                |
| Menu   | Enter     | Select                       |                |
| Exit   | Back      | menu > Exit                  |                |

Holding Select, or the first menu item *Change to ...*, switches what
Prev/Next do: zoom, then up/down, then left/right. A short hint on the map
confirms the new mode, e.g. "Move up/down", and the zoom badge shows an
arrow icon while Prev/Next pan. The menu also has *Search*, *Directions*,
*All directions* and *End route* (while there is a route), *Clear pin*
(while a pin is shown), *Online tiles* on/off, *Exit* and *Back*. On touch
screens the +/- buttons appear after the first touch.

## 3D view

*3D view* in the menu tilts the map into a perspective looking ahead. The
arrows (or Q and E) turn it, and on a board with a motion sensor (the
T-Deck MAX) turning the device turns the view too, without GPS; it may take
a few seconds the first time while the sensor starts. Moving with a GPS fix
turns it to the way of travel.

## My location

On boards with a GPS receiver (`gps__is_available()`, e.g. the LilyGO T-Deck
MAX) GPS stays off until you ask for your location: the *My location* button
in the bottom right corner on touch screens, or *My location* in the menu.
That turns the receiver on, and the map centres on the first fix and keeps
following it until the map is panned; press it again to centre once more.
On e-paper the map stays still while the dot is in the middle half of the
screen, so only the dot redraws; when it leaves, the map jumps ahead in that
direction.
*Turn GPS off* in the menu powers it down again.

The position is a blue dot, and the current speed sits in a box at the bottom
left, above the scale bar. The button is grey without a fix, white with one,
and orange while the map follows. Until there is a fix, a line on the left
under the status shows the progress: *GPS 1/3: starting* (or *no data* when
the receiver stays silent), *GPS 2/3: time sync, N seen*, then *GPS 3/3: 2/4
locked, N seen* while it waits for four satellites. GPS needs the `gps`
permission.

## Search

The search box at the top of the map shows the pinned place's name, or
"Search" when there is no pin. Tap it on touch screens, or use *Search* in
the menu. It asks for a place or an address and sends it to
[Nominatim](https://nominatim.org/), preferring places near what is on
screen. The map stays usable while it runs; Back cancels it. Pick one of the
results (the distance from the screen centre is on the right) and the map
jumps there and drops a pin. Typing coordinates such as `50.0614, 19.9372`
jumps straight there without the network.

## Directions

*Directions* in the menu plans a route in three questions: *To* (the pinned
place, a search, or the map centre), *From* (*My location* on boards with
GPS, the map centre, or a search) and *Travel by* (car, bike or walk). The
route comes from the [OSRM](https://project-osrm.org/) servers FOSSGIS runs
at `routing.openstreetmap.de`, one per travel mode, and needs the network
only to plan it. It is drawn on the map (blue, or black on e-paper), with a
pin at the destination.

From *My location* the app navigates. The banner at the top shows the next
manoeuvre as an arrow, the distance to it and the instruction, e.g. "350 m,
Turn left onto Grodzka"; the line under it is the manoeuvre after that
("Then ..."), and the box at the bottom the distance and time left. The map
follows the position as with *My location*. More than 50 m off the route
for three fixes in a row plans a new route from where you are (at most
every 15 s), and within 25 m of the destination "You have arrived" ends it.
The distance is rounded (to 10 m, 50 m below 1 km, 100 m above) so the
banner, and an e-paper screen, change less often.

From anywhere else the whole route fits on screen and the banner shows the
steps one by one, starting with the first ("1/12"); tap the banner for the
next one. *All directions* lists every step with its length; picking one
shows it on the map. *End route*, or Back, removes the route.

## Tiles

Tiles are 256x256 PNGs in the usual `z/x/y` layout:

```
/sdcard/maps/tiles/<z>/<x>/<y>.png
```

Any tool that exports an OSM-style tile folder works for offline use.
Downloaded tiles go to the same folder, so everything viewed once also
works offline later. Downloads run in the background (at most 5 at once)
through `http__request_start()`, so the map stays responsive; tiles that
scroll off screen are cancelled. The core reuses each connection
(keep-alive), so a burst of tiles from one server pays for one TLS
handshake instead of one per tile. When offline, the app gets online in the
background (`net__connect_start()`: a saved Wi-Fi network, or mobile data;
at most every 30 s) and shows "Connecting" meanwhile; a connection made
elsewhere is picked up within a second. It needs no `wifi` permission.

On e-paper each tile is turned into black and white as it loads
(`main/ink.c`), because the panel would dither the pale map colours into
speckles: text, railways and road casings are black, roads are outlined
(except around labels, whose white halos would otherwise look like roads),
water is dotted, and everything else is white. The tiles on the SD card
stay in colour.

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
| `search.url`   | `https://nominatim.openstreetmap.org/search`      |
| `route.url`    | `https://routing.openstreetmap.de`                |

The default tile server is fine for personal use, but its
[usage policy](https://operations.osmfoundation.org/policies/tiles/) forbids
bulk downloading and heavy app traffic. Point `tiles.url` at your own server
or a provider with an API key before distributing the app. The same goes for
`search.url`: the public Nominatim allows one search a second and no
autocomplete, see its [usage policy](https://operations.osmfoundation.org/policies/nominatim/).
`route.url` is an OSRM server laid out like FOSSGIS's, with
`/routed-car`, `/routed-bike` and `/routed-foot` under it. FOSSGIS's
[usage policy](https://routing.openstreetmap.de/about.html) asks for a valid
user agent, at most one request a second, and no heavy use.

Map data © OpenStreetMap contributors.
