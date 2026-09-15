# Haptic-Sense Data Collection — Instructions

Thank you for helping with this! You do not need to know anything about electronics or
programming to do this correctly. Everything below is written so you can follow it exactly,
step by step. It should take about 60–90 minutes total, and it's fine to take breaks whenever
you want — just not in the middle of a single 45-second recording.

---

## 1. The one thing you actually need to do

1. Open a terminal window (if one isn't already open for you).
2. Type exactly this, then press Enter:

   ```
   cd ~/haptic-sense && bash collect_data.sh
   ```

3. From that point on, the program on screen will tell you exactly what to do, one step at a
   time. Mostly this means: **read the instruction on screen, get in position, press Enter, and
   wave your hand (or the object it names) in front of the sensor for the count given.**

That's the whole task. Everything else in this document is context, technique tips, and what to
do if something looks wrong — you don't need to memorize it, just glance at it if you're unsure.

---

## 2. What you're looking at

The device on the desk is a small circuit board with a sensor mounted on it that can tell how far
away an object is (like a tiny radar, using light instead of radio). We're recording example data
of hands and objects moving toward and away from it, at different speeds and angles, so that the
project's software can learn to recognize "something is approaching quickly" versus "nothing much
is happening."

**Do not worry about getting anything "wrong."** There's no way for you to damage anything by
waving a hand near a sensor. The worst case is a recording that isn't very useful, which is easy
to just redo.

---

## 3. What should already be on the desk

If everything was set up for you properly, you should have:

- The circuit board, already plugged into the computer via USB, sitting on the desk facing
  outward (there should be a small sensor window on it — that's what you're waving in front of).
- A dark piece of cloth or a dark sleeve.
- A light-colored clipboard or book.
- A phone or laptop you can hold up (doesn't need to be turned on).
- A terminal window, ideally already open to the right folder.

If any of these are missing, use whatever reasonably matches the description (e.g., any dark
piece of fabric, any light rigid object) — it doesn't need to be exact.

---

## 4. Quick reference — what "surface," "angle," and "speed" mean

The program will walk you through all of these automatically and tell you exactly which one is
next — you don't need to plan this yourself. This table is just here so you know what to expect.

| Category | Options | What to do |
|---|---|---|
| **Surface** | bare hand, dark cloth, light clipboard, phone/laptop | Hold the named object facing the sensor |
| **Angle** | straight-on, from the left, from the right, from above, from below | Approach the sensor from that direction |
| **Speed** | slow, normal, fast | How quickly you sweep in and out |

For every combination, the motion is the same: **start with your hand/object at about arm's
length (roughly 1.3 meters — about one big step back with your arm out), sweep smoothly all the
way in until it's almost touching the sensor (a couple of centimeters away), then sweep back out,
and repeat that in-and-out motion continuously** for the full recording, at the pace shown on
screen.

Keep the motion smooth and continuous rather than a series of quick pokes — one steady sweep in,
one steady sweep out, over and over.

---

## 5. Things you should NOT do

- Don't unplug the USB cable, unless a message specifically tells you to as a troubleshooting
  step.
- Don't press any buttons on the circuit board itself.
- Don't touch, tug, or reposition any of the wires connected to it.
- Don't move the board itself once it's positioned.
- Don't close the terminal window while a recording is in progress (you'll see
  `>>> RECORDING NOW <<<` on screen when one is running).
- Don't open or use any other programs on the computer while this is running.
- Don't delete any files.
- Don't try to fix anything yourself if something looks broken — see the section below instead.

It's completely fine to press Enter and take your time between steps — the program waits for you.

---

## 6. If something looks wrong

The program checks itself automatically at the very start (a 12-second test) and will tell you
in plain language if something is wrong before any real recording begins. If you see a message
starting with **"STOP"** or **"PROBLEM"**:

1. Stop — don't continue past that point, and don't try to fix it yourself.
2. Leave the computer and the board exactly as they are.
3. Make a note of exactly what the screen said (a photo of the screen is fine).
4. Wait for the project owner to come back, or let them know as soon as you can.

If a recording is interrupted partway through (for example, the computer needs to be paused for
some reason), it's safe to press **Ctrl and C together** to stop it. Nothing will be damaged, and
anything already saved stays saved — the interrupted recording can simply be skipped or redone
later.

If you ever need to stop for the day before finishing everything, that's fine too. Just note down
the last recording number you completed (the screen shows "Recording X of 60") and stop there —
don't try to restart from the middle yourself.

---

## 7. When you're done

The screen will show **"ALL RECORDINGS COMPLETE"** once every combination has been recorded.
At that point:

- You're finished — thank you.
- Don't touch, move, or delete anything.
- Just leave the computer as it is.
