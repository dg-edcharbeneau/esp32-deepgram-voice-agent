# Images

The README embeds `device-orb.jpg` from here. These are the shots the docs want;
none of them can be generated from source, so they have to be taken by hand.

| File | What it is | How to get it |
|---|---|---|
| `device-orb.jpg` | The board mid-reply, orb face lit. **Used by the README.** | Talk to the device and photograph the panel while it is speaking. Landscape, the round panel filling most of the frame, no glare across the AMOLED. |
| `device-spectrum.jpg` | The spectrum face during agent speech. | Ask it to switch faces (`UI_DEFAULT_FACE` also works, but that needs a reflash), then photograph during a reply. |
| `portal.png` | The captive-portal setup page. | No device needed: `./host/portal.sh` opens it in a browser. Screenshot at phone width. |

A short GIF or MP4 of one full turn — greeting, question, reply, orb reacting —
would be better than `device-orb.jpg` and `device-spectrum.jpg` together. If you
add one, put it here and swap the README's `<img>` for it.

Keep files under ~1 MB each; a repo that clones slowly because of screenshots is
its own kind of unfinished.
