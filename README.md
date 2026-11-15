# zmk-host-status

A small ZMK module that lets apps on your computer see what your keyboard is
doing: the active layer, which layers are locked on, whether every split half
is linked, and which firmware build is running. It adds one Bluetooth GATT
service to the central half and notifies only when something changes, so the
cost is a few bytes per layer change.

Built for a [zmk-battery-bar](https://github.com/itouuuuuuuuu/zmk-battery-bar)
fork that shows the layer in the macOS menu bar, but the protocol is plain GATT
and any host app can read it.

## Install

Add to `config/west.yml`:

```yaml
  remotes:
    - name: johncattrall
      url-base: https://github.com/johncattrall
  projects:
    - name: zmk-host-status
      remote: johncattrall
      revision: main
```

That's all. The module enables itself on the central half of a split (or on a
unibody board) and stays out of peripheral builds. Disable with
`CONFIG_ZMK_HOST_STATUS=n`. Only the central half needs reflashing.

After flashing, macOS may keep its cached copy of the keyboard's old service
list. If a host app does not see the new service, remove the keyboard in
System Settings, Bluetooth, and pair it again.

## Protocol

Service `6e1a0001-7c5b-4f2e-9a3d-2b8c4d1e5f60`, all reads require an encrypted
(bonded) link:

| Characteristic | UUID suffix | Properties | Value |
|---|---|---|---|
| Status | `...0002` | read, notify | 12-byte payload below |
| Layer names | `...0003` | read | names in keymap index order, `\n`-separated |
| Build | `...0004` | read | git short SHA of the config repo (`GITHUB_SHA` in CI), or `local` |

Status payload v1, little endian:

| Offset | Type | Field |
|---|---|---|
| 0 | u8 | version (1) |
| 1 | u8 | flags, bit 0: all expected split halves linked |
| 2 | u32 | active layers, bit *i* = layer at index *i* |
| 6 | u32 | locked layers: active with no key held (toggles, auto layers like NumWord, timed temp layers) |
| 10 | u8 | split halves connected |
| 11 | u8 | split halves expected (0 on unibody boards) |

Future versions only append fields, so readers should accept any version
of 1 or above with at least 12 bytes.

"Locked" is computed in firmware by watching key positions: a layer that is
still active when no key is held is locked. That cleanly separates `&tog` and
auto layers from `&mo`, `&lt` and combo holds without hooking any behavior.

## License

MIT
