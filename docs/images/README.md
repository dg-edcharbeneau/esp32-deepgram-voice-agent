# Images

| File | What it is | Used by |
|---|---|---|
| `device-orb.png` | The board mid-reply, orb face lit and labelled `speaking`. | The README header. |
| `portal.png` | The captive-portal setup page, as a phone sees it after joining the device's AP. | Reference; not embedded. |

`portal.png` is a real device screenshot rather than the `host/portal.sh`
preview, which is why it shows Android's "Sign in to dg-agent-B1A1" sheet around
the page. Both password fields are empty in it on purpose — the page is
photographed before anything is typed, so nothing in this directory contains a
credential. Keep it that way if you retake either shot.

`host/portal.sh` renders the page with no device attached if you only need the
layout.

Keep files under ~1 MB each; a repo that clones slowly because of screenshots is
its own kind of unfinished.
