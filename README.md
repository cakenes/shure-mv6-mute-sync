# Shure‑MV6‑Mute‑Sync
Bidirectional mute‑sync support for the Shure MV6 microphone on Linux.  
Provides a small HID kernel module plus a userspace daemon that keeps the hardware mute button and the system mute state in sync.

---

## Dependencies (install once)

### Arch
> Make sure you install the headers matching your running kernel  
> (`linux-lts` → `linux-lts-headers`, `linux-zen` → `linux-zen-headers`, etc.)

```bash
sudo pacman -S linux-headers libpulse
```

### Debian / Ubuntu
```bash
sudo apt install linux-headers-$(uname -r) libpulse-dev
```

### Build & install
```bash
git clone https://github.com/cakenes/shure-mv6-mute-sync.git
cd shure-mv6-driver
make
sudo make install
sudo modprobe hid_shure_mv6
systemctl --user enable --now shure-mv6-sync
```

### Uninstall
```bash
systemctl --user disable --now shure-mv6-sync
sudo make uninstall
sudo rmmod hid_shure_mv6
```

The module loads automatically on next boot via `/etc/modules-load.d/`. The sync daemon starts automatically via the systemd user session.
