# Blueprint BP-TD110 CUPS Driver for Raspberry Pi

---

## What This Is

A from-scratch CUPS raster filter (`raster-tspl.c`) that lets the Blueprint
BP-TD110 thermal label printer work on Raspberry Pi (ARM), shared via AirPrint
so any device on the network can print without installing Blueprint's driver.

**Why:** Blueprint only ships x86/x64 binaries. Raspberry Pi is ARM. Also,
Blueprint's Mac driver won't survive the next macOS update that drops Rosetta.
This solution puts the driver on the Pi permanently — Mac, iPhone, iPad all
print driverless via AirPrint.

**Printer:**
- Model: Blueprint BP-TD110
- Protocol: TSPL (Thermal/Shipping/Printer Language)
- USB: `usb://Blueprint/BP-TD110?serial=<your-serial>`
- 1284 Device ID: `MANUFACTURER:Blueprint;COMMAND SET:ESC/POS;MODEL:BP-TD110;`
  (printer reports ESC/POS but accepts TSPL — both protocols work simultaneously)
- Resolution: 203 DPI
- Label size tested: 100 x 150 mm

---

## Files in This Package

```
blueprint_driver_pi/
├── src/
│   └── raster-tspl.c                  <- CUPS filter (the main driver)
├── ppd/
│   └── Blueprint-BP-TD110.ppd         <- Printer description file
├── avahi/
│   └── blueprint-td110.avahi.service  <- Avahi static service (AirPrint, no TLS)
├── Makefile                           <- Build instructions
├── install.sh                         <- Full install script (run on Pi as sudo)
└── README.md                          <- This file
```

---

## How It Works

The driver is a standard CUPS raster filter written in C. CUPS calls it with
1-bit raster image data, and the filter converts that into TSPL `BITMAP`
commands sent directly to the printer over USB.

AirPrint advertisement is handled by Avahi (mDNS/Bonjour) rather than CUPS's
built-in mDNS. This is intentional — see Bug Fix #4 below for why.

---

## Installation (Fresh Pi Setup)

### Prerequisites

- Raspberry Pi running Raspberry Pi OS (or any Debian/Ubuntu ARM Linux)
- Blueprint BP-TD110 connected via USB
- Internet access to install packages

### Steps

**1. Copy files to the Pi:**
```bash
Clone this git to your pi
```

**2. Run the installer on the Pi:**
```bash
sudo bash install.sh
```

