#!/usr/bin/env python3
"""Talk the board through language changes out loud, and check each one in its
serial log.

    voice_demo.py [--port PORT] [--languages bg,es] [--no-reset] [--log FILE]

For each language: "Change your language to <it>." in English, a question in that
language, "English, please" in that language, and a question in English. Every
line is synthesised with /v2/speak and played through this Mac's speaker
(afplay) to the board's microphone, once the board has answered the last.
The board is reset first, so the session is fresh; --no-reset uses the one
running.

A line counts when the board heard it and answered; a language change counts
when set_voice ran and UpdateSpeak, UpdatePrompt and UpdateListen were all
acknowledged -- or, past the board's in-place limit, when it reopened the
session with the new voice. A line nobody heard is played once more, and so is
a change request the agent answered without changing voice ("I can if you
want"). If the board is still not in English after that, the run stops there:
every later line would go to the wrong listen model.

Needs pyserial (voice_demo.sh finds the ESP-IDF Python, which has it) and
DEEPGRAM_API_KEY, which is never printed. Speech is cached in
build/voice_demo/, and the board's whole log is written there too.
"""
import argparse
import glob
import hashlib
import json
import os
import platform
import re
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.request

try:
    import serial
except ImportError:
    sys.exit("pyserial is missing: run voice_demo.sh, which uses the ESP-IDF Python")

HERE = os.path.dirname(os.path.abspath(__file__))
CACHE = os.path.join(HERE, "build", "voice_demo")
SPEAK_URL = "https://api.deepgram.com/v2/speak"
ME = "flux-kit-en"   # the voice the "user" speaks English in

# One voice per language, as main/voices.c features it, and what to say in it.
LANGS = {
    "en": ("flux-kit-en", "English", "What is the capital of Italy?", None),
    "es": ("flux-carmen-es", "Spanish", "¿Cuál es la capital de Francia?",
           "En inglés, por favor."),
    "fr": ("flux-camille-fr", "French", "Quelle est la capitale de l'Espagne ?",
           "En anglais, s'il te plaît."),
    "de": ("flux-sonja-de", "German", "Was ist die Hauptstadt von Italien?",
           "Auf Englisch, bitte."),
    "hi": ("flux-anjali-hi", "Hindi", "भारत की राजधानी क्या है?",
           "अंग्रेज़ी में बात करो।"),
    "ru": ("flux-olga-ru", "Russian", "Какая столица Германии?",
           "По-английски, пожалуйста."),
    "pt": ("flux-bianca-pt", "Portuguese", "Qual é a capital da Espanha?",
           "Em inglês, por favor."),
    "ja": ("flux-ayumi-ja", "Japanese", "フランスの首都はどこですか？",
           "英語でお願いします。"),
    "it": ("flux-sofia-it", "Italian", "Qual è la capitale della Francia?",
           "In inglese, per favore."),
    "nl": ("flux-anouk-nl", "Dutch", "Wat is de hoofdstad van Duitsland?",
           "In het Engels, alsjeblieft."),
    "bg": ("flux-desislava-bg", "Bulgarian", "Коя е столицата на Франция?",
           "Английски, моля."),
}

B, DIM, GRN, RED, YEL, CYN, OFF = ("\033[1m", "\033[2m", "\033[32m", "\033[31m",
                                    "\033[33m", "\033[36m", "\033[0m")
if not sys.stdout.isatty():
    B = DIM = GRN = RED = YEL = CYN = OFF = ""


# ---------------------------------------------------------------- speech

