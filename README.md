<table>
  <tr>
    <td>
      <img src="https://github.com/user-attachments/assets/50fa1be6-618c-4df2-8bbb-7a6f6f3c3b64" alt="1" width="100%" />
    </td>
    <td>
      <video src="https://github.com/user-attachments/assets/31f3e455-6a3a-4056-8376-136cc2250673" alt="2" width="100%" />
    </td>
  </tr>
</table>

# Piano Lights, a BLE/MIDI bridge between Synthesia and a LED strip

Arduino firmware for the ESP32 WROOM: a **BLE/MIDI** peripheral recognized by
Windows and usable as a **MIDI output in Synthesia** (with the 'key lights'
feature). Incoming notes light up the matching LEDs of a **WS2812B**
strip. Alignment and colors are configured through an **embedded web
page** (WiFi STA with automatic AP fallback).

Optionally, an **I2S MEMS microphone** (INMP441) turns the firmware into a
MIDI *input* as well: it listens to an **acoustic piano** and reports the
notes actually played back to Synthesia, so the song waits for you even
without a digital piano ("Melody" mode).

## 1. Installation (Arduino IDE)

1. Install the board support package **esp32 by Espressif Systems**
   (Boards Manager).
2. Install via the Library Manager:
   - **MIDI Library**        (Francois Best)
   - **BLE-MIDI**            (lathoub, but patched afterwards)
   - **NimBLE-Arduino**      (h2zero)
   - **FastLED**             (Daniel Garcia)
   - **ArduinoJson**         (Benoit Blanchon)
   - **ESP Async WebServer** (ESP32Async)
   - **Async TCP**           (ESP32Async)
3. Select the **ESP32 Dev Module** board and, importantly, under **Tools**:
   **Partition Scheme → "Minimal SPIFFS (1.9MB APP with OTA/128KB SPIFFS)"**.
   Indeed, with BLE + WiFi the binary exceeds the default app partition.
4. Open `PianoLights.ino` (the `PianoLights.h` file must be in
   the same folder; it will appear as a second tab) and upload.

> **Replacing the BLE-MIDI library**:
> The `BLE-MIDI` version (lathoub) shipped by the manager is not
> compatible as-is with NimBLE as used here (the fix concerns
> `src/hardware/BLEMIDI_ESP32_NimBLE.h`). An already-fixed version is provided
> with the project in `libraries/BLE-MIDI.zip`. To install it:
>
> 1. Close the Arduino IDE.
> 2. Unzip `libraries/BLE-MIDI.zip` (from the project folder): the archive
>    contains a `BLE-MIDI/` folder.
> 3. Replace the contents of the folder **entirely**
>    `C:\Users\%USERNAME%\Documents\Arduino\libraries\BLE-MIDI`
>    with the contents of this archive. Concretely: delete the existing
>    `BLE-MIDI` folder if present (if it was installed by the manager),
>    then copy the `BLE-MIDI/` folder from the archive in its place, so as
>    to obtain `...\Arduino\libraries\BLE-MIDI\src\hardware\BLEMIDI_ESP32_NimBLE.h`.
>    We replace the whole folder (not just this file) to avoid any
>    mix of versions.
> 4. Reopen the Arduino IDE: the fixed library is then taken into account.

## 2. First startup

1. On first boot (no WiFi saved), the ESP32 opens the access
   point **`Piano-Lights-AP`** (open, no password).
2. Connect to it, open **http://192.168.4.1**, enter the SSID/password
   of your router in the WiFi section → the ESP32 reboots in STA.
3. After that, the page is reachable at **http://pianolights.local** (or via
   the IP shown in the serial monitor at 115200 baud).
4. If the connection fails (wrong password, router off), AP mode
   re-enables automatically after 15 s.

## 3. Pairing with Windows + Synthesia

1. Windows: **Settings → Bluetooth & devices → Add a device →
   Bluetooth** → pair **Piano-Lights**. (Windows 10/11 required: BLE/MIDI
   goes through the UWP / WinRT MIDI API.)
2. Enable WinRT support so Synthesia sees the BLE/MIDI bridge:
   - hold the **Shift** key while launching
     **Synthesia** to open the configuration window
   - in the **Setting** dropdown, look for
     **`Midi.UseWinRTMidi`**
   - tick the **Value** box, then close the config and restart Synthesia
3. Synthesia: **Settings → Music Devices** → under outputs, enable
   **Piano-Lights (MIDI OUT)** and enable the key-lighting feature on it
   (*key lights*). As of today there is no mode that splits hands by channel.
4. Copy the left-hand / right-hand **channel numbers** shown
   by Synthesia into the web page (Colors section). By default the
   firmware expects left = channel 1, right = channel 2; any other channel
   uses the third color.
5. (Optional) Disable features other than key lighting to avoid getting
   feedback from the piano itself (*Prevent "local" notes* does not seem to do it).

## 4. Aligning the LEDs with the keys

In the Alignment section of the web page:

- Pick a **density preset** (60 / 96 / 120 / 144 / 240 / 332 LEDs/m)
  which pre-fills "LEDs per key", then fine-tune that ratio **as a decimal**: if
  the alignment gradually drifts along the keyboard, adjust by
  ±0.01–0.05.
- **Offset**: aligns the first LED with the first key.
- **Reversed**: if the strip is laid out with its cable on the right side.
- Clicking the keys of the **virtual keyboard** actually lights the LEDs.
"Save preferences" stores everything in flash.

## 5. Playing an acoustic piano + using the microphone

This optional feature lets Synthesia's *"wait for note"* practice mode work with
a real acoustic piano: an I2S microphone listens to the room and the firmware
sends the notes it hears back over BLE/MIDI.

1. In the **Hardware (mic)** section of the web page, tick *Enable I2S microphone*,
   choose the three GPIOs, then **Save preferences** and **reboot** (enabling the
   mic or changing a GPIO needs a reboot; gain and threshold apply live).
2. In Synthesia, also select **Piano-Lights** as a MIDI **input** (Settings →
   Music Devices), so the notes detected by the microphone reach the game.
3. Adjust **gain** so the input-level meter reacts clearly to your playing without
   pinning at the top, and **threshold** to trade sensitivity against false
   positives. The detected note is shown live under the meter.

### Limitations

The firmware does not attempt blind polyphonic transcription.
Instead it verifies the notes Synthesia already expects: the keys it lights
up are exactly the notes the player should hit, so a small Goertzel filter bank
only checks whether each expected note's fundamental and first harmonics are
present in a 100 ms window after a percussive onset.

## 6. Architecture

| File                | Role                                                        |
|---------------------|-------------------------------------------------------------|
| `PianoLights.ino`   | BLE/MIDI, note→LED mapping, WiFi STA/AP, HTTP API, NVS       |
| `PianoLights.h`     | Configuration page (standalone HTML/CSS/JS, in PROGMEM)      |
| `PianoLightsMic.h`  | I2S microphone capture, onset + Goertzel note detection, calibration |

HTTP API: `GET /api/config`, `POST /api/config`, `POST /api/test`
(`{note, on, ch}`), `POST /api/alloff`, `POST /api/wifi`, `POST /api/reboot`,
`GET /api/mic/status`, `GET /api/mic/cal`, `POST /api/mic/cal`
(`{note}` arms a capture, `{note:-1}` disarms, `{clear:true}` erases calibration).
