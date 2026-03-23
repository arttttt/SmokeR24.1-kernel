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
#   - Requires 'hcitool' (from BlueZ, usually in /system/bin/ or /vendor/bin/)
#   - Bluetooth must be ON before running this script
#   - Plug in HEADPHONES — they act as FM antenna!
#   - If no audio: check 'tinymix' or 'amixer' for "FM Switch" control
#
# Protocol: BCM FM commands are HCI VSC with OGF=0x3F, OCF=0x0015.
#   Parameters: <register_addr> <0x00=write|0x01=read> [data_lo] [data_hi]
#   Frequency register value = (freq_MHz*10 * 100) - 64000
#
# Source: drivers/bluetooth/broadcom/v4l2_fm_driver/ in SmokeR24.1-kernel
#

set -e

# ============================================================================
#  Constants
# ============================================================================

OGF="0x3F"
OCF="0x0015"

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
REG_SEARCH_BOUNDARY=0xFB
REG_SEARCH_METHOD=0xFC
REG_SCH_STEP=0xFD       # Step: 0=50kHz, 1=100kHz, 2=200kHz
REG_PRESET_MAX=0xFE

# FM ON/OFF values for REG_RDS_SYS
FM_OFF=0x00
FM_ON=0x01
FM_RDS_ON=0x02
FM_ON_RDS=0x03          # FM + RDS both on

# Audio control bits (REG_AUD_CTL0)
AUD_RF_MUTE=0x0001
AUD_MANUAL_MUTE=0x0002
AUD_DAC_ON=0x0010
AUD_I2S_ON=0x0020
AUD_DEEMPH_75US=0x0040  # 0=50us(EUR), 1=75us(NA)

# Stereo mode (REG_FM_CTRL)
STEREO_AUTO=0x02

# Tune mode (REG_SCH_TUNE)
TUNE_PRESET=0x01
TUNE_SEEK=0x02

# Seek direction
SCAN_UP=0x80
SCAN_DOWN=0x00

# Defaults
DEFAULT_FREQ=1000       # 100.0 MHz
DEFAULT_VOLUME=180
DEFAULT_RSSI_THRESHOLD=0x55  # 85 dBm
FM_ENABLE_DELAY_MS=300

# Band limits (Europe/China/Russia compatible, widest range)
BAND_LOW=760            # 76.0 MHz
BAND_HIGH=1080          # 108.0 MHz
SCAN_STEP=1             # 0.1 MHz per step

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

# Convert integer to hex byte
hex8() {
    printf "0x%02X" $(( $1 & 0xFF ))
}

# Find hcitool
find_hcitool() {
    for p in /system/bin/hcitool /vendor/bin/hcitool /system/xbin/hcitool hcitool; do
        if command -v "$p" >/dev/null 2>&1; then
            echo "$p"
            return 0
        fi
    done
    return 1
}

# Find mixer tool
find_mixer() {
    for p in /system/bin/tinymix /vendor/bin/tinymix tinymix; do
        if command -v "$p" >/dev/null 2>&1; then
            echo "$p tinymix"
            return 0
        fi
    done
    for p in /system/bin/amixer /vendor/bin/amixer amixer; do
        if command -v "$p" >/dev/null 2>&1; then
            echo "$p amixer"
            return 0
        fi
    done
    return 1
}

HCITOOL=""
MIXER_CMD=""
MIXER_TYPE=""

check_prereqs() {
    # Root check
    if [ "$(id -u)" -ne 0 ]; then
        die "Must run as root. Use 'su -c ./fm_poc.sh' or run from root shell"
    fi

    # hcitool
    HCITOOL=$(find_hcitool) || die "hcitool not found. Install BlueZ tools or push hcitool binary to /system/bin/"

    # mixer
    local mixer_info
    mixer_info=$(find_mixer) || log "WARNING: No mixer tool found. Audio routing may need manual setup."
    if [ -n "$mixer_info" ]; then
        MIXER_CMD=$(echo "$mixer_info" | awk '{print $1}')
        MIXER_TYPE=$(echo "$mixer_info" | awk '{print $2}')
    fi

    # BT check
    if ! $HCITOOL dev 2>/dev/null | grep -q "hci0"; then
        die "No HCI device found. Is Bluetooth turned ON?"
    fi

    log "hcitool: $HCITOOL"
    log "mixer: ${MIXER_CMD:-none} (${MIXER_TYPE:-none})"
    log "HCI device: hci0 OK"
}

