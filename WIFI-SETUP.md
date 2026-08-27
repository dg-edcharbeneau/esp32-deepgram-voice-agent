# Wi-Fi and API key setup

> **Status: A and C confirmed on hardware 2026-08-26**, moving a working board
> onto a phone hotspot: held BOOT to forget the saved network, then joined the
> portal and picked the new one. B (menuconfig seed) and D (wipe everything) are
> still described from the code as written and have not been walked. Narrow this
> note as you go rather than deleting it.
>
> Hotspot note earned the hard way: an iPhone defaults its hotspot to 5 GHz on
> recent models, and the S3 has no 5 GHz radio, so the network never appears in
> the scan. Turn on **Maximize Compatibility** first.

Credentials are chosen at runtime. Nothing here needs a toolchain unless you
want one.

## Which path do you want?

| Situation | Go to |
|---|---|
| Brand-new board, or one that has been wiped | [A. Setup portal](#a-setup-portal) |
| Moving a working device to a different network | [C. Forget and re-provision](#c-forget-and-re-provision) |
| Wrong password saved, device will not connect | nothing to do — it offers the portal by itself, see [What happens on its own](#what-happens-on-its-own) |
| Bench development, same network every time | [B. Menuconfig seed](#b-menuconfig-seed) |
| Handing the board to someone else | [D. Wipe everything](#d-wipe-everything) |
| Setting or replacing the Deepgram API key | [A. Setup portal](#a-setup-portal) — it asks for the key too |
| Device says `check api key` | the key was rejected; re-enter it via [C](#c-forget-and-re-provision) |

Before anything else, two limits that explain most failures:

- **2.4 GHz only.** The ESP32-S3 has no 5 GHz radio. A network that is 5 GHz-only
  will not appear in the scan and cannot be joined. On a router that presents one
  name for both bands this usually still works, but band-steering can make it
  flaky.
- **WPA2-PSK and open networks only.** No WPA3-only APs, no WPA2-Enterprise, so
  no corporate or campus networks that want a username as well as a password.

## A. Setup portal

The normal path. Nothing to install.

1. **Power on.** With no saved network the ring shows a QR code with
   `dg-agent-XXXX` underneath — `XXXX` comes from the board's MAC, so two boards
   on one bench are distinguishable. The serial log says:

   ```
   I (…) main: no network configured, starting setup portal
   I (…) wifi_prov: portal up: join "dg-agent-A1B2", then browse to http://192.168.4.1/
   ```

2. **Join the device's network.** Point a phone camera at the QR and it offers to
   join `dg-agent-A1B2` directly, passphrase included — nothing to type.

   **The setup network is WPA2, not open.** It used to be open; it is encrypted
   now because the portal carries your Deepgram API key, which unlike a Wi-Fi
   password works from anywhere and bills to you. If you cannot scan the QR, the
   nine-character passphrase is on the panel under the network name. It is
   random per boot and uses no ambiguous characters — no `0`/`O`, no `1`/`l`/`I`.

3. **The setup page should open by itself.** The device answers every DNS query
   with its own address and redirects every unknown URL to the portal, which is
   the signal both iOS and Android use to pop a sign-in sheet. If it does not
   appear, browse to `http://192.168.4.1/`.

4. **Pick your network and enter the password.** The list is sorted as scanned,
   with signal strength beside each name and `open` marked where there is no
   password. For a hidden network choose **Other / hidden network...** and type
   the name — a hidden AP broadcasts no name, so it can never appear in a scan.

5. **Enter the Deepgram API key.** Create one at
   [console.deepgram.com](https://console.deepgram.com). Paste it; **Show key**
   reveals what you pasted, which is worth a glance because a truncated paste
   fails identically to a wrong key.

   **Blank means keep.** If the device already has a key the page says so, and
   leaving the field empty keeps it. That is safe for the key specifically
   because an empty key is never valid — unlike an empty Wi-Fi password, which
   legitimately means an open network. So re-provisioning Wi-Fi never costs you
   the key.

   The key is only checked for obvious damage — spaces, control characters. Its
   real test is the first connection, because the device has no route to Deepgram
   while it is running the portal.

6. **Save and connect.** The page confirms, the device reboots, the setup network
   disappears. On the way back up you should see `got ip …` and then hear the
   greeting. **The greeting is the proof** — it is the first thing that requires
   the key to have been accepted.

   If the key was wrong the panel reads `check api key` and the device **stops
   trying** rather than reconnecting forever. That is deliberate: retrying a
   rejected key never succeeds, and a device that says `error` and keeps
   reconnecting looks like a network problem.

Credentials and the key are stored in NVS and survive reflashing the app. They
live in separate namespaces, which is why forgetting the network leaves the key
alone.

## B. Menuconfig seed

For a board that never leaves your desk.

```
idf.py menuconfig  →  Deepgram Agent Device  →  Wi-Fi SSID / Wi-Fi password
                                            →  Deepgram API key
```

> **NVS wins.** These are a *first-boot seed*, used only when nothing has been
> saved yet. Once the device has been provisioned — through the portal, or by an
> earlier boot that consumed this seed — the saved network takes precedence and
> **editing these values and reflashing appears to do nothing at all.**
>
> This is the single most confusing thing about the setup. To make the seed take
> effect again, forget the saved network first (path C or D).
>
> **The API key follows the same rule**, with one difference that matters:
> forgetting the network (path C) does *not* erase the key, so only a full wipe
> (path D) brings the menuconfig key back.

Leave both empty to go straight to the portal on first boot, which is what you
want for any device that leaves the bench.

You can tell which one was used from the log:

```
I (…) wifi_creds: using menuconfig seed "MyNetwork" (nothing saved yet)
I (…) wifi_creds: saved network "MyNetwork"        ← this one came from NVS
```

And the same for the key, which is reported by length and never printed:

```
I (…) api_key: using the menuconfig seed (nothing saved yet)
I (…) api_key: using the saved key (40 characters)  ← this one came from NVS
```

## C. Forget and re-provision

**Hold the BOOT button for three seconds.** The ring shows `forgetting wi-fi`,
the device reboots, and comes back up in the setup portal.

```
W (…) boot_btn: BOOT held 3000 ms -- forgetting the saved network
I (…) wifi_creds: credentials erased (ESP_OK)
```

**This is also how you replace the API key**, since the portal is where the key
is entered. The key itself is *not* erased — only the network is — so the portal
will offer to keep it, and you can either leave the field blank or paste a new
one over it.

The cost is that changing only the key still means re-picking the network. There
is no way to reach the portal without forgetting the network first; that is a
known gap rather than an oversight.

BOOT is the button on GPIO 0; the other one is RESET, wired to the chip's enable
line, and it cannot do anything but reset. A short click of BOOT starts or stops
the agent session, exactly like tapping the screen.

> **Do not hold BOOT while pressing RESET.** GPIO 0 is a strapping pin: held low
> through a reset it puts the chip into USB download mode and the firmware never
> runs. That is not a Wi-Fi reset, and the screen will simply stay dark. Press
> RESET on its own — or power-cycle — to get out of it.
>
> The forget gesture is a press *after* the device is running.

## D. Wipe everything

Erases credentials, the API key, saved voice, volume, and Wi-Fi calibration
data:

```sh
idf.py erase-flash && idf.py flash
```

To clear only the stored settings without re-flashing 8 MB of application:

```sh
esptool.py erase_region 0x9000 0x6000
```

`0x9000`/`0x6000` are the NVS partition's offset and size from
[partitions.csv](partitions.csv); check there before trusting them if the flash
layout has changed.

## What happens on its own

You rarely need path C, because an unreachable network resolves itself:

1. The device tries the saved network, retrying up to `CONFIG_WIFI_MAX_RETRY`
   times (default 8). Each attempt logs a reason code.
2. When the budget runs out — or after 30 seconds either way — it gives up and
   raises the setup portal, **keeping the saved credentials**. A router that is
   merely rebooting must not cost you your password.
3. If nobody joins the portal for five minutes, the device reboots and tries the
   saved network again.

So a device that loses its network for ten minutes recovers by itself; one that
has genuinely moved waits for you with a portal open.

## Troubleshooting

**The setup network does not appear.**
The device only raises it when it has no working network. If it has credentials
that still work, it will use them — hold BOOT for three seconds to force the
portal.

**Joined `dg-agent-XXXX`, but no page opened.**
Browse to `http://192.168.4.1/` directly. If that works, the page is fine and
only the automatic pop-up failed — usually a private-relay or secure-DNS setting
on the phone. If it does not load at all, check for
`wifi_prov: portal up:` in the log.

**The scan list is empty or the page says `scan failed`.**
Use **Other / hidden network...** and type the name. Check the log for
`wifi_prov: scan found N networks`. Zero found on a bench full of Wi-Fi points at
the radio, not the portal.

**Saved, but it never connects.**

```
W (…) wifi: disconnected (reason 15), retry 1/8
E (…) wifi: giving up after 8 attempts (last reason 15)
```

The reason code is the useful part:

| Reason | Meaning |
|---|---|
| 15 (`4WAY_HANDSHAKE_TIMEOUT`) | wrong password, nearly always |
| 201 (`NO_AP_FOUND`) | wrong name, out of range, or a 5 GHz-only network |
| 2 (`AUTH_EXPIRE`) / 4 (`ASSOC_EXPIRE`) | weak signal, or the AP dropped us |

**It connects but the agent never speaks.**
Wi-Fi is fine — `got ip` proves it. That is a Deepgram session problem; see the
README and [agent-edge-host-header-404.md](agent-edge-host-header-404.md).

**Changed the SSID in menuconfig and nothing happened.**
Expected. See the callout in [path B](#b-menuconfig-seed): the saved network
wins. Forget it first.

## Where this lives in the code

| File | Role |
|---|---|
| [main/wifi_creds.c](main/wifi_creds.c) | NVS storage, and the seed precedence rule |
| [main/wifi_sta.c](main/wifi_sta.c) | Station bring-up, retry budget, `GOT_IP` wait |
| [main/wifi_prov.c](main/wifi_prov.c) | SoftAP, portal page, captive-portal DNS |
| [main/boot_button.c](main/boot_button.c) | GPIO 0: click to toggle, hold to forget |
| [main/main.c](main/main.c) | Boot flow and the fall-through to the portal |
