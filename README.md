# Morse Telegraph 📟

```
.-- .... .- -   .... .- - ....   --. --- -..   .-- .-. --- ..- --. .... -
```
*"What hath God wrought", the first long-distance Morse message, sent in 1844. This repo is the 2026 sequel.*

Send a Telegram message from your phone, and a little cube across the living room beeps it out in Morse code. Tap the cube back, and your dots and dashes land in Telegram as plain text. Your friend reads `HELLO`, not `.... . .-.. .-.. ---`.

It is completely useless. That is the point.

---

## Why build this?

Every notification today is the same buzz and the same banner. The idea: a nice object for nerds that sits in the living room and taps out your incoming messages like a 19th-century telegraph office. A sound in the room instead of a banner on a screen.

I had never built any hardware before. So the rule from day one was: **prove it's fun before making it pretty.** No casing, no soldering, no shopping list. Just: can a message become a beep, and does hearing it make you smile?

## How it got here (the thought process)

Each step answered exactly one question, and each one is kept in this repo because of that.

**Plan A: the stuff already on the desk.** A Raspberry Pi and a synthesizer with a USB MIDI port. The Pi would read messages and play them as notes on the synth. Zero purchases. Then the Pi turned out to have no Wi-Fi antenna and no way to plug it into the network. Plan A was over.

**Plan B: the Mac.** Everything moved to the laptop. A Python script polls a Telegram bot and plays the Morse through the Mac's speakers. *Question answered: can a message become sound?* Yes: it beeped `HI` on the first run. Fun, confirmed.

**Why Telegram and not WhatsApp?** The long-term wish list includes WhatsApp, Signal and Discord. But Telegram is the one with a free, open bot API you can talk to in ten lines of code. Start where the door is open.

**Plan C: the browser.** A single HTML file that plays the same Morse with no install. *Question answered: can other people hear it without owning anything?* Yes, which makes it easy to show the idea to anyone.

**Plan D: leave the computer.** The first two versions need a computer awake with a terminal open. That's not an object, that's a chore. Enter the **M5Stack ATOM Echo**: a 2 cm cube, about €17, with a speaker, an LED and a button for a whole top face. Nothing to solder. Plug it into any USB charger and it *is* the telegraph. And because it has a button, it can also *send*: the button became a telegraph key.

**Decisions along the way, and why:**
- **Same Morse timing in all three versions**, ported symbol for symbol. If the cube sounds different from the prototype, you're debugging two things at once.
- **No Wi-Fi password or token in the code.** The cube opens its own setup page on first boot. That's what made it safe to publish this repo.
- **Typing timing separate from playback timing.** A machine plays dots at 150 ms; a human thumb absolutely does not. The key is deliberately more forgiving.
- **Two processor cores, two jobs.** One core waits on Telegram (up to 20 s at a time), the other listens to your thumb. Otherwise the key would freeze every time the network thought about things.
- **Skip what Morse can't say.** Accents, emoji and punctuation are dropped in silence rather than guessed at. Morse predates the 🎉.

## What's in the box

| | what it is | what it proved |
|---|---|---|
| [`prototype/`](prototype/) | Python script, plays Morse through a Mac's speakers | that a message *can* become sound |
| [`web/`](web/) | single HTML file, Web Audio, no install | that other people can hear it without owning anything |
| [`firmware/`](firmware/) | Arduino sketch for an M5Stack ATOM Echo | that it can leave the computer entirely |

---

## The Morse itself (same everywhere, on purpose)

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

## Running the Mac prototype

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

## The hardware version 🧊

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

## The wall: one bot cannot drive two devices

This is the architectural wall worth knowing before you build a second one.

Telegram's `getUpdates` hands each message to whichever client asks first and
then marks it consumed. Two devices polling one bot steal messages from each
other at random; neither gets a reliable feed. The same applies to running the
Python prototype while the hardware is on — one of them will swallow everything.

So this design is deliberately **one bot, one device, yours**. Fanning out to
friends needs a server in the middle that polls Telegram once and republishes to
each device over something like MQTT, with per-device pairing. That isn't here.

## What's next

- **A proper casing.** Step two, now that the fun is proven: wood, brass, something that deserves a shelf. Ideally made and assembled in France, with a nod to the heritage of the telegraph.
- **More messengers.** WhatsApp, Signal, Discord, not just Telegram.
- **Telegraphs for friends.** Needs the bridge server described above, so several cubes can share messages.

## Licence

None yet, which legally means all rights reserved. Add one if you want others to be able to use this.

`...-.-` (end of work)
