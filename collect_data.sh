#!/bin/bash
# Haptic-Sense Block 6 -- guided data collection script.
# Run with:  cd ~/haptic-sense && bash collect_data.sh
# Safe to Ctrl-C at any time between prompts; anything already saved stays saved.

set -u
LOGDIR="$HOME/haptic-sense/logs"
mkdir -p "$LOGDIR"
BAUD="115200"
CAPTURE_SECONDS=45

echo "=================================================="
echo " Haptic-Sense -- Block 6 Data Collection"
echo "=================================================="
echo ""
echo "This script will walk you through everything, one step at a time."
echo "You will just be pressing Enter and waving your hand -- nothing else."
echo ""

# ---------------------------------------------------------------
# Port auto-detection + retry helpers.
# The board's serial port name (e.g. /dev/ttyACM0) can shift to
# /dev/ttyACM1 or higher if it gets unplugged/replugged, and there
# can be a brief failure right after plugging in (something else on
# the system probing the new port). detect_port() re-scans fresh
# every time rather than trusting a value found earlier, and
# run_capture() retries past a fast/early failure instead of giving
# up on the first try.
# ---------------------------------------------------------------
detect_port() {
  local candidates
  candidates=(/dev/ttyACM*)
  if [ ! -e "${candidates[0]:-}" ]; then
    return 1
  fi
  # Prefer the highest-numbered node -- the most recently created one
  # after a replug. Only this one line goes to stdout (it's the
  # function's real return value); everything else in this file uses
  # stderr for messages so redirecting a capture's stdout to hide the
  # raw serial spam never hides a status/retry message too.
  echo "${candidates[-1]}"
  return 0
}

# run_capture <seconds> <outfile>
# Detects the port fresh, listens on it via stty+cat for <seconds>, and
# to 3 times total if it fails immediately (empty/near-empty output,
# which is what a transient port-open failure looks like -- a real
# successful capture always produces a lot of output over the full
# duration).
run_capture() {
  local secs="$1" outfile="$2" attempt port
  for attempt in 1 2 3; do
    port=$(detect_port)
    if [ -z "$port" ]; then
      echo "No /dev/ttyACM* device found (attempt $attempt/3). Waiting 3 seconds and checking again..." >&2
      sleep 3
      continue
    fi
    if [ "$attempt" -gt 1 ]; then
      echo "Retrying on $port (attempt $attempt/3)..." >&2
      sleep 2
    fi
    # Configure the port directly with stty and read it with cat, instead
    # of picocom. picocom manages its OWN local-terminal raw mode as well
    # as the serial port, and that setup step is what was throwing
    # "Cannot set the device attributes: Interrupted system call" here --
    # a known picocom issue when its output is piped (as ours is, into
    # tee) rather than going straight to an interactive terminal.
    # stty + cat only touches the serial device's settings, never the
    # local terminal, so this whole class of failure goes away.
    if ! stty -F "$port" "$BAUD" cs8 -cstopb -parenb raw -echo -echoe -echok -crtscts 2>/dev/null; then
      echo "Could not configure $port (attempt $attempt/3) -- retrying..." >&2
      sleep 2
      continue
    fi
    timeout "$secs" cat "$port" | tee "$outfile"
    # A real capture of $secs seconds produces many lines. Fewer than
    # 5 means the capture failed fast (bad port, port busy, etc.) rather
    # than actually capturing for the full duration.
    if [ "$(wc -l < "$outfile" 2>/dev/null || echo 0)" -ge 5 ]; then
      return 0
    fi
    echo "" >&2
    echo "That attempt failed fast (only $(wc -l < "$outfile" 2>/dev/null || echo 0) lines captured) -- retrying..." >&2
  done
  echo "" >&2
  echo "!!! Could not get a working connection to the board after 3 tries. !!!" >&2
  echo "STOP here. Common causes: the USB cable came loose, or another program" >&2
  echo "on this computer already has the serial port open. Do not try to fix" >&2
  echo "this yourself -- wait for the project owner." >&2
  return 1
}

# ---------------------------------------------------------------
# Step 0: pre-flight check -- verifies the board is alive and the
# correct firmware is running, BEFORE any real capture starts.
# ---------------------------------------------------------------
echo "First, a 12-second check to make sure everything is working."
echo "Just leave your hands away from the sensor for this one."
echo ""
read -p "Press Enter to run the check: " _

PRECHECK="$LOGDIR/preflight_check.log"
if ! run_capture 12 "$PRECHECK"; then
    exit 1
fi

if [ ! -s "$PRECHECK" ]; then
    echo ""
    echo "!!! PROBLEM: no data came through at all. !!!"
    echo "Check the USB cable is plugged in firmly, then run this script again."
    echo "If it fails a second time, STOP and wait -- do not continue."
    exit 1
fi

# NOTE: grep -c prints a valid count (e.g. "0") even when it finds no
# matches, but still exits with status 1 in that case. Do NOT chain
# "|| echo 0" onto these -- that pattern runs the fallback echo *in
# addition to* grep's own already-valid "0" output, silently producing
# a two-line value that breaks every numeric comparison below. Capture
# grep's output plain, and only fall back with a shell parameter
# expansion (which looks at emptiness, not exit status).
CSV_COUNT=$(grep -c "\[CSV\]" "$PRECHECK" 2>/dev/null); CSV_COUNT=${CSV_COUNT:-0}
RAWLOG_COUNT=$(grep -c "\[RAWLOG\]" "$PRECHECK" 2>/dev/null); RAWLOG_COUNT=${RAWLOG_COUNT:-0}
DRV_OK=$(grep -c "\[DRV\].*armed=1" "$PRECHECK" 2>/dev/null); DRV_OK=${DRV_OK:-0}
IMU_OK=$(grep -c "\[IMU\].*armed=1" "$PRECHECK" 2>/dev/null); IMU_OK=${IMU_OK:-0}
# A CSV row with d0=1000 is the "no distance reading yet" placeholder --
# normal for the first fraction of a second after boot, but if EVERY row
# in a full 12-second window is still 1000, the distance sensor is not
# actually ranging (this can happen if it's disconnected, occluded, or
# stuck) and every recording after this would silently be useless.
RANGING_OK=$(grep "\[CSV\]" "$PRECHECK" 2>/dev/null | grep -vc "d0=1000"); RANGING_OK=${RANGING_OK:-0}

