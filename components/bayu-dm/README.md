# bayu-dm

The Bayu display manager. It runs the login screen and starts the session
the user picks: a Wayland one (`wayland-sessions`), or an X11 one
(`xsessions`, such as IceWM), which `bayu-dm-xsession` runs on Xorg. The
login screen offers X11 sessions, named "... (X11)", when Xorg and xinit are
installed.

## Dependencies

The base library of [bayu-toolkit](https://github.com/igos-nusantara/bayu)
and PAM; `bayu-compositor` at runtime. GoogleTest for the tests.

## Build

```shell
cmake -S . -B build -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build
sudo cmake --install build
```

A source install needs the login screen's user once:

```shell
sudo adduser --system --group --home /var/lib/bayu-greeter bayu-greeter
```

## Enable

```shell
sudo systemctl disable sddm
sudo systemctl enable bayu-dm
```

To log a user in without a password once per boot, as a live system does,
add to `/etc/bayu-dm.conf`:

```ini
[Autologin]
User=bayu
# Optional; the default session otherwise.
Session=bayu-wayland
```

## Tests

```shell
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build
ctest --test-dir build --output-on-failure
```