# ============================================================================
#  FM command primitives
# ============================================================================

# Write 16-bit value to FM register
# Usage: fm_write <reg_addr_hex> <value_16bit_decimal>
fm_write() {
    local reg=$1
    local val=$2
    local lo=$(hex8 $val)
    local hi=$(hex8 $(( val >> 8 )))
    local result

    result=$($HCITOOL cmd $OGF $OCF $reg 0x00 $lo $hi 2>&1)
    if echo "$result" | grep -q "Input/output error\|Connection timed out\|Can't send"; then
        err "Command failed for reg $reg: $result"
        return 1
    fi
    return 0
}

# Write 8-bit value to FM register
fm_write8() {
    local reg=$1
    local val=$2
    local lo=$(hex8 $val)

    $HCITOOL cmd $OGF $OCF $reg 0x00 $lo 0x00 >/dev/null 2>&1
}

# Read FM register (returns hex bytes on stdout)
# Usage: fm_read <reg_addr_hex> <read_len_1_or_2>
fm_read() {
    local reg=$1
    local len=${2:-2}
    local result

    result=$($HCITOOL cmd $OGF $OCF $reg 0x01 $(hex8 $len) 2>&1)
    echo "$result"
}

# Read 16-bit register value, return as decimal
fm_read16() {
    local reg=$1
    local raw
    raw=$(fm_read $reg 2)

    # Parse response: look for the data bytes after status+opcode+rdwr
    # HCI Event: 0x0e plen N
    #   XX 15 FC 00 <reg> <rdwr> <lo> <hi>
    # The last two hex bytes before the end are our data
    local bytes
    bytes=$(echo "$raw" | grep ">" | tail -1 | sed 's/.*> //' | tr -s ' ')

    if [ -z "$bytes" ]; then
        echo "0"
        return 1
    fi

    # Extract data bytes (positions depend on response format)
    # Try to get the last 2 bytes from the response
    local lo hi
    lo=$(echo "$bytes" | awk '{print $(NF-1)}')
    hi=$(echo "$bytes" | awk '{print $NF}')

    if [ -z "$lo" ] || [ -z "$hi" ]; then
        echo "0"
        return 1
    fi

    echo $(( 0x$hi * 256 + 0x$lo ))
}

# Read 8-bit register value
fm_read8() {
    local reg=$1
    local raw
    raw=$(fm_read $reg 1)

    local bytes
    bytes=$(echo "$raw" | grep ">" | tail -1 | sed 's/.*> //')
    local val
    val=$(echo "$bytes" | awk '{print $NF}')

    if [ -z "$val" ]; then
        echo "0"
        return 1
    fi

    echo $(( 0x$val ))
}

# ============================================================================
#  Audio routing
# ============================================================================

fm_audio_on() {
    if [ -z "$MIXER_CMD" ]; then
        log "No mixer tool — please enable FM audio manually:"
        log "  tinymix 'FM Switch' 1"
        log "  OR: amixer cset name='FM Switch' on"
        return
    fi

    log "Enabling FM audio path (RT5671 AIF4)..."
    if [ "$MIXER_TYPE" = "tinymix" ]; then
        $MIXER_CMD "FM Switch" 1 2>/dev/null || log "WARNING: 'FM Switch' control not found in tinymix"
    else
        $MIXER_CMD cset name='FM Switch' on 2>/dev/null || log "WARNING: 'FM Switch' control not found in amixer"
    fi
}

