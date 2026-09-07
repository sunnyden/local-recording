# Board evidence and bring-up checklist

Evidence: `reference\Board\ATK_DNESP32S3_V1.4.pdf`, sheets CORE, FUNC, FUNC2,
and the reference recorder/music register protocols. Vendor source is unchanged.
The schematic was rendered and inspected locally; generated review images are
not distributed with the firmware.

## GPIO40 and selector P5

The CORE schematic identifies **GPIO40 = CT_INT / LCD_DC**, not XL9555 INT.
FUNC2 connects XL9555 pin 1 to **IIC_INT**. CORE selector P5 has:

| Pin | Net |
| --- | --- |
| 1 | IIC_INT |
| 2 | SPI_MISO |
| 3 | BOOT |
| 4 | IO_SEL |
| 5 | 1WIRE_DQ |
| 6 | LCD_DC |

FUNC identifies the SPI LCD socket MISO-position pin as IO_SEL. SPI LCD use
requires **IO_SEL to LCD_DC (P5 pins 4–6)**. Leave IIC_INT isolated from SPI_MISO:
a 1–2 jumper would let the expander interrupt interfere with SD MISO. Do not
bridge 1WIRE_DQ to LCD_DC, and disconnect any external touch controller driving
CT_INT/GPIO40. The firmware holds CT_RST low and does not install GPIO40 IRQs.
Polling keys alone would not resolve a physical short.

The reference `XL9555_INT_IO GPIO_NUM_40` declaration is therefore not used.
The user subsequently confirmed working LCD output with the supplied factory/
reference firmware on this board. The coordinator accepts this as LCD routing
confirmation for local bring-up; it is not a claim that every P5 jumper was
visually inspected. The generic default profile still leaves GPIO40 untouched;
`-Profile local-bringup` enables it for this confirmed board with credentials off.

## Implemented pin map

| Function | GPIO / expander |
| --- | --- |
| I2C SDA / SCL | 41 / 42 |
| ES8388 7-bit address | 0x10 |
| XL9555 7-bit address | 0x20 |
| I2S MCLK / BCLK / WS | 3 / 46 / 9 |
| I2S ESP TX / RX | 10 / 14 |
| Shared SPI2 clock / MOSI / MISO | 12 / 11 / 13 |
| SD CS / LCD CS / LCD D/C | 2 / 21 / 40 |
| KEY0 / KEY1 / KEY2 / KEY3 | IO1_7 / IO1_6 / IO1_5 / IO1_4 |
| SPI LCD power / reset | IO1_3 / IO1_2, active high |
| Amplifier SHUTDOWN | IO0_2, **high = off, low = enabled** |

Amplifier polarity follows the MD8002A SHUTDOWN net and the music sample's
explicit speaker enable value 0, not the ambiguous `SPK_EN` name.
Initial codec output gain is 20/33 with an additional 6 dB digital attenuation.
ADC gain is fixed at 12 dB; no automatic level control. Adjust only after
checking clipping and feedback on the actual board.

Modern paired I2S channels use Philips stereo 16-bit slots, 16 kHz WS, a coherent
**256 × 16 kHz = 4.096 MHz MCLK**, and codec ADC/DAC ratio registers 0x02.
The microphone is LIN1/RIN1 according to FUNC2; the left ADC is copied to both
I2S slots by the codec and firmware selects left. Validate the analog routing
with an actual microphone signal; reference configuration alone is not proof.

## Discovery and parent ROM identification

PnP found `USB-SERIAL CH340 (COM3)`,
`USB\VID_1A86&PID_7523\5&2E786FD2&0&9`.
`Win32_SerialPort` alone is insufficient on this machine. This initial PnP
discovery did not open the serial port or reset the board.

On 2026-09-07 the parent separately used esptool 5.3.1 `--no-stub` chip-ID and
flash-ID commands through COM3, with the working Espressif activation profile.
It identified ESP32-S3 QFN56 revision 0.2, embedded 8 MB AP 3.3 V PSRAM, and
16 MB quad 3.3 V flash. **Two normal RTS resets occurred.** There were no flash
writes, erase, eFuse writes, or credential reads. The firmware implementation
agent did not independently open COM3.

This establishes ROM-reported capacities only. Runtime PSRAM initialization,
octal/80 MHz compatibility, our LCD driver, and microphone/speaker/acoustic
operation still need verification. The later factory/reference LCD confirmation
satisfies the coordinator's routing gate. Application flashing remains a parent-
coordinated rollback-safe local-only operation, not something this build runs.

## Owner-coordinated next steps

1. Verify SPI LCD module (ST7789, 320×240), P5 routing, speaker connection, FAT SD.
2. Explicitly approve flashing and decide whether real credentials may persist.
3. Build the matching profile. Never infer approval from the compile-only profile.
4. Flash through the discovered CH340 port only under parent coordination.
5. Read startup telemetry: flash capacity, PSRAM total, internal free, largest DMA.
6. Check keys and LCD orientation before starting audio; no card auto-format.
7. Record an acoustic reference, inspect WAV rate/channels/duration and play it.
8. Measure MCLK/BCLK/WS and full-duplex clock continuity during playback clear.
9. Fault-test removal/full card and power-loss recovery using expendable test media.
10. Only then enroll Wi-Fi/Microsoft and validate server VAD/AEC at safe volume.
