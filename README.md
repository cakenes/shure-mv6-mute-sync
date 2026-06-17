# Shure‑MV6‑Mute‑Sync
Bidirectional mute‑sync support for the Shure MV6 microphone on Linux.  
Provides a small HID kernel module plus a userspace daemon that keeps the hardware mute button and the system mute state in sync.

---

## Dependencies (install once)

<details>
<summary>Arch</summary>

> Make sure you install the headers matching your running kernel  
> (`linux-lts` → `linux-lts-headers`, `linux-zen` → `linux-zen-headers`, etc.)

```bash
sudo pacman -S dkms linux-headers libpulse
```

</details>

<details>
<summary>Debian / Ubuntu</summary>

```bash
sudo apt install dkms linux-headers-$(uname -r) libpulse-dev
```

</details>

---

## Install with DKMS (recommended)

DKMS automatically recompiles the kernel module after every kernel update.

```bash
git clone https://github.com/cakenes/shure-mv6-mute-sync.git
cd shure-mv6-mute-sync
make
sudo make dkms-install
sudo modprobe hid_shure_mv6
systemctl --user enable --now shure-mv6-sync
```

<details>
<summary>Uninstall</summary>

```bash
systemctl --user disable --now shure-mv6-sync
sudo rmmod hid_shure_mv6
sudo make dkms-uninstall
```

</details>

---

<details>
<summary>Install without DKMS (manual)</summary>

> ⚠️ You will need to re-run `make && sudo make install` after every kernel update.

```bash
git clone https://github.com/cakenes/shure-mv6-mute-sync.git
cd shure-mv6-mute-sync
make
sudo make install
sudo modprobe hid_shure_mv6
systemctl --user enable --now shure-mv6-sync
```

**Uninstall:**
```bash
systemctl --user disable --now shure-mv6-sync
sudo make uninstall
sudo rmmod hid_shure_mv6
```

</details>

The module loads automatically on next boot via `/etc/modules-load.d/`. The sync daemon starts automatically via the systemd user session.
