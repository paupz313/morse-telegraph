#!/usr/bin/env python3
"""
MORSE TELEGRAPH -- v1 prototype (Mac speaker version, no extra hardware)

WHAT THIS DOES:
- Polls a Telegram bot every few seconds for new messages
- When a message arrives, plays it in Morse code as beep tones through
  your Mac's built-in speakers (short beep = dot, long beep = dash)

SETUP STEPS (do these before running):
1. In Terminal:
     pip3 install requests numpy sounddevice

2. Create your Telegram bot:
     - Open Telegram, search for "BotFather", start a chat
     - Send: /newbot
     - Follow the prompts (give it a name), it gives you a TOKEN like
       "123456789:AAExampleTokenTextGoesHere"
     - Paste that into BOT_TOKEN below

3. Run it:
     python3 morse_telegraph_speaker.py

4. Message your bot on Telegram from your phone. Within ~2 seconds you
   should hear your message played back in Morse code beeps.
"""

import time
import requests
import numpy as np
import sounddevice as sd

# ---------- FILL THIS IN ----------
BOT_TOKEN = "PASTE_YOUR_TOKEN_FROM_BOTFATHER_HERE"
# -----------------------------------

BASE_URL = f"https://api.telegram.org/bot{BOT_TOKEN}"
CHECK_INTERVAL_SECONDS = 2

BEEP_FREQ_HZ = 600      # pitch of the beep -- change to taste
SAMPLE_RATE = 44100

# Morse timing (seconds). Adjust DOT_SEC to change overall speed.
DOT_SEC = 0.15
DASH_SEC = DOT_SEC * 3
SYMBOL_GAP_SEC = DOT_SEC
LETTER_GAP_SEC = DOT_SEC * 3
WORD_GAP_SEC = DOT_SEC * 7

MORSE_TABLE = {
    'A': '.-',    'B': '-...',  'C': '-.-.',  'D': '-..',
    'E': '.',     'F': '..-.',  'G': '--.',   'H': '....',
    'I': '..',    'J': '.---',  'K': '-.-',   'L': '.-..',
    'M': '--',    'N': '-.',    'O': '---',   'P': '.--.',
    'Q': '--.-',  'R': '.-.',   'S': '...',   'T': '-',
    'U': '..-',   'V': '...-',  'W': '.--',   'X': '-..-',
    'Y': '-.--',  'Z': '--..',
    '0': '-----', '1': '.----', '2': '..---', '3': '...--',
    '4': '....-', '5': '.....', '6': '-....', '7': '--...',
    '8': '---..', '9': '----.',
}


def make_tone(duration_sec):
    t = np.linspace(0, duration_sec, int(SAMPLE_RATE * duration_sec), endpoint=False)
    tone = 0.4 * np.sin(2 * np.pi * BEEP_FREQ_HZ * t)

    # short fade in/out to avoid audible clicks at start/end of each beep
    fade_len = min(int(SAMPLE_RATE * 0.01), len(tone) // 2)
    if fade_len > 0:
        fade = np.linspace(0, 1, fade_len)
        tone[:fade_len] *= fade
        tone[-fade_len:] *= fade[::-1]

    return tone.astype(np.float32)


def beep(duration_sec):
    tone = make_tone(duration_sec)
    sd.play(tone, samplerate=SAMPLE_RATE)
    sd.wait()
    time.sleep(SYMBOL_GAP_SEC)


def play_morse_message(text):
    print(f"Playing message: {text}")
    for char in text.upper():
        if char == ' ':
            time.sleep(WORD_GAP_SEC)
            continue

        code = MORSE_TABLE.get(char)
        if code is None:
            continue  # skip unsupported characters (punctuation etc.)

        for symbol in code:
            if symbol == '.':
                beep(DOT_SEC)
            elif symbol == '-':
                beep(DASH_SEC)

        time.sleep(LETTER_GAP_SEC - SYMBOL_GAP_SEC)


def get_updates(offset=None):
    params = {"timeout": 10}
    if offset:
        params["offset"] = offset
    resp = requests.get(f"{BASE_URL}/getUpdates", params=params, timeout=15)
    resp.raise_for_status()
    return resp.json().get("result", [])


def main():
    print("Starting Morse Telegraph...")
    play_morse_message("HI")  # boot confirmation beep

    last_update_id = None

    while True:
        try:
            updates = get_updates(offset=last_update_id)
            for update in updates:
                last_update_id = update["update_id"] + 1
                message = update.get("message", {})
                text = message.get("text")
                from_name = message.get("from", {}).get("first_name", "unknown")

                if text:
                    print(f"New message from {from_name}: {text}")
                    play_morse_message(text)

        except requests.exceptions.RequestException as e:
            print(f"Network error, retrying: {e}")
            time.sleep(5)
            continue

        time.sleep(CHECK_INTERVAL_SECONDS)


if __name__ == "__main__":
    main()
