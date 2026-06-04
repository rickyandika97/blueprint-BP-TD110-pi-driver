#!/bin/bash
# install.sh — Install Blueprint BP-TD110 CUPS driver on Raspberry Pi (ARM Linux)
# Run as: sudo bash install.sh

set -e

if [ "$(id -u)" -ne 0 ]; then
    echo "ERROR: Run this script with sudo." >&2
    exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

echo "=== Blueprint BP-TD110 Driver Installer ==="
echo ""

# ---- 1. Install build dependencies ----
echo "[1/6] Installing build dependencies..."
apt-get update -qq
apt-get install -y -qq gcc libcups2-dev libcupsimage2-dev cups avahi-daemon

# ---- 2. Build the filter ----
echo "[2/6] Building raster-tspl filter..."
cd "$SCRIPT_DIR"
make clean
make

# ---- 3. Install filter ----
echo "[3/6] Installing filter to CUPS filter directory..."
FILTER_DIR=/usr/lib/cups/filter
install -o root -g root -m 755 raster-tspl "$FILTER_DIR/raster-tspl"

# ---- 4. Install PPD ----
echo "[4/6] Installing PPD file..."
PPD_DIR=/usr/share/cups/model/Blueprint
install -d -m 755 "$PPD_DIR"
gzip -c "$SCRIPT_DIR/ppd/Blueprint-BP-TD110.ppd" > "$PPD_DIR/Blueprint-BP-TD110.ppd.gz"
chmod 644 "$PPD_DIR/Blueprint-BP-TD110.ppd.gz"

# ---- 5. Configure CUPS ----
echo "[5/6] Configuring CUPS..."

CUPSD_CONF=/etc/cups/cupsd.conf

# Listen on all interfaces (needed for AirPrint clients on the network)
if ! grep -q "^Listen \*:631\|^Port 631" "$CUPSD_CONF"; then
    sed -i 's/^Listen localhost:631/Listen *:631/' "$CUPSD_CONF"
fi

# Disable CUPS's own Bonjour — Avahi static file handles it instead.
# CUPS 2.4.x unconditionally adds TLS=1.2 to its mDNS TXT record even when
# there are no SSL certs installed. macOS sees TLS=1.2, uses ipps://, and
# every job fails with "The printer is in use." because the TLS handshake
# drops immediately. By letting Avahi advertise instead (without TLS=1.2),
# all clients auto-connect via plain IPP.
sed -i 's/^BrowseLocalProtocols.*/BrowseLocalProtocols none/' "$CUPSD_CONF"
if ! grep -q "^BrowseLocalProtocols" "$CUPSD_CONF"; then
    echo "BrowseLocalProtocols none" >> "$CUPSD_CONF"
fi

# Allow access from local network
if ! grep -q "Allow @LOCAL" "$CUPSD_CONF"; then
    python3 - <<'PYEOF'
import re

with open('/etc/cups/cupsd.conf', 'r') as f:
    content = f.read()

loc_block = re.search(r'<Location />(.*?)</Location>', content, re.DOTALL)
if loc_block and 'Allow @LOCAL' not in loc_block.group(0):
    new_block = loc_block.group(0).replace(
        '</Location>',
        '  Order allow,deny\n  Allow @LOCAL\n</Location>'
    )
    content = content[:loc_block.start()] + new_block + content[loc_block.end():]

admin_block = re.search(r'<Location /admin>(.*?)</Location>', content, re.DOTALL)
if admin_block and 'Allow @LOCAL' not in admin_block.group(0):
    new_block = admin_block.group(0).replace(
        '</Location>',
        '  Allow @LOCAL\n</Location>'
    )
    content = content[:admin_block.start()] + new_block + content[admin_block.end():]

with open('/etc/cups/cupsd.conf', 'w') as f:
    f.write(content)
PYEOF
fi

# Disable SSL so iOS falls back to plain IPP instantly (no 15-second delay).
# CUPS auto-generates self-signed certs which iOS rejects slowly (15+ retries).
# Without any cert, the rejection is immediate and iOS falls back in < 1 second.
mkdir -p /etc/cups/ssl
rm -f /etc/cups/ssl/*.crt /etc/cups/ssl/*.key
# Lock the directory so CUPS cannot regenerate certs on restart
chattr +i /etc/cups/ssl/ 2>/dev/null || true
# Remove any SSL directives left from a previous install
sed -i "/^ServerCertificate\|^ServerKey\|^TLSMinVersion\|^DefaultEncryption/d" "$CUPSD_CONF"

# ---- 6. Install Avahi static service file (AirPrint advertisement) ----
echo "[6/6] Installing Avahi AirPrint service file..."
# This registers the printer via mDNS/_ipp._tcp without the TLS=1.2 key,
# so macOS and iOS both discover it as plain IPP (ipp://) instead of ipps://.
install -d -m 755 /etc/avahi/services
install -o root -g root -m 644 \
    "$SCRIPT_DIR/avahi/blueprint-td110.avahi.service" \
    /etc/avahi/services/blueprint-td110.service

systemctl enable cups
systemctl restart cups

systemctl enable avahi-daemon
systemctl restart avahi-daemon

systemctl enable cups-browsed
systemctl restart cups-browsed

echo ""
echo "=== Installation complete ==="
echo ""
echo "Next steps:"
echo ""
echo "  1. Connect your Blueprint BP-TD110 printer to the Raspberry Pi via USB."
echo ""
echo "  2. Add the printer queue (replace serial if needed):"
echo "       sudo lpadmin -p Blueprint-TD110 \\"
echo "           -E \\"
echo "           -v \"usb://Blueprint/BP-TD110?serial=TD11022330168\" \\"
echo "           -P /usr/share/cups/model/Blueprint/Blueprint-BP-TD110.ppd.gz \\"
echo "           -o printer-is-shared=true \\"
echo "           -o media=w100h150 \\"
echo "           -D \"Blueprint BP-TD110 Label Printer\" \\"
echo "           -L \"Raspberry Pi\""
echo "       sudo cupsaccept Blueprint-TD110"
echo "       sudo cupsenable Blueprint-TD110"
echo "       sudo lpadmin -d Blueprint-TD110"
echo ""
echo "  3. Open port 631 in UFW:"
echo "       sudo ufw allow 631/tcp comment \"CUPS printing\""
echo "       sudo ufw reload"
echo ""
echo "  4. Your Blueprint printer will appear as an AirPrint printer on"
echo "     any device on the same local network (iPhone, iPad, Mac, etc.)."
echo "     On Mac: System Settings → Printers & Scanners → +"
echo "     Look for 'Blueprint BP-TD110 Label Printer' in the Default tab."
echo ""
echo "  NOTE: If a Mac had previously added this printer as ipps://, fix it:"
echo "    lpadmin -p <name> -v \"ipp://$(hostname -I | awk '{print $1}'):631/printers/Blueprint-TD110\""
echo ""
