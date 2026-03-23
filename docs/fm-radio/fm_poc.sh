#!/system/bin/sh
#
# FM Radio PoC — Xiaomi Mi Pad 1 (mocha), BCM4354
# Sends HCI Vendor-Specific Commands (opcode 0xFC15) to control FM receiver
#
# Usage:
#   ./fm_poc.sh on [freq]       Turn on FM, tune to freq (default 1000 = 100.0 MHz)
#   ./fm_poc.sh tune <freq>     Tune to frequency
#   ./fm_poc.sh off             Turn off FM
#   ./fm_poc.sh status          Read current RSSI and frequency
#   ./fm_poc.sh vol <0-255>     Set FM volume
#   ./fm_poc.sh mute            Mute audio
#   ./fm_poc.sh unmute          Unmute audio
#   ./fm_poc.sh scan            Scan full band, report stations with signal
#   ./fm_poc.sh seek_up         Seek next station up
#   ./fm_poc.sh seek_down       Seek next station down
#
# Frequency is in units of MHz*10:
#   1000 = 100.0 MHz
#   876  = 87.6 MHz
#   1062 = 106.2 MHz
#
# IMPORTANT:
#   - Requires root (su)
#   - Requires 'hci_fm' binary (see hci_fm.c, compile and push to device)
#   - Bluetooth must be ON before running this script
#   - Plug in HEADPHONES — they act as FM antenna!
#   - If no audio: check 'tinymix' or 'amixer' for "FM Switch" control
#
# Build hci_fm:
#   arm-linux-gnueabihf-gcc -static -o hci_fm hci_fm.c
#   adb push hci_fm /data/local/tmp/
#   adb shell chmod +x /data/local/tmp/hci_fm
#
# Protocol: BCM FM commands are HCI VSC with OGF=0x3F, OCF=0x0015.
#   Parameters: <register_addr> <0x00=write|0x01=read> [data_lo] [data_hi]
#   Frequency register value = (freq_MHz*10 * 100) - 64000
#
# Source: drivers/bluetooth/broadcom/v4l2_fm_driver/ in SmokeR24.1-kernel
#

# ============================================================================
#  Constants
# ============================================================================

# FM registers
REG_RDS_SYS=0x00        # FM ON/OFF + RDS enable
REG_FM_CTRL=0x01        # Band, stereo mode
REG_RDS_CTL0=0x02       # RDS/RBDS control
REG_AUD_CTL0=0x05       # Audio control: mute, DAC/I2S, de-emphasis
REG_SCH_CTL0=0x07       # Search control: RSSI threshold + direction
REG_SCH_SNR=0x08        # Search SNR threshold
REG_SCH_TUNE=0x09       # Tune/Seek trigger
REG_FM_FREQ=0x0A        # Frequency register (16-bit)
REG_RSSI=0x0F           # RSSI readback
REG_FM_RDS_MSK=0x10     # Interrupt mask
REG_FM_RDS_FLAG=0x12    # Interrupt status
REG_RDS_DATA=0x80       # RDS FIFO
REG_PCM_ROUTE=0x4D      # FM-over-BT-SCO routing
REG_SNR=0xDF            # SNR readback
REG_VOLUME=0xF8         # Volume (0-255)
REG_BLEND_MUTE=0xF9     # Blend/soft mute
REG_SEARCH_METHOD=0xFC
REG_SCH_STEP=0xFD       # Step: 0=50kHz, 1=100kHz, 2=200kHz
REG_PRESET_MAX=0xFE

# FM ON/OFF values for REG_RDS_SYS
FM_OFF=0
FM_ON=1
FM_ON_RDS=3             # FM + RDS both on

# Audio control values (REG_AUD_CTL0)
AUD_MANUAL_MUTE=2       # 0x0002
AUD_I2S_ON=32           # 0x0020
AUD_I2S_UNMUTED=32      # I2S on, unmuted
AUD_I2S_MUTED=34        # I2S on + manual mute

# Stereo mode (REG_FM_CTRL)
STEREO_AUTO=2           # 0x02

# Tune mode (REG_SCH_TUNE)
TUNE_PRESET=1
TUNE_SEEK=2

# Seek direction (upper nibble of SCH_CTL0)
SCAN_UP=128             # 0x80
SCAN_DOWN=0             # 0x00

