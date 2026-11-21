# Phone

Phone app for boards with a cellular modem (`cellular__is_available()`, the
LilyGO T-Deck MAX's A7682E): keypad, recent calls, contacts, the in-call
screen and USSD codes.

```bash
python3 native_apps/tools/build_apps.py --target elf --idf-target esp32s3 --app phone
```

Copy `phone_esp32s3.elf` to `/apps/`: an incoming call runs
`phone incoming`, and Core finds `/apps/phone.elf` or the build for its chip,
`/apps/phone_esp32s3.elf`. The app is single-instance
(`singleInstance` in `manifest.json`), so a call brings a running one to the
front instead of starting a second.

## Tests

Host unit tests compile the app sources against fakes of the SDK calls
(`test/fakes.c`) and run under AddressSanitizer:

```bash
make -C native_apps/examples/phone/test
```

## Screens

- **Keypad**: the number, the contact it belongs to, 3×4 keys, and the call
  button. With nothing typed the call button brings back the last number
  called. A number starting with `*` or `#` and ending with `#` is sent as
  a USSD code and the network's reply is shown.
- **Recents**: incoming, outgoing and missed calls with time and length;
  tap or Enter to call back. The app opens here after a missed call.
- **Contacts**: *New contact*, then contacts by name; tap or Enter to call.
- **In call**: name, number and timer, mute, the keypad for tones, volume
  and hang up; an incoming call shows answer and decline.

On e-paper the call timer redraws every 15 seconds instead of every second.

## Controls

| Action               | Keyboard                        | Touch                    |
|----------------------|---------------------------------|--------------------------|
| Type a number        | digits, `*`, `#`, `+`           | keys                     |
| Delete               | Backspace                       | ⌫ (hold to clear)        |
| Call / open          | Enter                           | call button, row         |
| Switch tab           | Left / Right                    | tab bar                  |
| Move in a list       | Up / Down                       | tap                      |
| Options              | Menu                            | hold a row               |
| Answer / decline     | Enter / Back                    | green / red              |
| Hang up              | Back                            | red                      |
| Volume, mute         | Up / Down, `m`                  | buttons                  |
| Exit                 | Back                            | menu > Exit              |

On the T-Deck keyboard the keypad keys work without Alt: the letter keys
that show 1-9, `*`, `#` and 0 type those.

The app does not need to stay open for calls: Core watches the modem and
starts it when one rings. The app draws inside the launcher's window frame (`dialog__draw_window()`), so the
top bar with the clock, battery and status icons is the launcher's; the
signal bars are the cellular status icon there.

The options menu calls, adds the number to contacts, edits or deletes a
contact or a call, clears recents, and turns the phone on or off. Opening
the app turns the phone on; it then stays on, also after a reboot, so calls
can come in.

## Data

Contacts and history are text files in the app's data folder
(`/appdata/phone`, or on the SD card when moved in App Settings):
`contacts.txt` (`number,name`) and `history.txt`
(`utc,seconds,i|o,result,number`, newest first, up to 100 calls). Calls the
modem logged while the app was closed are added when it opens.
