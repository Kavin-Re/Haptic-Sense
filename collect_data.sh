#!/bin/bash
# Haptic-Sense Block 6 -- guided data collection script.
# Run with:  cd ~/haptic-sense && bash collect_data.sh
# Safe to Ctrl-C at any time between prompts; anything already saved stays saved.

set -u
LOGDIR="$HOME/haptic-sense/logs"
mkdir -p "$LOGDIR"
PORT="/dev/ttyACM0"
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
# Step 0: pre-flight check -- verifies the board is alive and the
# correct firmware is running, BEFORE any real capture starts.
# ---------------------------------------------------------------
echo "First, a 12-second check to make sure everything is working."
echo "Just leave your hands away from the sensor for this one."
echo ""
read -p "Press Enter to run the check: " _

PRECHECK="$LOGDIR/preflight_check.log"
timeout 12 picocom -b "$BAUD" "$PORT" | tee "$PRECHECK"

if [ ! -s "$PRECHECK" ]; then
    echo ""
    echo "!!! PROBLEM: no data came through at all. !!!"
    echo "Check the USB cable is plugged in firmly, then run this script again."
    echo "If it fails a second time, STOP and wait -- do not continue."
    exit 1
fi

CSV_COUNT=$(grep -c "\[CSV\]" "$PRECHECK" 2>/dev/null || echo 0)
RAWLOG_COUNT=$(grep -c "\[RAWLOG\]" "$PRECHECK" 2>/dev/null || echo 0)
DRV_OK=$(grep -c "\[DRV\].*armed=1" "$PRECHECK" 2>/dev/null || echo 0)
IMU_OK=$(grep -c "\[IMU\].*armed=1" "$PRECHECK" 2>/dev/null || echo 0)

echo ""
echo "Check results:"
echo "  CSV rows seen:    $CSV_COUNT   (should be > 0)"
echo "  RAWLOG rows seen: $RAWLOG_COUNT   (should be 0)"
echo "  DRV armed:        $DRV_OK   (should be > 0)"
echo "  IMU armed:        $IMU_OK   (should be > 0)"
echo ""

if [ "$RAWLOG_COUNT" -gt 0 ] || [ "$CSV_COUNT" -eq 0 ] || [ "$DRV_OK" -eq 0 ] || [ "$IMU_OK" -eq 0 ]; then
    echo "!!! STOP: something is not right (see the numbers above). !!!"
    echo "Do not continue. Please stop here and wait -- do not try to fix this yourself."
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
      timeout "$CAPTURE_SECONDS" picocom -b "$BAUD" "$PORT" | tee "$FNAME" > /dev/null
      echo ">>> Done. Saved. <<<"
    done
  done
done

echo ""
echo "=================================================="
echo " ALL RECORDINGS COMPLETE ($N of $TOTAL done)"
echo "=================================================="
echo "You're finished -- thank you!"
echo "Please don't touch, move, or delete anything. Just leave the computer as it is."