# Defaults
DEFAULT_FREQ=1000       # 100.0 MHz
DEFAULT_VOLUME=180
DEFAULT_RSSI_THRESHOLD=85  # 0x55

# Band limits
BAND_LOW=760            # 76.0 MHz
BAND_HIGH=1080          # 108.0 MHz
SCAN_STEP=1             # 0.1 MHz per step

# FM enable delay
FM_ENABLE_DELAY=0.3     # seconds

# ============================================================================
#  Helper functions
# ============================================================================

log() {
    echo "[FM] $*"
}

err() {
    echo "[FM ERROR] $*" >&2
}

die() {
    err "$*"
    exit 1
}

# Convert frequency (MHz*10, e.g. 1000=100.0MHz) to register value
freq_to_reg() {
    echo $(( ($1 * 100) - 64000 ))
}

# Convert register value back to frequency (MHz*10)
reg_to_freq() {
    echo $(( ($1 + 64000) / 100 ))
}

# Format frequency for display: 1000 → "100.0"
fmt_freq() {
    local mhz=$(( $1 / 10 ))
    local dec=$(( $1 % 10 ))
    echo "${mhz}.${dec}"
}

# ============================================================================
#  hci_fm tool discovery
# ============================================================================

HCI_FM=""

find_hci_fm() {
    local dir
    dir=$(dirname "$0")
    for p in \
        "${dir}/hci_fm" \
        /data/local/tmp/hci_fm \
        /system/bin/hci_fm \
        /vendor/bin/hci_fm \
        /system/xbin/hci_fm \
        ; do
        if [ -x "$p" ]; then
            HCI_FM="$p"
            return 0
        fi
    done
    return 1
}

# Find mixer tool
MIXER_CMD=""
MIXER_TYPE=""

find_mixer() {
    for p in /system/bin/tinymix /vendor/bin/tinymix tinymix; do
        if command -v "$p" >/dev/null 2>&1; then
            MIXER_CMD="$p"
            MIXER_TYPE="tinymix"
            return 0
        fi
    done
    for p in /system/bin/amixer /vendor/bin/amixer amixer; do
        if command -v "$p" >/dev/null 2>&1; then
            MIXER_CMD="$p"
            MIXER_TYPE="amixer"
            return 0
        fi
    done
    return 1
}

check_prereqs() {
    # Root check
    if [ "$(id -u)" -ne 0 ]; then
        die "Must run as root. Use 'su -c ./fm_poc.sh' or run from root shell"
    fi

    # hci_fm binary
    find_hci_fm || die "hci_fm not found. Build it:
  arm-linux-gnueabihf-gcc -static -o hci_fm hci_fm.c
  adb push hci_fm /data/local/tmp/
  adb shell chmod +x /data/local/tmp/hci_fm"

    # mixer (optional)
    find_mixer || log "WARNING: No mixer tool found. Audio routing may need manual setup."

    log "hci_fm: $HCI_FM"
    log "mixer:  ${MIXER_CMD:-none} (${MIXER_TYPE:-none})"
}

# ============================================================================
#  FM command primitives
# ============================================================================

# Write 16-bit value to FM register
fm_write() {
    local reg=$1
    local val=$2
    local result

    result=$($HCI_FM write $reg $val 2>&1)
    case "$result" in
        OK*) return 0 ;;
        *)   err "write reg=$reg val=$val: $result"; return 1 ;;
    esac
}

# Write 8-bit value to FM register
fm_write8() {
    local reg=$1
    local val=$2
    local result

    result=$($HCI_FM write8 $reg $val 2>&1)
    case "$result" in
        OK*) return 0 ;;
        *)   err "write8 reg=$reg val=$val: $result"; return 1 ;;
    esac
}

# Read FM register, return decimal value (16-bit)
fm_read16() {
    local reg=$1
    local result lo hi

    result=$($HCI_FM read $reg 2 2>&1)
    case "$result" in
        OK*)
            # Parse "OK XX YY" → value = 0xYY * 256 + 0xXX
            lo=$(echo "$result" | awk '{print $2}')
            hi=$(echo "$result" | awk '{print $3}')
            if [ -n "$lo" ] && [ -n "$hi" ]; then
                echo $(( 0x$hi * 256 + 0x$lo ))
            else
                echo "0"
            fi
            return 0
            ;;
        *)
            echo "0"
            return 1
            ;;
    esac
}

