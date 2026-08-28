# ADR-0009: VelaWear BLE Canonical Source

## Status

Accepted

## Context

The current ble_repro build enters through
`openvela/apps/packages/demos/contest2026_329_velawear_agent`. This path is a
symbolic link to
`openvela/contest2026_329_dela/app/velawear_agent`, so the linked directory is
the canonical application tree used by the build.

The Git-root-level `app/velawear_agent` is an older copy. It is not the input
to the current ble_repro build and does not contain the canonical BLE
implementation.

## Decision

1. The canonical VelaWear GATT implementation is
   `openvela/contest2026_329_dela/app/velawear_agent/drivers/velawear_ble.c`.
   It owns the service UUID, status and threshold characteristic UUIDs, GATT
   attributes, CCCD handling, and notification behavior.
2. The application `CMakeLists.txt` must add
   `drivers/velawear_ble.c` to the VelaWear application sources, which produces
   `libapps_velawear.a`.
3. BLE changes must only be made in the canonical file and its canonical
   application tree. Do not modify the Git-root-level
   `app/velawear_agent` copy for BLE work.

## Verification

For every BLE source change:

1. Check that the canonical `CMakeLists.txt` contains
   `drivers/velawear_ble.c`.
2. Check the current Ninja command list for
   `drivers/velawear_ble.c` and the VelaWear application archive.
3. Check the final ELF and relevant static libraries for
   `velawear_ble_*` and GATT symbols.

## Consequences

Future BLE changes have one canonical source of truth. The host regression
script must keep its UUIDs and service contract synchronized with the
canonical GATT implementation, including status, threshold, CCCD, and notify
behavior.