The installer handles: installing build dependencies, compiling the filter,
installing the PPD, configuring CUPS (network access, disabling TLS, disabling
CUPS's own Bonjour), and installing the Avahi service file.

**3. Add the printer queue:**

Find your printer's USB serial first:
```bash
lpinfo -v | grep Blueprint
```

Then add the queue (replace the serial with yours):
```bash
sudo lpadmin -p Blueprint-TD110 \
  -E \
  -v "usb://Blueprint/BP-TD110?serial=<your-serial>" \
  -P /usr/share/cups/model/Blueprint/Blueprint-BP-TD110.ppd.gz \
  -o printer-is-shared=true \
  -o media=w100h150 \
  -D "Blueprint BP-TD110 Label Printer" \
  -L "Raspberry Pi"
sudo cupsaccept Blueprint-TD110
sudo cupsenable Blueprint-TD110
sudo lpadmin -d Blueprint-TD110
```

**4. Open port 631 in the firewall:**
```bash
sudo ufw allow 631/tcp comment "CUPS printing"
sudo ufw reload
```

After this, the printer appears as an AirPrint printer on any device on the
same local network.

---

## Adding the Printer on Mac (No Driver Install Needed)

1. System Settings → Printers & Scanners → **+**
2. **Default tab** → look for **"Blueprint BP-TD110 Label Printer"**
   (auto-discovered via Bonjour/AirPrint)
3. **Use:** `AirPrint` or `Auto Select`
4. Click **Add**

If not in the Default tab, use the **IP tab:**
- Protocol: `IPP`
- Address: `<pi-ip>`
- Queue: `/printers/Blueprint-TD110`
- Use: `AirPrint` or `Auto Select`

**Do not use IPPS or auto-selected HTTPS** — the Pi has no TLS cert. If the
printer was previously added as `ipps://`, fix it:
```bash
lpadmin -p <printer-name> -v "ipp://<pi-ip>/printers/Blueprint-TD110"
```

**Paper size:** Shows as "Small Photo" in the AirPrint dialog — that is Apple's
name for 100×150mm. It is pre-selected as the default.

---

## Bugs Fixed During Development

### 1. All-black output — FIXED
**Cause:** CUPS delivers 1-bit raster in luminance convention (bit 1 = white),
even though the PPD requests CS_K. TSPL BITMAP expects the opposite (bit 1 =
print/black). Data needed bitwise inversion.
**Fix in `raster-tspl.c`:** 1-bit path now uses `~cup_row[b]` instead of
`memcpy`. 8-bit threshold changed from `> 127` to `< 128` for the same reason.

### 2. Old test jobs printing on every new job — FIXED
**Cause:** Multiple test jobs got stuck in CUPS spool during debugging.
**Fix:** Stopped CUPS, deleted all spool files, restarted CUPS.
If this happens again:
```bash
sudo systemctl stop cups cups.socket cups.path
sudo bash -c "rm -f /var/spool/cups/c* /var/spool/cups/d*"
sudo systemctl start cups
```
**Note:** The glob must run inside `sudo bash -c "..."`. Plain `sudo rm /var/spool/cups/c*`
won't work — the glob expands before sudo elevates, and a normal user can't
read that directory.

### 3. iOS printing slow (15-second delay before print) — FIXED
**Cause:** iOS always tries IPPS (encrypted IPP) before falling back to plain
IPP. CUPS had auto-generated self-signed certs, so the TLS handshake partially
completed before iOS rejected the untrusted cert — this full cycle repeated
~15 times with ~1 second each before iOS gave up and used plain IPP.
**What was tried:** Adding a self-signed cert with proper SANs — didn't help
because iOS still won't trust self-signed certs without per-device installation.
**Fix applied:**
- Deleted all certs from `/etc/cups/ssl/`
- Made the ssl directory immutable (`chattr +i`) so CUPS can't regenerate certs
- Removed `ServerCertificate` / `ServerKey` directives from `cupsd.conf`
- Now CUPS has zero TLS support → iOS IPPS attempt gets an immediate GnuTLS
  "no certificate" alert → iOS falls back to plain IPP in < 1 second
- No per-device setup needed, works for all iPhones automatically

**To re-enable SSL in future** (e.g. if you want to do the cert install):
```bash
sudo chattr -i /etc/cups/ssl/   # unlock the directory
# then follow SSL setup steps and add ServerCertificate/ServerKey to cupsd.conf
```

**Trade-off:** Print jobs travel over plain HTTP on the local network (no
encryption). Acceptable for home/office — only a concern if you're on a
shared/untrusted network.

### 4. macOS "The printer is in use" — FIXED

**Problem:** macOS print jobs fail immediately with "The printer is in use."
The Pi shows the printer as idle with an empty spool.

**Root cause:** CUPS 2.4.x always advertises `TLS=1.2` in its Bonjour TXT
record even when no SSL certs are installed — it's compiled into the binary,
not controllable via `cupsd.conf`. When macOS sees `TLS=1.2`, it
auto-discovers the printer as `ipps://` (encrypted IPP). Every connection
then fails because the Pi has no cert → TLS handshake drops → macOS returns
`CUPS_BACKEND_BUSY` → "The printer is in use." iOS has a fallback path that
recovers; macOS CUPS does not.

**Fix applied:**
1. Set `BrowseLocalProtocols none` in `cupsd.conf` — disables CUPS's own
   Bonjour advertisement entirely.
2. Created `/etc/avahi/services/blueprint-td110.service` — a static Avahi
   registration with all the same TXT records as CUPS would generate, but
   **without the `TLS=1.2` key**, plus the `_universal._sub._ipp._tcp` AirPrint
   subtype so iOS can discover it. This is the file in `avahi/` in this package.
3. Restarted CUPS and avahi-daemon.

macOS and iOS now both discover and print correctly. Because the Bonjour record
no longer advertises TLS, any device that re-discovers the printer will
auto-connect via plain IPP.

**Why the subtype matters:** iOS AirPrint browses specifically for
`_universal._sub._ipp._tcp` (not plain `_ipp._tcp`). Android (Mopria) browses
`_ipp._tcp` directly. CUPS registers this subtype automatically; a hand-written
Avahi file must include it explicitly via `<subtype>_universal._sub._ipp._tcp</subtype>`
or iOS will not see the printer at all.

**On an existing Mac** that had already added the printer as `ipps://`,
change the URI once:
```bash
lpadmin -p <printer-name> -v "ipp://<pi-ip>/printers/Blueprint-TD110"
```
Or delete the printer and re-add — it will now discover as plain IPP.

**To verify the Bonjour advertisement from Mac:**
```bash
dns-sd -L "Blueprint BP-TD110 Label Printer @ <hostname>" _ipp._tcp local.
# Should show all TXT records but NO TLS=1.2
```

### 5. Label content offset / not centered — FIXED

**Problem:** Label content was shifted left (cut off near physical edge).
Then after initial fix, it was slightly too far right with an asymmetric
right margin.

**Root cause — phase 1:** TSPL `BITMAP 0,0,...` starts at the physical edge
of the printhead. The BP-TD110 has a dead zone at x=0 — content placed at
x=0 gets partially cut off.

**Root cause — phase 2:** The PPD's `ImageableArea` had only 4pt (≈1.4mm)
margins, and the filter's `x_offset=16` (2mm) placed the bitmap further right.
The result was 2mm left margin but only ~0.87mm right margin — visually
off-center.

**Fix applied — complete solution:**
- Filter `PrintXOffset` default: **16 dots (2mm)** — shifts the BITMAP right
  in printer coordinates to clear the dead zone and set the left margin.
- PPD `HWMargins` changed from `4pt` to `5.67pt` (= 16 dots = exactly 2mm)
  on each side. All `ImageableArea` entries updated accordingly.
- CUPS now rasterises content into **767 dots** wide (was 776 dots).
- Filter places 767-dot bitmap at x=16.
- Result: **2mm left margin, 2mm right margin** on a 100mm label.

Layout at 203 DPI (label width = 799 dots):
```
|<-- 16 dots (2mm) -->|<-- 767 dots content -->|<-- 16 dots (2mm) -->|
0                     16                       783                   799
```

To adjust the horizontal position at runtime:
```bash
sudo lpadmin -p Blueprint-TD110 -o PrintXOffset=20   # shift right
sudo lpadmin -p Blueprint-TD110 -o PrintXOffset=12   # shift left
```

---

## TSPL Protocol (What the Filter Sends)

```
SIZE {w} mm, {h} mm          <- label physical size
GAP 2 mm, 0 mm               <- gap sensing (BLINE for black mark, GAP 0 for continuous)
SET RIBBON OFF               <- direct thermal, no ribbon
DENSITY {n}                  <- darkness 0-15, default 8
SPEED {n}                    <- inches/sec, default 4
SET TEAR ON
SET PEEL OFF
SET CUTTER OFF
CLS                          <- clear image buffer
BITMAP {x_offset},0,{w_bytes},{h},1,{binary_data}
PRINT 1,1                    <- print 1 copy
```

**Bitmap data:** 1-bit packed, MSB = leftmost dot, bit 1 = print (black).
CUPS raster is inverted (bit 1 = white in CS_W convention) so every byte is
bitwise-NOT'd before sending.

---

## Compatible Blueprint Models

| Model | Protocol | Notes |
|-------|----------|-------|
| BP-TD110 | TSPL | Tested, working |
| BP-TD110D | TSPL | Same filter |
| BP-TD110X | TSPL | Same filter |
| BP-TR110 | TSPL | Same filter |
| BP-TD110BT | TSPL | Bluetooth — needs extra BT setup |
| BP-TR110Z | ZPL | Different protocol, not supported here |

---

## Tailscale + AirPrint

AirPrint discovery does **not** work over Tailscale — mDNS is LAN-only and
Tailscale doesn't forward multicast traffic.

**Workaround for Mac on Tailscale:**
System Settings → Printers → + → IP tab:
- Address: `<pi-tailscale-ip>`
- Queue: `/printers/Blueprint-TD110`
- Use: Auto Select

**iPhone on Tailscale:** No clean solution. Use the printer on local WiFi instead.

---

## Useful Commands

```bash
# Printer status
lpstat -p Blueprint-TD110

# All jobs (including completed)
lpstat -W all

# Cancel all jobs
sudo bash -c "cancel -a Blueprint-TD110"

# Clear stuck spool (if jobs won't cancel)
sudo systemctl stop cups cups.socket cups.path
sudo bash -c "rm -f /var/spool/cups/c* /var/spool/cups/d*"
sudo systemctl start cups

# Restart CUPS
sudo systemctl restart cups

# Restart Avahi (if AirPrint stops advertising)
sudo systemctl restart avahi-daemon

# Verify Bonjour advertisement (from Mac — should NOT show TLS=1.2)
dns-sd -L "Blueprint BP-TD110 Label Printer @ <hostname>" _ipp._tcp local.

# Check AirPrint is broadcasting (from Mac)
dns-sd -B _ipp._tcp local.

# Check CUPS error log
sudo tail -f /var/log/cups/error_log

# Enable debug logging (temporarily)
sudo cupsctl --debug-logging
# ... reproduce issue ...
sudo cupsctl --no-debug-logging

# Adjust horizontal alignment
sudo lpadmin -p Blueprint-TD110 -o PrintXOffset=16   # default (2mm)
sudo lpadmin -p Blueprint-TD110 -o PrintXOffset=20   # shift right
sudo lpadmin -p Blueprint-TD110 -o PrintXOffset=12   # shift left

# CUPS web UI (from any browser on the local network)
# http://<pi-ip>:631
```