# Read FM register, return decimal value (8-bit)
fm_read8() {
    local reg=$1
    local result val

    result=$($HCI_FM read $reg 1 2>&1)
    case "$result" in
        OK*)
            val=$(echo "$result" | awk '{print $2}')
            if [ -n "$val" ]; then
                echo $(( 0x$val ))
            else
                echo "0"
            fi
            return 0
            ;;
        *)
            echo "0"
            return 1
            ;;
    esac
}

# ============================================================================
#  Audio routing
# ============================================================================

fm_audio_on() {
    if [ -z "$MIXER_CMD" ]; then
        log "No mixer tool — please enable FM audio manually:"
        log "  tinymix 'FM Switch' 1"
        return
    fi

    log "Enabling FM audio path (RT5671 AIF4)..."
    if [ "$MIXER_TYPE" = "tinymix" ]; then
        $MIXER_CMD "FM Switch" 1 2>/dev/null || log "WARNING: 'FM Switch' control not found"
    else
        $MIXER_CMD cset name='FM Switch' on 2>/dev/null || log "WARNING: 'FM Switch' control not found"
    fi
}

fm_audio_off() {
    [ -z "$MIXER_CMD" ] && return

    log "Disabling FM audio path..."
    if [ "$MIXER_TYPE" = "tinymix" ]; then
        $MIXER_CMD "FM Switch" 0 2>/dev/null
    else
        $MIXER_CMD cset name='FM Switch' off 2>/dev/null
    fi
}

# ============================================================================
#  FM operations
# ============================================================================

fm_power_on() {
    local freq=${1:-$DEFAULT_FREQ}
    local reg_val=$(freq_to_reg $freq)

    log "=== FM Power ON ==="
    log "Frequency: $(fmt_freq $freq) MHz"

    # Step 1: FM ON
    log "Sending FM ON..."
    fm_write $REG_RDS_SYS $FM_ON
    if [ $? -ne 0 ]; then
        die "FM ON command failed. Is BT running? Is FM chip responding?"
    fi

    # Step 2: Wait for FM to settle
    log "Waiting for FM init (300ms)..."
    sleep $FM_ENABLE_DELAY

    # Step 3: Stereo auto mode
    log "Setting stereo auto..."
    fm_write $REG_FM_CTRL $STEREO_AUTO

    # Step 4: Audio path — I2S ON, unmuted, 50us de-emphasis
    log "Audio: I2S on, unmuted, 50us de-emphasis..."
    fm_write $REG_AUD_CTL0 $AUD_I2S_UNMUTED

    # Step 5: Scan step 100kHz
    fm_write8 $REG_SCH_STEP 1

    # Step 6: Volume
    log "Volume: $DEFAULT_VOLUME/255"
    fm_write $REG_VOLUME $DEFAULT_VOLUME

    # Step 7: Set frequency
    log "Tuning to $(fmt_freq $freq) MHz (reg=$reg_val)..."
    fm_write $REG_FM_FREQ $reg_val

    # Step 8: Trigger tune (preset mode)
    fm_write8 $REG_SCH_TUNE $TUNE_PRESET

    # Step 9: Wait for tune
    sleep 0.5

    # Step 10: Enable ALSA audio path
    fm_audio_on

    log ""
    log "=== FM should be playing ==="
    log "If no audio:"
    log "  1. Are headphones plugged in? (FM antenna!)"
    log "  2. Try: tinymix 'FM Switch' 1"
    log "  3. Try different frequency: ./fm_poc.sh tune <freq>"
    log "  4. Run: ./fm_poc.sh diag"
    log ""

    fm_status
}

fm_power_off() {
    log "=== FM Power OFF ==="
    fm_audio_off
    fm_write $REG_AUD_CTL0 $AUD_MANUAL_MUTE
    sleep 0.1
    fm_write $REG_RDS_SYS $FM_OFF
    log "FM is OFF"
}

