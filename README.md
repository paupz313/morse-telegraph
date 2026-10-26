# Morse Telegraph

Send a Telegram message, hear it beeped out in Morse code on a device across the
room. Tap the device's button back, and your Morse arrives in Telegram as plain
readable text.

Three pieces, built in that order, each one kept here because each one answers a
different question.

| | what it is | what it proved |
|---|---|---|
| [`prototype/`](prototype/) | Python script, plays Morse through a Mac's speakers | that a message *can* become sound |
| [`web/`](web/) | single HTML file, Web Audio, no install | that other people can hear it without owning anything |
| [`firmware/`](firmware/) | Arduino sketch for an M5Stack ATOM Echo | that it can leave the computer entirely |

The interesting one is the firmware. The first two need a computer sitting there,
awake, with a terminal open. The Atom Echo is a 2 cm cube that costs about €17,
plugs into any USB charger, and is the whole telegraph.

---

## The Morse itself

Identical in all three, and deliberately so — the web and firmware versions were
ported symbol-for-symbol from the Python, not rewritten.

```
dot        1 unit        unit = 0.15 s
dash       3 units
gap between symbols   1 unit
gap between letters   3 units
gap between words     7 units
tone                  600 Hz sine, 10 ms fade on each edge
```

The 10 ms fade matters more than it looks. A sine wave that starts and stops at
full amplitude clicks audibly; ramping the edges removes it. All three
implementations do this, by three different mechanisms — numpy array slicing, a
Web Audio `linearRampToValueAtTime`, and a per-sample envelope in C.

A–Z and 0–9 only. Anything else — punctuation, accents, emoji — is skipped in
silence rather than guessed at.

---

## Running the prototype

Needs Python 3 and a Telegram bot token from
[BotFather](https://t.me/botfather).

Homebrew's Python refuses to install into itself (PEP 668), so use a virtual
environment — this is the step that trips people up:

```bash
python3 -m venv .venv
.venv/bin/pip install requests numpy sounddevice
```

Paste your token into `BOT_TOKEN` at the top of the script, then:

```bash
.venv/bin/python prototype/morse_telegraph_speaker.py
```

It beeps `HI` on startup, then plays anything you send your bot. Note it must be
run with `.venv/bin/python` — a bare `python3` won't see the packages.

## Running the web version

Open `web/index.html`. No build step, no dependencies, no network. Type
something and press play. Browsers require a real click before they'll make
sound, so the first press is the one that unlocks audio.

---

## The hardware version

An **M5Stack ATOM Echo** — ESP32 with an I2S speaker, an RGB LED, and a button
that is the entire top face. Nothing to solder.

### Build and flash

```bash
brew install arduino-cli
arduino-cli config init
arduino-cli config add board_manager.additional_urls \
  https://espressif.github.io/arduino-esp32/package_esp32_index.json
arduino-cli core update-index
arduino-cli core install esp32:esp32@2.0.17
arduino-cli lib install "ArduinoJson" "WiFiManager" "Adafruit NeoPixel"
```

The core version is pinned to **2.0.17** on purpose. ESP32 core 3.x changed the
I2S audio API, and this sketch uses the 2.x `driver/i2s.h` interface.

```bash
arduino-cli compile --fqbn esp32:esp32:m5stack-atom firmware/morse_telegraph_atom
arduino-cli upload -p /dev/cu.usbserial-XXXX --fqbn esp32:esp32:m5stack-atom firmware/morse_telegraph_atom
```

Find your port with `ls /dev/cu.*`. On recent macOS the ATOM Echo's CH9102
USB-serial chip works with no driver install.

### First run

No Wi-Fi credentials or token are compiled in. On first boot the LED pulses blue
and the device creates a Wi-Fi network called `MorseTelegraph-XXXX`. Join it from
a phone; a setup page opens by itself. Choose your network, enter its password,
paste your bot token, save.

It must be a **2.4 GHz** network. The ESP32 cannot see 5 GHz bands at all.

Credentials are stored in the ESP32's flash, not in this source, which is why
this repository is safe to publish. They survive reflashing.

### Using the key

The button is a telegraph key, with a live sidetone in the speaker while you
hold it down.

| | |
|---|---|
| quick tap | dot |
| longer press | dash |
| pause ~0.6 s | ends the letter |
| pause ~1.4 s | adds a space |
| pause ~3 s | sends it, then two green blinks |
| hold 3 s | discards the draft |
| press while it beeps | stops incoming playback |

It replies to whoever last messaged the bot, so send it one message first to
introduce yourself.

To reopen the setup page: unplug, hold the button, plug back in.

LED: blue pulse = connecting, green = idle, white = beeping or keying,
amber = unsent keyed text waiting, red = error.

### Tuning it to your thumb

The dot/dash boundary is 250 ms, which is a guess about an average hand. Every
press prints its true duration to serial:

```bash
arduino-cli monitor -p /dev/cu.usbserial-XXXX -c baudrate=115200
```

Tap a few letters, read the numbers, and adjust the `KEY_*` constants near the
top of the sketch. These are kept separate from the playback timing on purpose —
no human keys to 150 ms precision, and the two jobs want different tolerances.

---

## One bot cannot drive two devices

This is the architectural wall worth knowing before you build a second one.

Telegram's `getUpdates` hands each message to whichever client asks first and
then marks it consumed. Two devices polling one bot steal messages from each
other at random; neither gets a reliable feed. The same applies to running the
Python prototype while the hardware is on — one of them will swallow everything.

So this design is deliberately **one bot, one device, yours**. Fanning out to
friends needs a server in the middle that polls Telegram once and republishes to
each device over something like MQTT, with per-device pairing. That isn't here.

## Licence

None yet, which legally means all rights reserved. Add one if you want others to
be able to use this.