echo ""
echo "Check results:"
echo "  CSV rows seen:       $CSV_COUNT   (should be > 0)"
echo "  RAWLOG rows seen:    $RAWLOG_COUNT   (should be 0)"
echo "  DRV armed:           $DRV_OK   (should be > 0)"
echo "  IMU armed:           $IMU_OK   (should be > 0)"
echo "  Real distance rows:  $RANGING_OK   (should be > 0)"
echo ""

if [ "$RAWLOG_COUNT" -gt 0 ] || [ "$CSV_COUNT" -eq 0 ] || [ "$DRV_OK" -eq 0 ] || [ "$IMU_OK" -eq 0 ]; then
    echo "!!! STOP: something is not right (see the numbers above). !!!"
    echo "Do not continue. Please stop here and wait -- do not try to fix this yourself."
    exit 1
fi

if [ "$RANGING_OK" -eq 0 ]; then
    echo "!!! STOP: the distance sensor is not producing real readings. !!!"
    echo "(Every row in this check was still the placeholder value.)"
    echo "Do not continue -- recordings made right now would all be useless."
    echo "Please stop here and wait for the project owner. If you were told it's"
    echo "OK to try this yourself first: fully unplug the board's USB cable"
    echo "(and wall power, if it has a separate one), wait 5 seconds, plug it"
    echo "back in, wait 10 seconds, then run this script again from the start."
    exit 1
fi

echo "Everything looks healthy. Moving on to the real recordings."
echo ""
read -p "Press Enter to begin: " _

# ---------------------------------------------------------------
# Combo definitions
# ---------------------------------------------------------------
SURFACES=("hand" "sleeve" "wall" "phone")
declare -A SURFACE_DESC=(
  [hand]="Your BARE HAND -- open palm, fingers together, facing the sensor"
  [sleeve]="The DARK CLOTH -- wrap it around your hand/forearm, facing the sensor"
  [wall]="The LIGHT CLIPBOARD/BOOK -- hold it flat, facing the sensor"
  [phone]="The PHONE OR LAPTOP -- hold it flat, facing the sensor"
)

ANGLES=("straight" "left" "right" "high" "low")
declare -A ANGLE_DESC=(
  [straight]="STRAIGHT ON -- move directly toward the sensor, dead center"
  [left]="FROM THE LEFT -- approach at roughly a 45-degree angle from your left"
  [right]="FROM THE RIGHT -- approach at roughly a 45-degree angle from your right"
  [high]="FROM ABOVE -- start higher than the sensor and angle downward as you approach"
  [low]="FROM BELOW -- start lower than the sensor and angle upward as you approach"
)

SPEEDS=("slow" "normal" "fast")
declare -A SPEED_DESC=(
  [slow]="SLOW -- one full approach-and-retreat about every 3 seconds"
  [normal]="NORMAL -- one full approach-and-retreat about every 1.3 seconds"
  [fast]="FAST -- one full approach-and-retreat about every 0.75 seconds"
)

TOTAL=$((${#SURFACES[@]} * ${#ANGLES[@]} * ${#SPEEDS[@]}))
N=0

for surface in "${SURFACES[@]}"; do
  for angle in "${ANGLES[@]}"; do
    echo ""
    echo "=================================================="
    echo " NEW SETUP"
    echo " Surface: ${SURFACE_DESC[$surface]}"
    echo " Angle:   ${ANGLE_DESC[$angle]}"
    echo "=================================================="
    echo "Get into position now. The next 3 recordings (slow, normal, fast)"
    echo "all use this same surface and angle -- no need to reposition between them."
    echo ""
    read -p "Press Enter when you're ready: " _
    for speed in "${SPEEDS[@]}"; do
      N=$((N+1))
      echo ""
      echo "--- Recording $N of $TOTAL ---"
      echo "Surface: $surface | Angle: $angle | Speed: ${SPEED_DESC[$speed]}"
      echo ""
      echo "Reminder: sweep smoothly from about arm's length (~1.3m) all the way"
      echo "in to almost touching the sensor (~5cm), then back out, and repeat"
      echo "continuously at the pace above for the whole $CAPTURE_SECONDS seconds."
      echo ""
      read -p "Press Enter to start recording (or type s and Enter to skip this one): " ans
      if [ "${ans:-}" = "s" ]; then
        echo "Skipped."
        continue
      fi
      FNAME="$LOGDIR/csv_${surface}_${angle}_${speed}_$(date +%H%M%S).log"
      echo ""
      echo ">>> RECORDING NOW -- START WAVING <<<"
      if run_capture "$CAPTURE_SECONDS" "$FNAME" > /dev/null; then
        echo ">>> Done. Saved. <<<"
      else
        echo ">>> This recording failed -- see the message above. Stopping. <<<"
        exit 1
      fi
    done
  done
done

echo ""
echo "=================================================="
echo " ALL RECORDINGS COMPLETE ($N of $TOTAL done)"
echo "=================================================="
echo "You're finished -- thank you!"
echo "Please don't touch, move, or delete anything. Just leave the computer as it is."