fm_tune() {
    local freq=$1

    if [ -z "$freq" ]; then
        die "Usage: fm_poc.sh tune <freq_MHz*10>  (e.g., 1000 for 100.0 MHz)"
    fi
    if [ "$freq" -lt "$BAND_LOW" ] || [ "$freq" -gt "$BAND_HIGH" ]; then
        die "Frequency $(fmt_freq $freq) MHz out of range ($(fmt_freq $BAND_LOW)-$(fmt_freq $BAND_HIGH) MHz)"
    fi

    local reg_val=$(freq_to_reg $freq)
    log "Tuning to $(fmt_freq $freq) MHz..."
    fm_write $REG_FM_FREQ $reg_val
    fm_write8 $REG_SCH_TUNE $TUNE_PRESET
    sleep 0.5
    fm_status
}

fm_status() {
    log "--- Status ---"

    local rssi_raw=$(fm_read8 $REG_RSSI)
    local rssi=$(( (128 - rssi_raw) & 127 ))

    local freq_reg=$(fm_read16 $REG_FM_FREQ)
    local freq_mhz10=0
    [ "$freq_reg" -gt 0 ] && freq_mhz10=$(reg_to_freq $freq_reg)

    local snr=$(fm_read8 $REG_SNR)

    log "Frequency: $(fmt_freq $freq_mhz10) MHz (reg=$freq_reg)"
    log "RSSI:      $rssi dBuV (raw=$rssi_raw)"
    log "SNR:       $snr dB"

    if [ "$rssi" -gt 40 ]; then
        log "Signal:    STRONG"
    elif [ "$rssi" -gt 20 ]; then
        log "Signal:    MODERATE"
    elif [ "$rssi" -gt 5 ]; then
        log "Signal:    WEAK"
    else
        log "Signal:    NO SIGNAL"
    fi
}

fm_set_volume() {
    local vol=${1:-$DEFAULT_VOLUME}
    [ "$vol" -lt 0 ] || [ "$vol" -gt 255 ] && die "Volume must be 0-255"
    log "Volume: $vol/255"
    fm_write $REG_VOLUME $vol
}

fm_mute() {
    log "Muting..."
    fm_write $REG_AUD_CTL0 $AUD_I2S_MUTED
}

fm_unmute() {
    log "Unmuting..."
    fm_write $REG_AUD_CTL0 $AUD_I2S_UNMUTED
}

fm_seek() {
    local direction=$1
    local dir_bit=$SCAN_UP
    local dir_name="UP"
    [ "$direction" = "down" ] && dir_bit=$SCAN_DOWN && dir_name="DOWN"

    log "Seeking $dir_name..."

    fm_write8 $REG_SEARCH_METHOD 0
    fm_write8 $REG_PRESET_MAX 0

    local sch_ctl=$(( DEFAULT_RSSI_THRESHOLD | dir_bit ))
    fm_write8 $REG_SCH_CTL0 $sch_ctl
    fm_write8 $REG_SCH_TUNE $TUNE_SEEK

    log "Waiting for seek..."
    local i=0
    while [ $i -lt 20 ]; do
        sleep 1
        i=$(( i + 1 ))

        local flags=$(fm_read16 $REG_FM_RDS_FLAG)
        if [ $(( flags & 1 )) -ne 0 ]; then
            log "Seek complete!"
            fm_status
            return 0
        fi
        if [ $(( flags & 2 )) -ne 0 ]; then
            log "Seek reached band limit"
            fm_status
            return 1
        fi
        printf "."
    done
    echo ""
    log "Seek timed out"
    fm_status
    return 1
}

fm_scan() {
    log "=== Full Band Scan ==="
    log "Scanning $(fmt_freq $BAND_LOW) - $(fmt_freq $BAND_HIGH) MHz"
    log ""

    local freq=$BAND_LOW
    local found=0
    local stations=""

    while [ $freq -le $BAND_HIGH ]; do
        local reg_val=$(freq_to_reg $freq)
        fm_write $REG_FM_FREQ $reg_val >/dev/null 2>&1
        fm_write8 $REG_SCH_TUNE $TUNE_PRESET >/dev/null 2>&1
        sleep 0.05

        local rssi_raw=$(fm_read8 $REG_RSSI 2>/dev/null)
        local rssi=$(( (128 - rssi_raw) & 127 ))

        printf "\r  $(fmt_freq $freq) MHz  RSSI: %3d dBuV  " "$rssi"

        if [ "$rssi" -gt 25 ]; then
            printf " <<< STATION"
            found=$(( found + 1 ))
            stations="${stations}  $(fmt_freq $freq) MHz  (RSSI: $rssi)\n"
        fi
        echo ""

        freq=$(( freq + SCAN_STEP ))
    done

    echo ""
    log "=== Found $found station(s) ==="
    [ -n "$stations" ] && printf "$stations"
}

