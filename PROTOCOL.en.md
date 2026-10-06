# Atom Spectra Protocol — commands and packet format

[🇷🇺 Русская версия](PROTOCOL.md) · **🇬🇧 English**

Reference for the communication protocol of the **Atom Spectra PRO / Atom Nano 8** multichannel
analyzer (MCA): text control/tuning commands and the binary **shproto** packet protocol this
gateway speaks.

> The analyzer talks over a virtual COM port (USB, FTDI). Port parameters: `8 N 1`, speed one of
> `38400 | 115200 | 460800 | 600000 | 921600`. At `38400` and `115200` the full spectrum cannot
> be downloaded once per second. Every text command is terminated by `Enter`.
>
> ⚠️ The settings / fine-tuning commands change the factory configuration of the device. Use them
> with care — the gateway does not need them for normal operation.

## What this gateway uses

| Gateway action | Command / code |
|---|---|
| Request parameters and calibration on connect | text `-inf` |
| "Reset" button in the Web UI | text `-rst` |
| Arbitrary command from the Web UI (`/api/command`) | any text (e.g. `-sta`, `-sto`, `-sho`) |
| Receive histogram | packet `cmd 0x01` |
| Receive status (CPS, dead-time…) | packet `cmd 0x04` |
| Receive text replies | packet `cmd 0x03` |

---

## Work modes

| Command | Description |
|---|---|
| `-mode 0\|1\|2` | Switch mode: `0` — MCA (normal operation), `1` — oscilloscope, `2` — pulse viewer. Modes 1/2 are for factory adjustment of the analog circuitry. |

## MCA — spectrum acquisition control

| Command | Description |
|---|---|
| `-sta [xx] [-r] [-s]` | Start acquisition. `xx` — acquisition time in seconds; `-r` — reset spectrum before start; `-s` — silent (the device does NOT upload the spectrum every second). |
| `-sto` | Stop acquisition. |
| `-sho` | Upload the current spectrum. |
| `-stt` | Upload current MCA status (`collecting` — processing data, `stopped` — not). |
| `-rst` | Reset the current spectrum. |

## Pulse viewer mode

| Command | Description |
|---|---|
| `-dbg XXX YYY` | Range of bins whose pulse waveforms to upload (`XXX` — lowest bin, `YYY` — highest). Example: `-dbg 600 700`. |

## Pulse processing settings (factory)

| Command | Description |
|---|---|
| `-ris XX` | Number of ADC points for pulse rise. |
| `-fall YY` | Number of ADC points for pulse fall. |
| `-max ZZ` | Maximum integral value. On reaching it, the pulse increments the last bin 8191. |
| `-U[0-255]` | High-voltage adjustment (HV). |
| `-V[0-255]` | Baseline adjustment. |
| `-nos AA` | Discriminator level. |
| `-hyst BB` | Discriminator hysteresis (recommended `1`). |
| `-frq CCCCCCC` | ADC sampling frequency in Hz (max 21 MHz). If unavailable, the nearest higher available value is set. |
| `-step D` | Discriminator comparison step. At high ADC frequencies must be >1 to avoid CPU overload. |

## Temperature compensation (max integral)

| Command | Description |
|---|---|
| `-tclear` | Clear max-integral temperature-compensation data. |
| `-t M N O` | Add a point: `M` — number (1..20), `N` — temperature, `O` — max integral for it. Up to 20 points, linear interpolation between them. |
| `-tp EEEE` | Time between max-integral recalculations, ms (recommended `1000`). |
| `-tc on\|off` | Enable/disable max-integral temperature compensation. |

## Baseline temperature compensation

| Command | Description |
|---|---|
| `-tc_pot?` | Request baseline temperature-compensation parameters. |
| `-tc_pot on` | Enable baseline compensation. |
| `-tc_pot off` | Disable baseline compensation. |
| `-t_pot H K L` | Add a baseline compensation point (`H` — number 1..20, `K` — temperature, `L` — value). |

## Overlapping-pulse rejection (pile-up)

| Command | Description |
|---|---|
| `-pileup [v1 v2 … v100]` | Pile-up compensation values (for detectors with long afterglow, e.g. CsI(Tl)). Each 0..1. |
| `-pthr [value]` | Minimum pulse amplitude to activate pile-up compensation. `8192` disables it. |
| `-prise [value1]` | ADC value (in % of amplitude) `N` samples BEFORE the peak. |
| `-srise N` | Number of samples before the peak (`0` — filter off). |
| `-pfall [value2]` | ADC value (in % of amplitude) `M` samples AFTER the peak. |
| `-sfall M` | Number of samples after the peak (`0` — filter off). |

> Logic: if the ADC value exceeds `value1%` of the peak `N` samples before the peak **or** exceeds
> `value2%` `M` samples after the peak, the pulse is treated as overlapping and rejected.

## Registers, calibration and serial number

| Command | Description |
|---|---|
| `-cal [0-39] hhhhhhhh` | 40 registers, a 4-byte number each. With no arguments — return all data. With a register address and data — write it. **The first 12 registers hold calibration data, the last register (39) holds the serial number.** |

## Other commands

| Command | Description |
|---|---|
| `-inf` | Request setting parameters (see the example response below). |
| `-spd 38400\|115200\|460800\|600000\|921600` | COM port speed. Use with extreme care! |
| `-sel 1\|2 1\|0` | Control CPU output pins (reserved for future use). |
| `-win EEE FFF` | Energy window for search mode (reserved). |
| `-div G` | Pulse divider in search mode (reserved). |
| `-defaults_save` | Save a copy of all settings. |
| `-defaults_restore` | Restore settings from the copy. |
| `-reboot` | Reboot the device. |

