# Connected-board bring-up

## Confirmed hardware and authorization

The owner confirmed that the supplied factory/reference firmware drives the
LCD correctly, explicitly authorized flashing, and confirmed that the SD card
is disposable and may be formatted for testing.

ROM inspection on COM3 identified ESP32-S3 QFN56 revision 0.2, 16 MiB quad
3.3 V flash, and 8 MiB embedded PSRAM. Secure boot and flash encryption are
disabled. No eFuse key material was read or programmed.

The flashed local-test image booted ESP-IDF 6.1 successfully. Runtime PSRAM
initialization found 8 MiB at 80 MHz and its SRAM memory test passed. Startup
reported 266,971 bytes of free internal memory and a 139,264-byte largest DMA
block before peripheral allocations. Audio initialization returned ESP_OK.
Those figures are startup observations, not worst-case streaming budgets.

## Factory layout is deliberately preserved

**Do not blindly run the generated `idf.py flash` or `idf.py app-flash` command
on this board's current layout.** The project partition CSV places its app at
`0x20000`; the retained factory table places the running app at `0x10000`.
Changing layouts requires a separate deliberate NVS/data migration.

| Factory region | Offset | Size |
| --- | --- | --- |
| Bootloader region | `0x0000` | 32 KiB reserved |
| Partition table | `0x8000` | 4 KiB reserved |
| NVS | `0x9000` | 24 KiB |
| PHY data | `0xf000` | 4 KiB |
| Factory application | `0x10000` | 3 MiB |
| Internal FAT filesystem | `0x3f0000` | 8 MiB |
| Internal SPIFFS filesystem | `0xc00000` | 4 MiB |

Before programming, the original bootloader region, partition table and entire
factory application slot were backed up to private session artifacts, with
sizes and SHA-256 verified. The backup names are
`factory-bootloader-region.bin`, `factory-partition-table.bin`, and
`factory-application.bin`. They are not committed or uploaded to a registry.

Only the new bootloader at `0x0` and the 1,488,128-byte LCD-enabled local-test
application at `0x10000` were programmed initially. Both written hashes were
verified. The original partition table, NVS, PHY and internal FAT/SPIFFS data
were not overwritten. Boot logs confirm loading the new application from
the existing `0x10000` factory entry.

The initial local image kept credentials disabled. The owner subsequently
explicitly approved ordinary unencrypted NVS for this PoC, and
`sdkconfig.poc-sync.defaults` was built and flashed at the same retained
`0x10000` app offset. SD auto-formatting is disabled again in that image.
No client secret, flash-encryption key, or eFuse key was provisioned.
Wi-Fi and user authorization are completed through the private BLE companion.

## Disposable SD formatting test

The first real mount returned FatFs `FR_NO_FILESYSTEM` (13), while the app
continued to initialize its LCD/audio and menu. The card was not silently
formatted.

The owner-approved test profile is `firmware\sdkconfig.sd-format-test.defaults`.
It enables `CONFIG_RECORDER_TEST_FORMAT_SD_ON_MOUNT_FAILURE`; that option is
**off by default** and warns explicitly when enabled. It permits the SDK to
format only the connected SD card when its filesystem cannot mount, not any
ESP flash partition. This is not the normal production behavior.

The test formatted the disposable SD card successfully and the subsequent mount
returned ESP_OK. The owner confirmed the new LCD showed SD OK, and physically
tested recording and playback successfully. The later PoC sync image has the
formatting option disabled before normal use.

## Remaining physical acceptance

The owner confirmed the menu, local recording/playback, authenticated BLE
provisioning, Wi-Fi configuration, Microsoft consent, and all recordings
appearing in OneDrive's `local-recording` folder. Sync succeeded after another
firmware flash/reboot without re-entering Wi-Fi or Microsoft credentials,
exercising saved Wi-Fi configuration and refresh-token reuse.

The owner noted quiet recording/local-speaker volume and explicitly accepted
it for now; gain settings were not increased. Sustained SD fault behavior,
shared I2S clock/clear behavior and live on-board AI acoustics remain separate
acceptance items.

## BLE discovery correction

The first real companion scan failed because its service filter incorrectly
used `0000ffff-0000-1000-8000-00805f9b34fb`. An unfiltered Windows scan found the
recorder advertising the SDK's actual service
`1775244d-6b43-439b-877c-060f2d9bed07`. The companion default and shared contract
were corrected, and firmware now explicitly pins that value for future builds.
The current flashed firmware already advertises it, so the host fix needs no
reflash.

Real Windows GATT discovery then found `proto-ver`, `prov-session`,
`prov-config`, and `recorder-control` with their 0x2901 descriptors. This
initially confirmed advertising and endpoint discovery. The subsequent real
Security 2 exchange, Wi-Fi setup and Microsoft authorization also completed.

## Sync failures corrected on real hardware

The consumer device endpoint returned `https://www.microsoft.com/link`, which
was added as an exact allowed verification URL. A valid device-level rejection
is now distinct from a corrupt protocol response, so it does not unnecessarily
destroy the secure BLE connection.

Live UART diagnostics identified an authorization header requiring about
1.5 KiB while the HTTP TX buffer allowed only 1 KiB. HTTPS and upload requests
now size their bounded TX buffer for the actual bearer token and URL, including
long preauthenticated upload URLs. Oversized inputs remain rejected, transport
errors are preserved, and diagnostics contain only safe phase/status/size data.

Wi-Fi reconnect begins on the station-start event, and the LCD displays Wi-Fi
and clock readiness. Entering a new Setup session still intentionally clears
the active RAM Wi-Fi configuration through Espressif's provisioning manager;
configure `wifi` again in that session before `graph`. Ordinary reboot uses
the saved NVS configuration and does not require Setup.