fm_audio_off() {
    if [ -z "$MIXER_CMD" ]; then
        return
    fi

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
        die "FM ON command failed. Is BT firmware running?"
    fi

    # Step 2: Wait for FM to settle
    log "Waiting ${FM_ENABLE_DELAY_MS}ms for FM to initialize..."
    sleep 0.3

    # Step 3: Set stereo auto mode, Europe/West band
    log "Setting stereo auto mode..."
    fm_write $REG_FM_CTRL $STEREO_AUTO

    # Step 4: Set audio path — I2S ON, unmuted, 50us de-emphasis (Europe)
    log "Configuring audio: I2S on, unmuted, 50us de-emphasis..."
    fm_write $REG_AUD_CTL0 $AUD_I2S_ON

    # Step 5: Set scan step to 100kHz
    fm_write8 $REG_SCH_STEP 1

    # Step 6: Set volume
    log "Setting volume to $DEFAULT_VOLUME/255..."
    fm_write $REG_VOLUME $DEFAULT_VOLUME

    # Step 7: Tune to frequency
    log "Tuning to $(fmt_freq $freq) MHz (reg=$reg_val = $(hex8 $reg_val) $(hex8 $(( reg_val >> 8 ))))..."
    fm_write $REG_FM_FREQ $reg_val

    # Step 8: Trigger tune (preset mode)
    fm_write8 $REG_SCH_TUNE $TUNE_PRESET

    # Step 9: Wait for tune to complete
    sleep 0.5

    # Step 10: Enable ALSA audio path
    fm_audio_on

    log ""
    log "=== FM should be playing ==="
    log "If no audio:"
    log "  1. Are headphones plugged in? (they are the antenna!)"
    log "  2. Try: tinymix 'FM Switch' 1"
    log "  3. Try a different frequency: ./fm_poc.sh tune <freq>"
    log "  4. Check dmesg for errors"
    log ""

    # Show status
    fm_status
}

fm_power_off() {
    log "=== FM Power OFF ==="

    fm_audio_off

    # Mute first
    fm_write $REG_AUD_CTL0 $AUD_MANUAL_MUTE
    sleep 0.1

    # FM OFF
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

    # Read RSSI
    local rssi_raw
    rssi_raw=$(fm_read8 $REG_RSSI 2>/dev/null) || rssi_raw=0
    # Convert from 2's complement: RSSI = (0x80 - raw) & ~0x80
    local rssi=$(( (128 - rssi_raw) & 127 ))

    # Read frequency register
    local freq_reg
    freq_reg=$(fm_read16 $REG_FM_FREQ 2>/dev/null) || freq_reg=0
    local freq_mhz10=0
    if [ "$freq_reg" -gt 0 ]; then
        freq_mhz10=$(reg_to_freq $freq_reg)
    fi

    # Read SNR
    local snr
    snr=$(fm_read8 $REG_SNR 2>/dev/null) || snr=0

    log "Frequency: $(fmt_freq $freq_mhz10) MHz (reg=$freq_reg)"
    log "RSSI:      $rssi dBuV (raw=0x$(printf '%02X' $rssi_raw))"
    log "SNR:       $snr dB"

    # Signal quality assessment
    if [ "$rssi" -gt 40 ]; then
        log "Signal:    STRONG"
    elif [ "$rssi" -gt 20 ]; then
        log "Signal:    MODERATE"
    elif [ "$rssi" -gt 5 ]; then
        log "Signal:    WEAK"
    else
        log "Signal:    NO SIGNAL (try different frequency or check antenna)"
    fi
}

fm_set_volume() {
    local vol=${1:-$DEFAULT_VOLUME}

    if [ "$vol" -lt 0 ] || [ "$vol" -gt 255 ]; then
        die "Volume must be 0-255"
    fi

    log "Setting volume to $vol/255"
    fm_write $REG_VOLUME $vol
}

fm_mute() {
    log "Muting FM..."
    fm_write $REG_AUD_CTL0 $(( AUD_I2S_ON | AUD_MANUAL_MUTE ))
}

