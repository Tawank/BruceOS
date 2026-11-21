# Messages

Text messages for boards with a cellular modem (the LilyGO T-Deck MAX's
A7682E): conversations, bubbles, and a compose bar.

```bash
python3 native_apps/tools/build_apps.py --target elf --idf-target esp32s3 --app messages
```

Copy `messages_esp32s3.elf` to `/apps/`. `messages <number>` opens that
conversation; the phone app's menu does this with "Send message".

## Tests

Host unit tests compile the app sources against fakes of the SDK calls
(`test/fakes.c`) and run under AddressSanitizer:

```bash
make -C native_apps/examples/messages/test
```

## Screens

- **Conversations**: *New message* (pick a contact or type a number), then
  one row per number, newest first, with the last message and its time. A
  dot marks unread messages.
- **Conversation**: messages as bubbles, received on the left and sent on
  the right, newest at the bottom; the compose bar shows the draft and how
  much of one message it uses (160 characters of plain ASCII, otherwise 70).

## Controls

| Action              | Keyboard             | Touch                   |
|---------------------|----------------------|-------------------------|
| Open                | Enter                | row                     |
| Move in the list    | Up / Down            |                         |
| Type                | keys                 | compose bar (keyboard)  |
| Delete a character  | Backspace            |                         |
| Send                | Enter                | send button             |
| Scroll a chat       | Up / Down            |                         |
| Back                | Back                 | header                  |
| Options             | Menu                 | hold a row              |
| Exit                | Back in the list     | menu > Exit             |

The options menu deletes a conversation or exits.

## How messages arrive

A received message stays on the SIM, and Core shows a notification. When
the app opens, and while it runs, it reads the SIM, saves the messages, and
only then deletes them from the SIM, so nothing is lost while it is closed.
Each part of a long message shows as its own message.

## Data

`/appdata/phone/messages.txt` (`utc,i|o,u|r,number,text`, oldest first, up
to 120 messages, text with `\n` and `\\` escaped). Names come from the phone
app's `/appdata/phone/contacts.txt`. These are absolute paths, so the app
needs the `storage` permission, and it does not follow the phone app's data
when App Settings moves it to the SD card.
