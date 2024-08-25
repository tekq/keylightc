<div align="center">

# `keylightc`

### Keyboard backlight daemon for Framework laptops in C
</div>

`keylightc` is a small system daemon for [Framework] laptops that listens to keyboard and touchpad input, and turns on the keyboard backlight while either is being used.
It is like [keylightd] except in C and with no dependencies!

[Framework]: https://frame.work/
[keylightd]: https://github.com/jonas-schievink/keylightd

## Package Installation

It is highly recommended to install a package if one is available.  Currently, a [PPA] is available for Ubuntu.  To set it up, run:

```shell
$ sudo add-apt-repository ppa:mamarley/keylightc-git
$ sudo apt update
$ sudo apt install keylightc
```

[PPA]: https://launchpad.net/~mamarley/+archive/ubuntu/keylightc-git

## Source Installation

To install from source, clone the repository and run:

```shell
$ make
$ sudo make install
$ sudo udevadm trigger -c add -s leds
$ sudo systemctl enable --now keylightc.service
```

`keylightc` has no dependencies you have to install first.  Really.
It works using the sysfs keyboard backlight interface introduced in Linux 6.11 and therefore requires at least that version.
It uses kernel event data directly and does not have any dependency on a desktop environment or display server.

## Running

Note that `keylightc` must either be run as root or the included `90-keylightc.rules` file placed in `/usr/lib/udev/rules.d`
and `sudo udevadm trigger -c add -s leds` executed before `keylightc` is run as a user with the `input` group.
Using the .deb package or executing the source installation commands above sets this up for you.

`keylightc` takes the following command-line arguments:

```
Usage: keylightc [--brightness <brightness>] [--fadeduration <fadeduration>] [--timeout <timeout>]
keylightc - automatic keyboard backlight daemon for Framework laptops
Options:
  --brightness          brightness level when active (1-100) [default=30]
  --fadeduration        fade time in microseconds (1-1000000) [default=100000]
  --timeout             activity timeout in seconds (1-2147483647) [default=10]
  --help                display usage information
```
