## Dependencies (install once)

### Arch
> Install the headers matching your kernel (`linux-lts` → `linux-lts-headers`, `linux-zen` → `linux-zen-headers`, etc.)

    sudo pacman -S linux-headers libpulse

### Debian/Ubuntu

    sudo apt install linux-headers-$(uname -r) libpulse-dev

## Build & install

    git clone <repo>
    cd shure-mv6-driver
    make
    sudo make install
    sudo modprobe hid_shure_mv6
    systemctl --user enable --now shure-mv6-sync

## Uninstall

    systemctl --user disable --now shure-mv6-sync
    sudo make uninstall
    sudo rmmod hid_shure_mv6

The module loads automatically on next boot via `/etc/modules-load.d/`. The sync daemon starts automatically via the systemd user session.
