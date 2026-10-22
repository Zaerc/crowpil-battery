# crowpil-battery

Linux kernel module exposing a CrowPi-L battery as a standard
[`power_supply`](https://www.kernel.org/doc/html/latest/power/power_supply_class.html)
device (`BAT0` / `AC0`), polled over I2C. Packaged with DKMS so it rebuilds
automatically whenever a new kernel is installed.

Tested against Rockchip vendor kernels (`linux-headers-*rockchip*`).

## Requirements

- `dkms`
- Kernel headers matching your running kernel (`uname -r`)

## Install (from .deb)

    dpkg-buildpackage -us -uc -b
    sudo dpkg -i ../crowpil-battery-dkms_*.deb

Verify:

    dkms status
    # crowpil_battery/1.0, <kernelver>, aarch64: installed

## Load

    sudo modprobe crowpil_battery
    cat /sys/class/power_supply/BAT0/capacity

## Troubleshooting

Build failures are logged to `/var/lib/dkms/crowpil_battery/<version>/build/make.log`.
The usual cause is missing headers for the target kernel.

To build for a kernel you aren't booted into:

    sudo dkms install -m crowpil_battery -v 1.0 -k <kernelversion>

## Related

I2C protocol constants and voltage curve derived from [crowPi-L_BatteryStatus](https://github.com/ppyne/crowPi-L_BatteryStatus) by Alexandre Vialle.

## License

GPL-2.0-or-later. See [LICENSE](LICENSE). Based in part on
`linux-fake_battery-module` (https://github.com/hoelzro/linux-fake-battery-module) by Rob Hoelz.