def speech(model, text):
    """A WAV of `text` in `model`, synthesised once and cached."""
    os.makedirs(CACHE, exist_ok=True)
    tag = hashlib.sha1(f"{model}|{text}".encode()).hexdigest()[:12]
    path = os.path.join(CACHE, f"{model}-{tag}.wav")
    if os.path.exists(path):
        return path
    url = f"{SPEAK_URL}?model={model}&encoding=linear16&sample_rate=16000&container=wav"
    req = urllib.request.Request(url, data=json.dumps({"text": text}).encode(),
                                 headers={"Authorization": f"Token {os.environ['DEEPGRAM_API_KEY']}",
                                          "Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=30) as r:
            data = r.read()
    except urllib.error.HTTPError as e:
        sys.exit(f"/v2/speak refused {model}: HTTP {e.code}"
                 + (" -- check DEEPGRAM_API_KEY" if e.code in (401, 402, 403) else ""))
    with open(path, "wb") as f:
        f.write(data)
    return path


def play(path):
    subprocess.run(["afplay", path], check=True)


# ---------------------------------------------------------------- the board

class Board:
    """Reads the serial log on a thread; the driver waits on what it says."""

    EVENTS = [
        (re.compile(r"voices: voice: flux-\w+-(\w+)"), "boot_lang"),
        (re.compile(r"SettingsApplied"), "live"),
        (re.compile(r"dg_agent: user: (.*)"), "heard"),
        (re.compile(r"dg_agent: assistant: (.*)"), "said"),
        (re.compile(r"voice change requested: (\S+)"), "voice"),
        (re.compile(r"dg_agent: (SpeakUpdated|PromptUpdated|ListenUpdated)"), "ack"),
        (re.compile(r"agent finished speaking"), "done"),
        (re.compile(r"reopening session to apply"), "reload"),
        (re.compile(r"session stopped"), "stopped"),
        (re.compile(r"never acknowledged|refused -- reopening|server error"), "trouble"),
        (re.compile(r"EVT beh INITIALIZING->"), "ready"),
        (re.compile(r"EVT beh SPEAKING->"), "quiet"),
    ]

    def __init__(self, port, log, reset):
        self.port, self.events, self.lock = port, [], threading.Lock()
        self.log = open(log, "wb")
        self.ser = serial.Serial(port, timeout=0.2)
        if reset:
            self.ser.dtr = False
            self.ser.rts = True
            time.sleep(0.15)
            self.ser.rts = False
        self.running = True
        self.ended = False   # a session that stops does not come back by itself
        self.reloading = False   # ...unless the board stopped it to reopen it
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self):
        buf = b""
        while self.running:
            try:
                data = self.ser.read(4096)
            except serial.SerialException:
                time.sleep(0.5)   # USB CDC re-enumerates on reset
                try:
                    self.ser.close()
                    self.ser = serial.Serial(self.port, timeout=0.2)
                except serial.SerialException:
                    pass
                continue
            if not data:
                continue
            self.log.write(data)
            self.log.flush()
            buf += data
            *lines, buf = buf.split(b"\n")
            for raw in lines:
                line = raw.decode("utf-8", "replace").strip()
                for rx, kind in self.EVENTS:
                    m = rx.search(line)
                    if m:
                        with self.lock:
                            if kind == "reload":
                                self.reloading = True
                            elif kind == "live":
                                self.reloading = False
                            elif kind == "stopped" and self.reloading:
                                kind = "restart"
                            elif kind == "stopped":
                                self.ended = True
                            self.events.append((kind, m.group(1) if m.groups() else ""))
                        self._show(kind, m.group(1) if m.groups() else "")
                        break

    @staticmethod
    def _show(kind, arg):
        if kind == "heard":
            print(f"    {DIM}board heard{OFF}  {arg}", flush=True)
        elif kind == "said":
            print(f"    {CYN}agent{OFF}        {arg}", flush=True)
        elif kind == "voice":
            print(f"    {B}voice ->{OFF}     {arg}", flush=True)
        elif kind == "ack":
            print(f"    {DIM}server       {arg}{OFF}", flush=True)
        elif kind == "trouble":
            print(f"    {RED}trouble{OFF}", flush=True)

    def mark(self):
        with self.lock:
            return len(self.events)

    def since(self, mark):
        with self.lock:
            return self.events[mark:]

    def wait(self, kind, mark, secs):
        """The first `kind` event after `mark`, "stopped" if the session ended
        first, or None after `secs`."""
        end = time.time() + secs
        while time.time() < end:
            if self.ended:
                return "stopped"
            for k, arg in self.since(mark):
                if k == "stopped":
                    return "stopped"
                if k == kind:
                    return arg or True
            time.sleep(0.2)
        return None

    def close(self):
        self.running = False
        time.sleep(0.3)
        self.ser.close()
        self.log.close()


# ---------------------------------------------------------------- the demo

class SessionEnded(Exception):
    pass


def until_quiet(board, mark):
    """Until the board has finished PLAYING what it was sent. AgentAudioDone
    only means the server has finished sending; the board plays on for a second
    or more, and a line spoken over that is lost to its echo handling -- every
    line missed in the first runs was."""
    if board.wait("done", mark, 15) == "stopped":
        raise SessionEnded
    quiet_mark = board.mark()
    if not any(k == "quiet" for k, _ in board.since(mark)):
        if board.wait("quiet", quiet_mark, 10) == "stopped":
            raise SessionEnded
    time.sleep(0.6)


def step(board, label, model, text, kind, lang_after, current, results):
    """Say one line and wait for the board to finish with it."""
    row = {"label": label, "heard": False, "answered": False, "voice": None,
           "acks": set(), "retried": False, "again": False, "kind": kind, "reloaded": False}
    results.append(row)   # now, so a session that ends mid-step still shows it
    for attempt in (1, 2):
        if board.ended:
            raise SessionEnded
        mark = board.mark()
        print(f"\n{B}you{OFF}  {text}", flush=True)
        play(speech(model, text))
        heard = board.wait("heard", mark, 8)
        if heard == "stopped":
            raise SessionEnded
        if heard:
            row["heard"] = True
            break
        print(f"    {YEL}not heard{OFF}" + (", saying it again" if attempt == 1 else ""), flush=True)
        row["retried"] = True
    if not row["heard"]:
        return current
    until_quiet(board, mark)
    evs = board.since(mark)
    row["answered"] = any(k == "said" for k, _ in evs)
    row["voice"] = next((a for k, a in evs if k == "voice"), None)

    if kind == "switch" and row["voice"] is None:
        # No change: it asked instead ("I can switch if you want"), answered
        # without calling set_voice, or misheard. The request again covers all
        # three; a "yes" only the first, and was itself misheard on the board.
        row["again"] = True
        mark2 = board.mark()
        print(f"\n{B}you{OFF}  {text}   {DIM}(no change yet: asking again){OFF}", flush=True)
        play(speech(model, text))
        heard2 = board.wait("heard", mark2, 8)
        if heard2 == "stopped":
            raise SessionEnded
        if heard2:
            until_quiet(board, mark2)
        evs += board.since(mark2)
        row["voice"] = next((a for k, a in evs if k == "voice"), None)
        # The confirmation that counts is the one after the second ask.
        row["answered"] = any(k == "said" for k, _ in board.since(mark2))

    if kind == "switch" and row["voice"] and any(k == "reload" for k, _ in board.since(mark)):
        # After INPLACE_CHANGES_MAX changes in one session (main/dg_agent.c),
        # the board applies the next by reopening the session, so there are no
        # acknowledgements to wait for: a fresh Settings carries the voice.
        row["reloaded"] = True
        print(f"    {DIM}board        reopening the session with the new voice{OFF}", flush=True)
        live = board.wait("live", mark, 20)
        if live != True:
            raise SessionEnded
        board.wait("ready", mark, 10)
        time.sleep(1.0)
        if row["voice"].endswith(f"-{lang_after}"):
            current = lang_after
    elif kind == "switch" and row["voice"]:
        # UpdateListen goes after the confirmation has been spoken.
        end = time.time() + 6
        while time.time() < end and "ListenUpdated" not in {a for k, a in board.since(mark) if k == "ack"}:
            time.sleep(0.2)
        row["acks"] = {a for k, a in board.since(mark) if k == "ack"}
        if row["voice"].endswith(f"-{lang_after}"):
            current = lang_after
    time.sleep(0.5)
    return current


def verdict(row):
    if not row["heard"]:
        return False, "not heard"
    if row["kind"] == "switch":
        if not row["voice"]:
            return False, "no voice change"
        missing = {"SpeakUpdated", "PromptUpdated", "ListenUpdated"} - row["acks"]
        if row["reloaded"]:
            missing = set()
        if missing:
            return False, "no " + ", ".join(sorted(m[:-7] for m in missing))
        if not row["answered"]:
            return False, "confirmation not spoken"
        return True, row["voice"] + ("  (session reopened)" if row["reloaded"] else "")
    return row["answered"], "answered" if row["answered"] else "no answer"


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--port", help="serial port (default: the only /dev/cu.usbmodem*)")
    ap.add_argument("--languages", default="bg,es",
                    help=f"comma-separated, from {','.join(k for k in LANGS if k != 'en')}")
    ap.add_argument("--no-reset", action="store_true", help="use the session already running")
    ap.add_argument("--log", default=os.path.join(CACHE, "serial.log"))
    args = ap.parse_args()

    if platform.system() != "Darwin":
        sys.exit("plays through afplay, so macOS only")
    if not os.environ.get("DEEPGRAM_API_KEY"):
        sys.exit("DEEPGRAM_API_KEY is not set")
    langs = [l.strip() for l in args.languages.split(",") if l.strip()]
    bad = [l for l in langs if l not in LANGS or l == "en"]
    if bad:
        sys.exit(f"unknown language(s): {','.join(bad)}")
    port = args.port
    if not port:
        ports = glob.glob("/dev/cu.usbmodem*")
        if len(ports) != 1:
            sys.exit(f"found {len(ports)} boards ({', '.join(ports) or 'none'}): pass --port")
        port = ports[0]

    # Synthesise everything up front, so no line waits on the network mid-demo.
    print(f"{DIM}preparing speech...{OFF}", flush=True)
    for l in langs:
        model, name, q, back = LANGS[l]
        for m, t in ((ME, f"Change your language to {name}."), (model, q), (model, back)):
            speech(m, t)
    speech(ME, LANGS["en"][2])

    os.makedirs(os.path.dirname(os.path.abspath(args.log)), exist_ok=True)
    board = Board(port, args.log, reset=not args.no_reset)
    results = []
    try:
        if not args.no_reset:
            print(f"{DIM}resetting the board, waiting for its session...{OFF}", flush=True)
            if board.wait("live", 0, 60) is not True:
                sys.exit(f"no session in 60 s -- see {args.log}")
            # Straight in: the board ends a session about 20 s after the last
            # thing it heard, and the greeting, if any, is short.
            board.wait("ready", 0, 10)
            until_quiet(board, 0) if any(k == "done" for k, _ in board.since(0)) else time.sleep(1.0)
        # Start from English: a voice left over from an earlier run (saved in
        # NVS) would hear "Change your language to ..." through the wrong listen model.
        boot = next((a for k, a in board.since(0) if k == "boot_lang"), "en")
        current = boot if boot in LANGS else "en"
        if current != "en":
            model, name, _, back = LANGS[current]
            print(f"\n{B}--- the board starts in {name}: back to English first ---{OFF}", flush=True)
            current = step(board, "start in English", model, back, "switch", "en", current, results)
            if current != "en":
                print(f"\n{RED}still in {name}: stopping here{OFF}", flush=True)
                langs = []
        for l in langs:
            model, name, q, back = LANGS[l]
            print(f"\n{B}--- {name} ---{OFF}", flush=True)
            current = step(board, f"to {name}", ME, f"Change your language to {name}.", "switch", l, current, results)
            current = step(board, f"{name} question", model, q, "q", l, current, results)
            current = step(board, "back to English", model, back, "switch", "en", current, results)
            if current != "en":
                print(f"\n{RED}still in {name}: stopping here{OFF}", flush=True)
                break
            current = step(board, "English question", ME, LANGS["en"][2], "q", "en", current, results)
    except SessionEnded:
        print(f"\n{RED}the board ended its session{OFF}", flush=True)
    except KeyboardInterrupt:
        print("\nstopped", flush=True)
    finally:
        board.close()

    print(f"\n{B}Summary{OFF}")
    passed = 0
    for row in results:
        ok, why = verdict(row)
        passed += ok
        notes = [n for n, on in (("said twice", row["retried"]), ("asked twice", row["again"])) if on]
        print(f"  {GRN + 'pass' if ok else RED + 'FAIL'}{OFF}  {row['label']:<20} {why}"
              + (f"  {DIM}({', '.join(notes)}){OFF}" if notes else ""))
    print(f"\n{passed}/{len(results)} steps passed. Board log: {args.log}")
    sys.exit(0 if results and passed == len(results) else 1)


if __name__ == "__main__":
    main()