fm_diag() {
    log "=== FM Diagnostics ==="

    echo ""
    log "hci_fm binary: $HCI_FM"
    echo ""

    log "Testing FM read (receiver ID, reg 0x28)..."
    local result
    result=$($HCI_FM read 0x28 2 2>&1)
    log "  Result: $result"

    echo ""
    log "Testing FM ON..."
    result=$($HCI_FM write 0x00 1 2>&1)
    log "  FM ON: $result"

    if echo "$result" | grep -q "^OK"; then
        sleep 0.3
        log "  FM chip responded! Reading back..."
        result=$($HCI_FM read 0x00 1 2>&1)
        log "  RDS_SYS readback: $result"
        $HCI_FM write 0x00 0 >/dev/null 2>&1
        log "  FM OFF sent"
    else
        log "  FM did NOT respond. Possible causes:"
        log "    - BT is not running"
        log "    - FM chip does not support this protocol"
        log "    - Bluedroid is blocking vendor commands"
    fi

    echo ""
    log "Audio mixer 'FM' controls:"
    if [ -n "$MIXER_CMD" ]; then
        if [ "$MIXER_TYPE" = "tinymix" ]; then
            $MIXER_CMD 2>&1 | grep -i "fm" | sed 's/^/  /' || log "  No FM controls found"
        else
            $MIXER_CMD controls 2>&1 | grep -i "fm" | sed 's/^/  /' || log "  No FM controls found"
        fi
    else
        log "  No mixer tool available"
    fi

    echo ""
    log "Kernel FM-related:"
    dmesg 2>/dev/null | grep -iE "fm|radio|v4l2fm|fmdrv|brcm_ldisc" | tail -20 | sed 's/^/  /' || log "  Nothing in dmesg"

    echo ""
    log "/dev devices:"
    ls -la /dev/radio* /dev/brcm_bt* 2>/dev/null | sed 's/^/  /' || log "  No radio/BT devices"
}

# ============================================================================
#  Main
# ============================================================================

usage() {
    cat <<'EOF'
FM Radio PoC — Xiaomi Mi Pad 1 (mocha), BCM4354

Usage: fm_poc.sh <command> [args]

Commands:
  on [freq]       Power on FM, tune to freq (default: 100.0 MHz)
  off             Power off FM
  tune <freq>     Tune to frequency
  status          Show current frequency, RSSI, SNR
  vol <0-255>     Set volume
  mute            Mute FM audio
  unmute          Unmute FM audio
  seek_up         Seek next station upward
  seek_down       Seek next station downward
  scan            Scan full band, list all stations
  diag            Run diagnostics

Frequency format: MHz * 10 (integer)
  1000 = 100.0 MHz     876 = 87.6 MHz
  1062 = 106.2 MHz     915 = 91.5 MHz

Requirements:
  - Root access
  - hci_fm binary (see hci_fm.c — build and push to device)
  - Bluetooth ON
  - Headphones plugged in (FM antenna)

Build hci_fm:
  arm-linux-gnueabihf-gcc -static -o hci_fm hci_fm.c
  adb push hci_fm /data/local/tmp/
  adb shell chmod +x /data/local/tmp/hci_fm
EOF
}

CMD="${1:-}"

case "$CMD" in
    on)         check_prereqs; fm_power_on "${2:-$DEFAULT_FREQ}" ;;
    off)        check_prereqs; fm_power_off ;;
    tune)       check_prereqs; fm_tune "$2" ;;
    status)     check_prereqs; fm_status ;;
    vol|volume) check_prereqs; fm_set_volume "$2" ;;
    mute)       check_prereqs; fm_mute ;;
    unmute)     check_prereqs; fm_unmute ;;
    seek_up)    check_prereqs; fm_seek up ;;
    seek_down)  check_prereqs; fm_seek down ;;
    scan)       check_prereqs; fm_power_on $BAND_LOW; sleep 0.5; fm_scan ;;
    diag)       check_prereqs; fm_diag ;;
    help|-h|--help) usage ;;
    *)          usage; exit 1 ;;
esac