fm_unmute() {
    log "Unmuting FM..."
    fm_write $REG_AUD_CTL0 $AUD_I2S_ON
}

fm_seek() {
    local direction=$1  # "up" or "down"
    local dir_bit=$SCAN_UP
    local dir_name="UP"

    if [ "$direction" = "down" ]; then
        dir_bit=$SCAN_DOWN
        dir_name="DOWN"
    fi

    log "Seeking $dir_name..."

    # Set search method = normal scan
    fm_write8 $REG_SEARCH_METHOD 0
    # Set preset max = 0 (no limit)
    fm_write8 $REG_PRESET_MAX 0
    # Set search control: RSSI threshold + direction
    local sch_ctl=$(( DEFAULT_RSSI_THRESHOLD | dir_bit ))
    fm_write8 $REG_SCH_CTL0 $sch_ctl
    # Trigger seek
    fm_write8 $REG_SCH_TUNE $TUNE_SEEK

    # Wait for seek (no interrupt handler, just poll)
    log "Waiting for seek to complete..."
    local timeout=20
    local i=0
    while [ $i -lt $timeout ]; do
        sleep 1
        i=$(( i + 1 ))

        # Read interrupt flags
        local flags
        flags=$(fm_read16 $REG_FM_RDS_FLAG 2>/dev/null) || continue

        # Check tune complete bit (bit 0)
        if [ $(( flags & 1 )) -ne 0 ]; then
            log "Seek complete!"
            fm_status
            return 0
        fi
        # Check tune fail bit (bit 1)
        if [ $(( flags & 2 )) -ne 0 ]; then
            log "Seek reached band limit, no station found"
            fm_status
            return 1
        fi

        printf "."
    done

    echo ""
    log "Seek timed out after ${timeout}s"
    fm_status
    return 1
}

fm_scan() {
    log "=== Full Band Scan ==="
    log "Scanning $(fmt_freq $BAND_LOW) - $(fmt_freq $BAND_HIGH) MHz, step $(fmt_freq $SCAN_STEP) MHz"
    log "This will take about 30 seconds..."
    echo ""

    local freq=$BAND_LOW
    local found=0
    local stations=""

    while [ $freq -le $BAND_HIGH ]; do
        local reg_val=$(freq_to_reg $freq)

        # Tune to frequency
        fm_write $REG_FM_FREQ $reg_val >/dev/null 2>&1
        fm_write8 $REG_SCH_TUNE $TUNE_PRESET >/dev/null 2>&1

        # Brief settle time
        sleep 0.05

        # Read RSSI
        local rssi_raw
        rssi_raw=$(fm_read8 $REG_RSSI 2>/dev/null) || rssi_raw=128
        local rssi=$(( (128 - rssi_raw) & 127 ))

        # Show progress
        printf "\r  $(fmt_freq $freq) MHz  RSSI: %3d dBuV  " "$rssi"

        # Station detected if RSSI above threshold
        if [ "$rssi" -gt 25 ]; then
            local bar=""
            local b=0
            while [ $b -lt $(( rssi / 5 )) ] && [ $b -lt 20 ]; do
                bar="${bar}#"
                b=$(( b + 1 ))
            done
            printf " <<< STATION  %s" "$bar"
            found=$(( found + 1 ))
            stations="${stations}  $(fmt_freq $freq) MHz  (RSSI: $rssi)\n"
        fi
        echo ""

        freq=$(( freq + SCAN_STEP ))
    done

    echo ""
    log "=== Scan Complete ==="
    log "Found $found station(s):"
    if [ -n "$stations" ]; then
        printf "$stations"
    else
        log "  No stations found. Check:"
        log "  - Are headphones plugged in? (antenna)"
        log "  - Is the FM chip actually responding? (check dmesg)"
    fi
}

# ============================================================================
#  Diagnostics
# ============================================================================

