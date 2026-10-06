# UWE5622 driver for OpenWrt

Linux/OpenWrt driver for the Unisoc/Spreadtrum UWE5622 Wi-Fi and Bluetooth chipset.

This repository contains a port of the original Spreadtrum/Unisoc UWE5622 driver to current OpenWrt and Linux kernels.

The driver is currently tested on the **WalnutPi 1B** with:

- OpenWrt main
- Linux 6.18
- mac80211/backports 7.2
- aarch64 / Allwinner H618
- UWE5622 connected via SDIO

## Status

### Wi-Fi

Working:

- SDIO initialization
- firmware loading
- cfg80211/mac80211 integration
- scanning
- station mode
- AP mode
- correct permanent MAC address registration

The driver has been adapted to the cfg80211 API used by current OpenWrt mac80211/backports.

Several kernel Fortify warnings caused by writes spanning structure fields in the original driver have also been fixed.

### Bluetooth

Working:

- Bluetooth power control through rfkill
- UWE5622 Bluetooth firmware initialization
- PSKEY configuration
- RF parameter configuration
- controller enable
- `/dev/ttyBT0`
- standard Linux `hci_uart`
- standard BlueZ `btattach`
- BR/EDR
- Bluetooth LE
- device discovery

The original userspace Bluetooth initialization has been moved into the kernel driver.

The initialization sequence uses the UWE5622 vendor HCI commands:

- `0xFCA0` — PSKEY configuration
- `0xFCA2` — RF configuration
- `0xFCA1` — controller enable

The driver waits synchronously for the corresponding HCI command-complete events.

A workaround is included for a controller capability issue where the UWE5622 advertises PARK support but rejects PARK when Linux configures the default link policy.

## OpenWrt package

The repository is structured as an OpenWrt kernel package:

```text
uwe5622/
├── Makefile
├── files/
│   └── uwe5622-bt.init
├── src/
│   ├── unisocwcn/
│   ├── unisocwifi/
│   └── tty-sdio/
└── tools/
    └── sprd_bt_init.c
```

The package builds the following kernel modules:

```text
uwe5622_bsp_sdio.ko
sprdwl_ng.ko
sprdbt_tty.ko
```

The OpenWrt init script powers up the Bluetooth part through rfkill and starts the standard BlueZ `btattach` utility for `/dev/ttyBT0`.

## Installation in an OpenWrt source tree

Copy or clone this repository to:

```text
package/kernel/uwe5622
```

For example:

```sh
cd openwrt/package/kernel
git clone <repository-url> uwe5622
```

Then enable the UWE5622 kernel package in the OpenWrt configuration and build it using the normal OpenWrt build system.

For package-only development:

```sh
make package/kernel/uwe5622/clean
make package/kernel/uwe5622/compile V=s
```

Firmware

The required UWE5622 firmware and configuration files are included in:

src/unisocwcn/fw/

Included files:

wcnmodem.bin
wifi_2355b001_1ant.ini
bt_configure_pskey.ini
bt_configure_rf.ini

The OpenWrt package installs the required files under /lib/firmware/.

## Debugging

Additional debug output added during the port is guarded by:

```c
UWE5622_DEBUG
```

Normal builds do not define this symbol.

It can be enabled during driver development without enabling generic kernel-wide `DEBUG` output.

## Tested platform

The current development and test platform is:

**WalnutPi 1B**

- Allwinner H618
- ARM64 Cortex-A53
- UWE5622 Wi-Fi/Bluetooth
- OpenWrt sunxi/cortexa53

WalnutPi 1B board support for OpenWrt is maintained separately from this driver.

## Origin

The driver is based on the original Spreadtrum/Unisoc UWE5622 Linux driver.

Original source files retain their Spreadtrum Communications Inc. copyright and authorship notices.

Changes in this repository include the OpenWrt integration and adaptations required for current Linux kernels, current cfg80211/mac80211 backports, Bluetooth initialization, kernel API changes, and Fortify fixes.

## License

The original driver source states that it is licensed under the **GNU General Public License version 2**.

Individual source files retain their original copyright and license notices.