### Example `-inf` response

```
VERSION 13 RISE 8 FALL 12 NOISE 15 F 3000000.00 MAX 30000 HYST 1 MODE 0 STEP 1 t 9219
POT 102 POT2 26 T1 OFF T2 OFF T3 33.5 OUT 0..0/1 Prise 0 Srise 0 Pfall 0 Sfall 0
TC OFF TCpot OFF Tco [0 0 … 0] TP 1000 PileUp [] PileUpThr 8192
```

| Field | Meaning |
|---|---|
| `VERSION 13` | Firmware version |
| `RISE 8 FALL 12` | Pulse rise/fall in ADC samples (used for integration) |
| `NOISE 15` | Noise discriminator level |
| `F 3000000.00` | ADC sampling frequency |
| `MAX 30000` | Maximum integral value |
| `HYST 1` | Discriminator hysteresis |
| `MODE 0` | Current work mode |
| `STEP 1` | Discriminator comparison step |
| `t 9219` | Seconds of the current spectrum acquisition |
| `POT 102 POT2 26` | Values of `U` and `V` respectively |
| `T1/T2/T3` | Temperature sensors (`OFF` — not connected; `33.5` — °C) |
| `Prise/Srise/Pfall/Sfall` | Overlapping-pulse rejection parameters |
| `TC / TCpot` | Temperature-compensation statuses |
| `TP 1000` | Max-integral recalculation period, ms |
| `PileUp / PileUpThr` | Pile-up compensation data and threshold |

---

## Binary shproto protocol — packet level

Exchange runs over a virtual COM port, `8 N 1`, binary protocol **shproto**.

**Packet structure:**

```
0xFF 0xFE   <cmd>   <payload…>   <CRC16 lo> <CRC16 hi>   0xA5
└── start ──┘                    └── CRC16 Modbus ────┘  └ finish
```

- **Start:** `0xFF 0xFE`.
- **Finish:** `0xA5`.
- **ESC `0xFD`:** if a `0xFE`, `0xA5` or `0xFD` byte appears inside the packet, an `0xFD` is
  inserted before it and the byte itself is inverted (`~byte`). On receive: after a `0xFD`, invert
  the following byte back.
- **CRC16:** Modbus algorithm (polynomial `0xA001`), computed over `cmd + payload`.

> The reference parser implementation (`shproto.c` / `shproto.h`) ships with the manufacturer
> documentation. In this project it lives in `components/shproto/`.

### Command codes (packet level)

| Code | Purpose |
|---|---|
| `0x01` | Histogram (spectrum) |
| `0x02` | Oscilloscope (4096 ADC points) |
| `0x03` | Text (text commands and replies, e.g. `-inf`) |
| `0x04` | Status / additional info |

### Histogram packet (`cmd 0x01`)

- `data[0:2]` — `offset` (uint16 LE). `offset == 0` marks the start of a new spectrum.
- `data[2:]` — bins, each uint32 LE. Bin count: `(len − 2) / 4`.
- Full spectrum: `HISTOGRAM_SIZE = 8192` channels.

```c
if (packet.cmd == 0x01) {                 // histogram
    uint16_t offset = *(uint16_t*)&packet.data[0];
    if (offset == 0) hist_width = 0;
    uint8_t *p = &packet.data[2];
    int count = (packet.len - 2) / 4;
    for (int i = 0; i < count; i++) {
        int idx = offset + i;
        if (idx < HISTOGRAM_SIZE)
            hist[idx] = p[i*4] | (p[i*4+1]<<8) | (p[i*4+2]<<16) | (p[i*4+3]<<24);
    }
}
```

### Status packet (`cmd 0x04`)

Sequentially (little-endian):

| Field | Type | Description |
|---|---|---|
| time | `long` (u32) | Seconds of the current spectrum acquisition |
| cpu_load | `short` (u16) | CPU load, % |
| cps | `long` (u32) | Counts per second |
| dead_time (invalid_pulses) | `long` (u32) | Count of rejected pulses (offset 10). The BecqMoni reference reads it as `InvalidPulses`: `TotalPulseCount = ValidPulseCount + InvalidPulses` |
| pulse_width | `long` (u32) | Total pulse width (ADC samples, offset 14). NOT used in the dead-time calculation (diagnostics) |

> **Dead/live time (#DT-4).** The method of the reference software **BecqMoni** (Am6er,
> `Utils/LiveTime.cs` + `AtomSpectraVCPDeviceForm.cs`): per-pulse dead time `τ = (RISE+FALL+1) / F`,
> where RISE/FALL/F come from the `-inf` reply (i.e. reported by the device). Dead time over the
> acquisition: `dead = (ValidPulseCount + InvalidPulses) · τ`, `live_time = time − dead`, where
> `ValidPulseCount` is the histogram sum and `InvalidPulses` is the offset-10 field. The
> `pulse_width` field (offset 14) is **not used** by the reference and plays no part in the
> calculation.

---

## Sources

- **Manufacturer documentation**: *"Atom Spectra PRO series multichannel analyzer. Commands and
  fine tuning"* — describes the text commands, the shproto format and the reference parser
  `shproto.c` / `shproto.h`.
- **BecqMoni** — open-source PC software for Atom Spectra / Atom Nano 8:
  <https://github.com/Am6er/BecqMoni>
- **FTDI** — virtual COM port driver installation guides:
  <https://ftdichip.com/document/installation-guides/>

> This file is an original description compiled from the sources above. Numeric formats and command
> codes are cross-checked against the protocol implementation in `components/shproto/` and `main/`
> of this repository.