fm_diag() {
    log "=== FM Diagnostics ==="

    # Check HCI
    echo ""
    log "HCI device info:"
    $HCITOOL dev 2>&1 | sed 's/^/  /'

    # Try a basic FM read (receiver ID register)
    echo ""
    log "Reading FM receiver ID (reg 0x28)..."
    local raw
    raw=$(fm_read 0x28 2 2>&1)
    echo "$raw" | sed 's/^/  /'

    # Check if FM responds at all
    echo ""
    log "Sending FM ON test..."
    raw=$(fm_write $REG_RDS_SYS $FM_ON 2>&1)
    if [ $? -eq 0 ]; then
        log "FM ON command accepted by HCI!"
        sleep 0.3
        # Read back
        log "Reading RDS_SYS register..."
        raw=$(fm_read $REG_RDS_SYS 1 2>&1)
        echo "$raw" | sed 's/^/  /'
        # Turn off
        fm_write $REG_RDS_SYS $FM_OFF >/dev/null 2>&1
    else
        log "FM ON command FAILED"
        echo "$raw" | sed 's/^/  /'
    fi

    # Check audio mixer
    echo ""
    log "Audio mixer controls with 'FM':"
    if [ -n "$MIXER_CMD" ]; then
        if [ "$MIXER_TYPE" = "tinymix" ]; then
            $MIXER_CMD 2>&1 | grep -i "fm" | sed 's/^/  /' || log "  No FM controls found"
        else
            $MIXER_CMD controls 2>&1 | grep -i "fm" | sed 's/^/  /' || log "  No FM controls found"
        fi
    else
        log "  No mixer tool available"
    fi

    # Check kernel modules
    echo ""
    log "Kernel FM/BT modules:"
    cat /proc/modules 2>/dev/null | grep -iE "fm|brcm|ldisc|radio" | sed 's/^/  /' || log "  No matching modules loaded"

    # Check /dev
    echo ""
    log "Radio/BT devices:"
    ls -la /dev/radio* /dev/brcm_bt* /dev/hci* 2>/dev/null | sed 's/^/  /' || log "  No radio/BT devices found"

    echo ""
    log "dmesg (FM/radio related, last 30 lines):"
    dmesg 2>/dev/null | grep -iE "fm|radio|v4l2fm|fmdrv|brcm_ldisc|ldisc" | tail -30 | sed 's/^/  /' || log "  No FM messages in dmesg"
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
  diag            Run diagnostics (check HCI, mixer, kernel)

Frequency format: MHz * 10 (integer)
  1000 = 100.0 MHz     876 = 87.6 MHz
  1062 = 106.2 MHz     915 = 91.5 MHz

Examples:
  ./fm_poc.sh on                # FM on at 100.0 MHz
  ./fm_poc.sh on 915            # FM on at 91.5 MHz
  ./fm_poc.sh scan              # Find all stations
  ./fm_poc.sh seek_up           # Find next station
  ./fm_poc.sh vol 200           # Louder
  ./fm_poc.sh off               # Power off

NOTE: Headphones MUST be plugged in — they serve as FM antenna!
EOF
}

CMD="${1:-}"

case "$CMD" in
    on)
        check_prereqs
        fm_power_on "${2:-$DEFAULT_FREQ}"
        ;;
    off)
        check_prereqs
        fm_power_off
        ;;
    tune)
        check_prereqs
        fm_tune "$2"
        ;;
    status)
        check_prereqs
        fm_status
        ;;
    vol|volume)
        check_prereqs
        fm_set_volume "$2"
        ;;
    mute)
        check_prereqs
        fm_mute
        ;;
    unmute)
        check_prereqs
        fm_unmute
        ;;
    seek_up)
        check_prereqs
        fm_seek up
        ;;
    seek_down)
        check_prereqs
        fm_seek down
        ;;
    scan)
        check_prereqs
        fm_power_on $BAND_LOW
        sleep 0.5
        fm_scan
        ;;
    diag)
        check_prereqs
        fm_diag
        ;;
    help|-h|--help)
        usage
        ;;
    *)
        usage
        exit 1
        ;;
esac
