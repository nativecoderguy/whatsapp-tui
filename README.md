# WhatsApp TUI

A C terminal interface that follows your active Omarchy theme. Two quiet panels,
thin dividers, conversation search, message bubbles, and keyboard navigation.

This Git project is also an installable Omarchy bar plugin: its optional
**WhatsApp** button opens the TUI in your themed terminal. The C app runs as a
separate process; Omarchy's shell remains responsible for the status bar.

The interface and QR renderer are C (ncursesw, json-c, libqrencode). A separate
Node.js process uses [Baileys](https://github.com/WhiskeySockets/Baileys) to
connect to WhatsApp's linked device protocol. This is an unofficial client.

## Run

```sh
whatsapp-tui                 # in your current terminal
whatsapp-tui-launch          # open a themed terminal window
whatsapp-tui --demo          # sample conversations, no connection
```

On your phone, open **WhatsApp → Linked devices → Link a device**, then scan the
QR. It refreshes automatically. Once linked, your session is reused on restart.
Chat history sync depends on what your phone supplies; it can take a little time.
The QR view tells you the required terminal dimensions if the window is too small.

Find **WhatsApp TUI** in the desktop application launcher.

## Controls

| Key | Action |
| --- | --- |
| Tab | Switch between chat list and composer |
| ↑ / ↓, j / k | Select a chat while the chat list has focus |
| / | Search conversations; Enter selects the first match |
| Enter | Focus composer, or send when composing |
| Esc | Return to chat list / close a dialog |
| Ctrl+N | Open a chat by international phone number |
| PgUp / PgDn | Scroll messages |
| Ctrl+U | Clear the current draft or search |
| V | Open the latest photo in the selected chat (chat list focus) |
| Ctrl+V | Paste a clipboard image into the selected chat |
| Ctrl+X | Remove the attached photo, keeping its caption |
| F1 | Show help |
| Ctrl+Q | Quit |

Mouse clicks select chats and focus the composer. The wheel scrolls the
conversation. Drafts are kept per chat during the running session. Failed sends
preserve the draft; accepted sends clear it. The single line composer folds
line breaks in bracketed paste into spaces and requires Enter to send.

## Photos and image paste

**Foot works with the existing launcher.** Photos use its Sixel support. Kitty
and Ghostty use the Kitty graphics protocol. The app detects the terminal and
keeps photo controls, captions, and borders in your active Omarchy colors.
Use a direct terminal session; multiplexers default to the external viewer.

Photos appear **inline in the chat timeline**, with their captions underneath.
Visible previews download automatically and are cached in memory; scrolling to
older photos loads their previews on demand. Incoming photos stay on the left,
and outgoing photos stay within their message bubbles on the right. Previews fit
the chat pane and resize with the terminal. At the edge of the viewport, scroll
until the whole preview fits to display it without overlapping the composer.
Click an inline photo to open the larger viewer.

Click a photo in the conversation, or press **V** with the chat list focused.
The photo viewer supports **← / →** to browse older/newer photos, **Esc** to
return, and **O** to open the original in `imv` (or your default image viewer).
Images are downloaded on demand and cached locally. A resize rerenders the
photo at the new dimensions. Terminals without supported graphics use the
external viewer. If detection needs an override:

```sh
WHATSAPP_TUI_GRAPHICS=sixel whatsapp-tui    # Foot
WHATSAPP_TUI_GRAPHICS=kitty whatsapp-tui    # Kitty or Ghostty
WHATSAPP_TUI_GRAPHICS=external whatsapp-tui # use an external viewer
```

To send a screenshot or copied image, select a chat and press **Ctrl+V**.
The app reads the Wayland image clipboard with `wl-paste`, stages the photo,
and opens a preview. Press **Esc** or **Enter** to return to the composer,
type an optional caption, then press **Enter** to send. **Ctrl+X** removes the
attachment. A failed send keeps both the photo and caption. Paste alone never
sends anything. Use **Ctrl+V**, since terminal shortcuts such as Ctrl+Shift+V
and Shift+Insert normally paste text instead of passing image data to the app.

Clipboard PNG, JPEG, WebP, and BMP images are supported and normalized to PNG.
Photos are limited to 20 MB. Clipboard preview works in `--demo` too, with no
WhatsApp connection or sends. Older photos cached by the original text only
version may lack download metadata until they sync again. View once photos
remain for viewing on your phone.

## Theme

The app reads `$XDG_STATE_HOME/omarchy/current/theme/colors.toml` (by default
`~/.local/state/omarchy/current/theme/colors.toml`) once a second. Background,
foreground, accent, panels, selection, muted text, and errors all come from
that file. The older `~/.config/omarchy/current/theme/colors.toml` location is
also supported. There is no built in Omarchy palette. Outside Omarchy, the app
uses terminal defaults and ANSI colors. QR codes use black and white for scanning.

The terminal window inherits your existing terminal font and configuration.
Terminal color support determines whether colors are exact or approximated
to the nearest available palette color.

## Build

Requires a C compiler, make, pkg-config, ncurses, json-c, qrencode, and Node.js 20+.
Image previews and paste also require `imagemagick` and `wl-clipboard`; both
are already available on this installation. `imv` is used for external viewing.
On Arch, the library packages are `ncurses json-c qrencode`.

```sh
git clone https://github.com/nativecoderguy/whatsapp-tui.git
cd whatsapp-tui
make deps                    # install the locked bridge dependencies
make                         # build the C interface
make install                 # install the app and Omarchy TUI launcher
make install-plugin          # install and enable the optional Omarchy bar button
```

The install target puts the binary and bridge under `~/.local/lib/whatsapp-tui`,
installs `whatsapp-tui` and `whatsapp-tui-launch` in `~/.local/bin`, and creates
an Omarchy TUI desktop launcher. `make install-plugin` adds this Git checkout's
`osman.whatsapp-tui` bar button from its published GitHub remote. Run it after
`make install`. The button uses the foreground, font, and tooltip styling of
the active Omarchy bar.

`WHATSAPP_TUI_BRIDGE` can point to another `index.mjs` if you relocate the bridge.
The default binary locates its installed bridge beside the `build` directory.

## Local data and scope

Pairing credentials and a plaintext history cache live in
`$XDG_STATE_HOME/whatsapp-tui` (default `~/.local/state/whatsapp-tui`). New session
files are owner readable only. Only one connected instance can run at a time;
previews can run separately. Bridge errors go to `bridge.log` in that directory.

This version sends and receives text and photos in direct and group chats.
Other media appear as labels; video, audio, calls, reactions, and editing are
not implemented. Downloaded photos and their metadata are in `media/`, and staged
clipboard images are in `attachments/` inside the local state directory. Resetting
the session clears these directories as well as pairing and chat history.
The bridge keeps the newest 500 messages per chat; the UI displays up to 400
messages from the selected chat and retains up to 20,000 messages in memory.
Closing the app keeps its device linked. To unlink remotely, use Linked devices
on your phone. `whatsapp-tui --reset-session` asks you to type `RESET`, then removes
this app's local credentials and history so you can pair again.
