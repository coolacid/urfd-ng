#!/bin/bash
#
# setup.sh -- install or update the urfd reflector and the tcd transcoder
#
#   sudo ./setup.sh            first install: asks a few questions, writes the
#                              configuration, builds and starts everything.
#                              On an installed machine it offers an update.
#   sudo ./setup.sh update     pull the latest version and rebuild, keeping the
#                              configuration.
#
# The reflector runs in Docker (with its dashboard), the transcoder natively
# with systemd, because the AMBE USB devices need low latency. Both can run on
# this machine, or the transcoder on a second one (e.g. a Raspberry Pi with the
# AMBE sticks).
#
# Supported: Debian, Ubuntu, Raspberry Pi OS (x86_64, arm64, armhf).
# Guide: docs/INSTALL.md

set -euo pipefail

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DATA_DIR="${URFD_DATA:-/opt/urfd}"
CFG_DIR="$DATA_DIR/config"
TCD_INI="/usr/local/etc/tcd.ini"
TCD_BUILD_CONF="/usr/local/etc/tcd-build.conf"   # remembers the MD380 choice for updates
LOG_FILE="/var/log/urfd-setup.log"
export URFD_DATA="$DATA_DIR"

# ---------------------------------------------------------------------------
# helpers
# ---------------------------------------------------------------------------

bold()  { printf '\033[1m%s\033[0m\n' "$*"; }
info()  { printf '\033[1;34m==>\033[0m %s\n' "$*"; }
warn()  { printf '\033[1;33mWARNING:\033[0m %s\n' "$*"; }
die()   { printf '\033[1;31mERROR:\033[0m %s\n' "$*" >&2; exit 1; }

# ask VAR "question" "default" [regex] [hint]
ask() {
    local var=$1 question=$2 default=${3:-} regex=${4:-} hint=${5:-} answer
    while true; do
        if [ -n "$default" ]; then
            read -r -p "$question [$default]: " answer </dev/tty || die "no terminal to answer on, run setup.sh in an interactive session (ssh -t)"
            answer=${answer:-$default}
        else
            read -r -p "$question: " answer </dev/tty || die "no terminal to answer on, run setup.sh in an interactive session (ssh -t)"
        fi
        if [ -z "$regex" ] || [[ "$answer" =~ $regex ]]; then
            printf -v "$var" '%s' "$answer"
            return
        fi
        echo "  ${hint:-Invalid input, please try again.}"
    done
}

# ask_yn "question" y|n -> exit status 0 for yes
ask_yn() {
    local question=$1 default=$2 answer prompt="[y/N]"
    [ "$default" = "y" ] && prompt="[Y/n]"
    while true; do
        read -r -p "$question $prompt: " answer </dev/tty || die "no terminal to answer on, run setup.sh in an interactive session (ssh -t)"
        answer=${answer:-$default}
        case "${answer,,}" in
            y|yes) return 0 ;;
            n|no)  return 1 ;;
        esac
    done
}

# set_ini FILE SECTION KEY VALUE -- set "KEY = VALUE" in [SECTION], replacing
# the active or commented-out key, keeping all other lines and comments
set_ini() {
    python3 - "$@" <<'PY'
import re, sys
path, section, key, value = sys.argv[1:5]
lines = open(path).read().split('\n')
start = next((i for i, l in enumerate(lines) if l.strip() == '[' + section + ']'), None)
if start is None:
    lines += ['', '[' + section + ']', key + ' = ' + value]
else:
    end = next((i for i in range(start + 1, len(lines)) if lines[i].startswith('[')), len(lines))
    active = re.compile(r'^\s*' + re.escape(key) + r'\s*=')
    commented = re.compile(r'^\s*#\s*' + re.escape(key) + r'\s*=')
    hit = next((i for i in range(start + 1, end) if active.match(lines[i])), None)
    if hit is None:
        hit = next((i for i in range(start + 1, end) if commented.match(lines[i])), None)
    if hit is None:
        lines.insert(start + 1, key + ' = ' + value)
    else:
        lines[hit] = key + ' = ' + value
open(path, 'w').write('\n'.join(lines))
PY
}

# del_ini FILE SECTION KEY -- remove the active KEY from [SECTION]
del_ini() {
    python3 - "$@" <<'PY'
import re, sys
path, section, key = sys.argv[1:4]
lines = open(path).read().split('\n')
start = next((i for i, l in enumerate(lines) if l.strip() == '[' + section + ']'), None)
if start is not None:
    end = next((i for i in range(start + 1, len(lines)) if lines[i].startswith('[')), len(lines))
    active = re.compile(r'^\s*' + re.escape(key) + r'\s*=')
    lines = [l for i, l in enumerate(lines) if not (start < i < end and active.match(l))]
open(path, 'w').write('\n'.join(lines))
PY
}

# set_php FILE KEY VALUE -- set $KEY = VALUE; in the dashboard config, e.g.
# set_php config.inc.php "CallingHome['Active']" "true"
set_php() {
    python3 - "$@" <<'PY'
import re, sys
path, key, value = sys.argv[1:4]
s = open(path).read()
pat = re.compile(r"^(\s*\$" + re.escape(key) + r"\s*=\s*).*?;", re.M)
if not pat.search(s):
    sys.exit("missing setting " + key)
s = pat.sub(lambda m: m.group(1) + value + ';', s, count=1)
open(path, 'w').write(s)
PY
}

php_str() { printf "'%s'" "${1//\'/\\\'}"; }

local_ip() {
    ip -4 route get 1.1.1.1 2>/dev/null | awk '{for (i=1; i<NF; i++) if ($i == "src") print $(i+1)}' | head -1
}

# run git as the owner of the checkout, not as root
repo_git() {
    local owner
    owner=$(stat -c %U "$REPO_DIR")
    if [ "$owner" != "root" ] && command -v sudo >/dev/null; then
        sudo -u "$owner" git -C "$REPO_DIR" "$@"
    else
        git -C "$REPO_DIR" "$@"
    fi
}

# ---------------------------------------------------------------------------
# checks and packages
# ---------------------------------------------------------------------------

check_system() {
    [ "$(id -u)" -eq 0 ] || die "please run as root: sudo ./setup.sh"
    [ -r /etc/os-release ] || die "unknown Linux distribution"
    . /etc/os-release
    case "${ID:-} ${ID_LIKE:-}" in
        *debian*|*ubuntu*|*raspbian*) ;;
        *) die "this script supports Debian, Ubuntu and Raspberry Pi OS, not '${PRETTY_NAME:-$ID}'" ;;
    esac
    command -v systemctl >/dev/null || die "systemd is required"
}

install_base_packages() {
    local missing=()
    for p in git curl python3 ca-certificates; do
        dpkg -s "$p" >/dev/null 2>&1 || missing+=("$p")
    done
    if [ ${#missing[@]} -gt 0 ]; then
        info "Installing ${missing[*]}"
        apt-get update -qq
        apt-get install -y -qq "${missing[@]}"
    fi
}

install_docker() {
    if command -v docker >/dev/null && docker compose version >/dev/null 2>&1; then
        return
    fi
    echo
    echo "The reflector runs in Docker, which is not installed (or lacks 'docker compose')."
    ask_yn "Install Docker from download.docker.com now?" y || die "Docker is required for the reflector"
    info "Installing Docker"
    curl -fsSL https://get.docker.com | sh
    systemctl enable --now docker
    docker compose version >/dev/null 2>&1 || die "'docker compose' is still missing after the Docker install"
}

# ---------------------------------------------------------------------------
# questions
# ---------------------------------------------------------------------------

ask_mode() {
    echo
    bold "What should run on this machine?"
    echo "  1) Reflector and transcoder (the AMBE USB sticks are plugged into this machine)"
    echo "  2) Reflector only (no transcoder, or the transcoder runs on another machine)"
    echo "  3) Transcoder only (the reflector runs on another machine)"
    ask MODE "Choice" "1" '^[123]$' "Please enter 1, 2 or 3."
    case "$MODE" in
        1) WITH_URFD=true;  WITH_TCD=true;  TCD_REMOTE=false ;;
        2) WITH_URFD=true;  WITH_TCD=false
           echo
           echo "A transcoder converts audio between D-Star, DMR/YSF, M17, P25 and NXDN."
           echo "Without one, users only hear stations that use the same mode."
           if ask_yn "Will a transcoder on another machine connect to this reflector?" n; then
               TCD_REMOTE=true
           else
               TCD_REMOTE=false
           fi ;;
        3) WITH_URFD=false; WITH_TCD=true;  TCD_REMOTE=false ;;
    esac
}

ask_reflector() {
    echo
    bold "Reflector"
    echo "Reflector callsigns are URF plus 3 letters or digits, e.g. URF363."
    echo "Pick one that is not taken yet, see https://xlxapi.rlx.lu/api/exportreflectors."
    ask CALLSIGN "Reflector callsign" "" '^URF[A-Z0-9]{3}$' "Use URF followed by 3 capital letters or digits, e.g. URF363."
    ask EMAIL "Sysop e-mail address" "" '^[^@ ]+@[^@ ]+\.[^@ ]+$' "Please enter a valid e-mail address."
    ask COUNTRY "Country code (2 letters, e.g. DE, AT, US)" "" '^[A-Z]{2}$' "Two capital letters, e.g. DE."
    ask SPONSOR "Sponsor or club (shown on the dashboard)" "$CALLSIGN"
    ask DASHURL "Dashboard address" "http://$(local_ip):8363"
    echo
    echo "Modules are the 'rooms' of the reflector, letters A to Z."
    ask MODULES "Modules" "ADMSZ" '^[A-Z]{1,26}$' "Capital letters A-Z, e.g. ADMSZ."
    MODULES=$(echo "$MODULES" | fold -w1 | awk '!seen[$0]++' | tr -d '\n')
    ECHO_MODULE=""
    if ask_yn "Use one module as echo/parrot (plays back what you say)?" y; then
        local def=""
        [[ "$MODULES" == *Z* ]] && def="Z"
        ask ECHO_MODULE "Echo module" "$def" "^[$MODULES]$" "Pick one of $MODULES."
    fi
    TC_MODULES=""
    if [ "$WITH_TCD" = true ] || [ "$TCD_REMOTE" = true ]; then
        ask_tc_modules
    fi
    if ask_yn "Enable the admin page in the dashboard (talkgroups, live log)?" y; then
        ADMIN_PASSWORD=$(python3 -c 'import secrets, string; print("".join(secrets.choice(string.ascii_letters + string.digits) for _ in range(12)))')
    else
        ADMIN_PASSWORD=""
    fi
    echo
    echo "The XLX directory lists the reflector in Pi-Star, WPSD and other hotspot software."
    echo "Register only once the reflector is reachable from the internet."
    if ask_yn "Register the reflector in the XLX directory now?" n; then
        CALLHOME=true
        ask CH_COUNTRY "Country name for the directory (e.g. Germany)" ""
        ask CH_COMMENT "Short description for the directory" "$CALLSIGN reflector"
    else
        CALLHOME=false
    fi
}

ask_tc_modules() {
    echo
    echo "Transcoded modules get audio converted between all modes. Each one needs"
    echo "AMBE capacity: one DV3000 stick (with the MD380 software vocoder) handles 1 module,"
    echo "DV3000+DV3003 handle 2, two DV3003 up to 3."
    ask TC_MODULES "Transcoded modules" "${MODULES:0:1}" "^[$MODULES]{1,3}$" "1 to 3 letters out of $MODULES."
}

ask_transcoder() {
    echo
    bold "Transcoder"
    if [ "$WITH_URFD" = true ]; then
        TCD_SERVER="127.0.0.1"
    else
        ask TCD_SERVER "IP address of the reflector" "" '^[0-9.]+$|^[0-9a-fA-F:]+$' "Please enter an IP address."
        echo "Enter the transcoded modules exactly as in the reflector's urfd.ini [Transcoder] Modules."
        ask TC_MODULES "Transcoded modules" "A" '^[A-Z]{1,3}$' "1 to 3 capital letters."
    fi
    echo
    echo "Which AMBE hardware is plugged in?"
    echo "  1) a single DV3000 / ThumbDV / AMBE3000 stick   (DMR/YSF then runs in software, MD380)"
    echo "  2) a DV3003, two DV3000, or DV3000 + DV3003"
    ask HW "Choice" "1" '^[12]$' "Please enter 1 or 2."
    if [ "$HW" = "1" ]; then
        TCD_MD380=true
        if [ ${#TC_MODULES} -gt 1 ]; then
            warn "a single stick handles one transcoded module, only ${TC_MODULES:0:1} will be transcoded"
            TC_MODULES=${TC_MODULES:0:1}
        fi
    else
        TCD_MD380=false
    fi
}

# ---------------------------------------------------------------------------
# configuration
# ---------------------------------------------------------------------------

write_urfd_config() {
    info "Writing the reflector configuration to $CFG_DIR"
    mkdir -p "$CFG_DIR" "$DATA_DIR/dashboard" "$DATA_DIR/xlxd-ch"
    chmod 777 "$DATA_DIR/xlxd-ch"   # written by the dashboard (www-data) in the container
    local ini="$CFG_DIR/urfd.ini"
    cp "$REPO_DIR/config/urfd.ini" "$ini"
    for f in urfd.interlink urfd.whitelist urfd.blacklist urfd.terminal; do
        [ -f "$CFG_DIR/$f" ] || cp "$REPO_DIR/config/$f" "$CFG_DIR/$f"
    done

    set_ini "$ini" Names Callsign "$CALLSIGN"
    set_ini "$ini" Names SysopEmail "$EMAIL"
    set_ini "$ini" Names Country "$COUNTRY"
    set_ini "$ini" Names Sponsor "$SPONSOR"
    set_ini "$ini" Names DashboardUrl "$DASHURL"

    set_ini "$ini" Modules Modules "$MODULES"
    # drop the template's descriptions of modules that do not exist
    local m
    for m in {A..Z}; do
        [[ "$MODULES" == *$m* ]] || del_ini "$ini" Modules "Description$m"
    done
    [ -n "$ECHO_MODULE" ] && set_ini "$ini" Modules "Description$ECHO_MODULE" "Echo"

    if [ -n "$TC_MODULES" ]; then
        set_ini "$ini" Transcoder Port 10100
        set_ini "$ini" Transcoder Modules "$TC_MODULES"
        if [ "$TCD_REMOTE" = true ]; then
            set_ini "$ini" Transcoder BindingAddress 0.0.0.0
        else
            set_ini "$ini" Transcoder BindingAddress 127.0.0.1
        fi
    else
        set_ini "$ini" Transcoder Port 0
        del_ini "$ini" Transcoder Modules
    fi

    if [ -n "$ECHO_MODULE" ]; then
        set_ini "$ini" Echo Enable true
        set_ini "$ini" Echo Module "$ECHO_MODULE"
    else
        set_ini "$ini" Echo Enable false
    fi

    set_ini "$ini" YSF RegistrationID "$((RANDOM % 90000 + 10000))"
    set_ini "$ini" YSF RegistrationName "$COUNTRY $CALLSIGN"
    set_ini "$ini" YSF RegistrationDescription "URF Reflector"

    if [ -n "$ADMIN_PASSWORD" ]; then
        set_ini "$ini" Admin Enable true
        set_ini "$ini" Admin Password "$ADMIN_PASSWORD"
    fi

    # paths inside the container
    local c=/usr/local/etc/urfd
    set_ini "$ini" "DMR ID DB" Mode both
    set_ini "$ini" "DMR ID DB" FilePath "$c/dmrid.dat"
    set_ini "$ini" "NXDN ID DB" Mode both
    set_ini "$ini" "NXDN ID DB" FilePath "$c/nxdn.dat"
    set_ini "$ini" "YSF TX/RX DB" Mode both
    set_ini "$ini" "YSF TX/RX DB" FilePath "$c/ysfnode.dat"
    set_ini "$ini" Files PidPath /var/run/xlxd.pid
    set_ini "$ini" Files XmlPath /var/log/xlxd.xml
    set_ini "$ini" Files WhitelistPath "$c/urfd.whitelist"
    set_ini "$ini" Files BlacklistPath "$c/urfd.blacklist"
    set_ini "$ini" Files InterlinkPath "$c/urfd.interlink"
    set_ini "$ini" Files G3TerminalPath "$c/urfd.terminal"

    local php="$DATA_DIR/dashboard/config.inc.php"
    cp "$REPO_DIR/dashboard/pgs/config.inc.php" "$php"
    set_php "$php" "PageOptions['ContactEmail']" "$(php_str "$EMAIL")"
    if [ -n "$ADMIN_PASSWORD" ]; then
        set_php "$php" "Admin['Enable']" true
        set_php "$php" "Admin['Password']" "$(php_str "$ADMIN_PASSWORD")"
    fi
    set_php "$php" "CallingHome['MyDashBoardURL']" "$(php_str "$DASHURL")"
    if [ "$CALLHOME" = true ]; then
        set_php "$php" "CallingHome['Active']" true
        set_php "$php" "CallingHome['Country']" "$(php_str "$CH_COUNTRY")"
        set_php "$php" "CallingHome['Comment']" "$(php_str "${CH_COMMENT:0:100}")"
    fi
    chown 33:33 "$php"   # www-data in the container
}

write_tcd_config() {
    info "Writing the transcoder configuration to $TCD_INI"
    mkdir -p "$(dirname "$TCD_INI")"
    cp "$REPO_DIR/tcd/config/tcd.ini" "$TCD_INI"
    python3 - "$TCD_INI" "$TCD_SERVER" "$TC_MODULES" "$TCD_SERIALS" <<'PY'
import re, sys
path, server, modules, serials = sys.argv[1:5]
lines = open(path).read().split('\n')
out = []
for l in lines:
    if re.match(r'^\s*ServerAddress\s*=', l):
        l = 'ServerAddress = ' + server
    elif re.match(r'^\s*Modules\s*=', l):
        l = 'Modules = ' + modules
    elif re.match(r'^\s*#?\s*DeviceSerial\s*=', l):
        continue
    out.append(l)
    # the serials go right after the comment that explains them
    if 'tcd --list-devices' in l:
        out += ['DeviceSerial = ' + s for s in serials.split()]
open(path, 'w').write('\n'.join(out))
PY
}

check_urfd_config() {
    info "Checking the configuration with inicheck"
    if ! docker run --rm -v "$CFG_DIR:/usr/local/etc/urfd:ro" urfd:latest \
            inicheck -q /usr/local/etc/urfd/urfd.ini; then
        die "the configuration has errors (see above), fix $CFG_DIR/urfd.ini and run setup.sh again"
    fi
}

# ---------------------------------------------------------------------------
# build and start
# ---------------------------------------------------------------------------

build_urfd() {
    info "Building the reflector image (this takes a few minutes, longer on a Raspberry Pi)"
    (cd "$REPO_DIR/docker" && docker compose build)
}

start_urfd() {
    info "Starting the reflector"
    (cd "$REPO_DIR/docker" && docker compose up -d --force-recreate)
}

build_tcd() {
    info "Building the transcoder (installs the vocoder libraries first)"
    if [ "$TCD_MD380" = true ]; then
        bash "$REPO_DIR/tcd/build.sh" --with-md380
    else
        bash "$REPO_DIR/tcd/build.sh"
    fi
    echo "MD380=$TCD_MD380" >"$TCD_BUILD_CONF"
}

ask_tcd_serials() {
    echo
    echo "Connected USB serial devices (the AMBE sticks show up here):"
    /usr/local/bin/tcd --list-devices || true
    echo
    echo "Enter the serial number(s) of the AMBE device(s), separated by spaces."
    echo "Leave empty to use every AMBE device tcd finds."
    ask TCD_SERIALS "Serial numbers" "" '^[A-Za-z0-9 ]*$' "Letters and digits only."
}

start_tcd() {
    info "Starting the transcoder"
    systemctl daemon-reload
    systemctl enable tcd >/dev/null 2>&1
    systemctl restart tcd
}

# ---------------------------------------------------------------------------
# summary
# ---------------------------------------------------------------------------

summary() {
    echo
    bold "========================================================================"
    bold " Done"
    bold "========================================================================"
    if [ "$WITH_URFD" = true ]; then
        cat <<EOF

 Reflector $CALLSIGN
   Dashboard:      $DASHURL
   Configuration:  $CFG_DIR/urfd.ini   (more protocols, BrandMeister, interlinks)
   After changes:  docker restart urfd
   Logs:           docker logs -f urfd
EOF
        [ -n "$ADMIN_PASSWORD" ] && echo "   Admin page:     π symbol in the dashboard footer, password: $ADMIN_PASSWORD"
        cat <<EOF

 Open these ports in your router / firewall (UDP unless noted):
   8363/tcp dashboard, 17000 M17, 42000 YSF, 8880 DMR+, 20001 DPlus,
   30001 DExtra, 30051 DCS, 41000 P25, 41400 NXDN, 10017 URF
   (62030 for DMR hotspots once you enable [MMDVM] in urfd.ini)
EOF
        [ "$TCD_REMOTE" = true ] && echo "   10100/tcp for the transcoder, from the transcoder machine only"
    fi
    if [ "$WITH_TCD" = true ]; then
        cat <<EOF

 Transcoder (modules $TC_MODULES)
   Configuration:  $TCD_INI
   After changes:  systemctl restart tcd
   Logs:           journalctl -u tcd -f
   Monitor:        http://$(local_ip):8080   or 'tcdmon' in a terminal
EOF
    fi
    cat <<EOF

 Update later with:  sudo $REPO_DIR/setup.sh update
 Setup log:          $LOG_FILE

EOF
}

# ---------------------------------------------------------------------------
# update
# ---------------------------------------------------------------------------

update() {
    local have_urfd=false have_tcd=false
    [ -f "$CFG_DIR/urfd.ini" ] && have_urfd=true
    [ -f "$TCD_INI" ] && have_tcd=true
    [ "$have_urfd" = true ] || [ "$have_tcd" = true ] || die "nothing installed yet, run 'sudo ./setup.sh' first"

    info "Updating the source in $REPO_DIR"
    repo_git pull --ff-only

    if [ "$have_urfd" = true ]; then
        install_docker
        build_urfd
        # the dashboard version shown in the footer lives in the kept config
        local ver
        ver=$(sed -n "s/^\$PageOptions\['DashboardVersion'\] *= *'\(.*\)';/\1/p" "$REPO_DIR/dashboard/pgs/config.inc.php")
        [ -n "$ver" ] && set_php "$DATA_DIR/dashboard/config.inc.php" "PageOptions['DashboardVersion']" "'$ver'"
        start_urfd
    fi
    if [ "$have_tcd" = true ]; then
        TCD_MD380=false
        if [ -f "$TCD_BUILD_CONF" ] && grep -q '^MD380=true' "$TCD_BUILD_CONF"; then
            TCD_MD380=true
        fi
        build_tcd   # restarts tcd if it was running
        systemctl is-active --quiet tcd || systemctl start tcd
    fi
    info "Update complete"
}

# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------

main() {
    check_system
    # the log ends up holding the admin password
    touch "$LOG_FILE" && chmod 600 "$LOG_FILE"
    exec > >(tee -a "$LOG_FILE") 2>&1
    echo "---- setup.sh $(date -Is) ----" >>"$LOG_FILE"
    install_base_packages

    if [ "${1:-}" = "update" ]; then
        update
        return
    fi
    if [ -f "$CFG_DIR/urfd.ini" ] || [ -f "$TCD_INI" ]; then
        echo
        echo "An existing installation was found."
        if ask_yn "Update it, keeping the configuration?" y; then
            update
            return
        fi
        ask_yn "Set it up again? This overwrites urfd.ini, the dashboard config and tcd.ini." n || exit 0
    fi

    bold "urfd / tcd setup"
    ask_mode
    [ "$WITH_URFD" = true ] && ask_reflector
    [ "$WITH_TCD" = true ] && ask_transcoder

    if [ "$WITH_URFD" = true ]; then
        install_docker
        write_urfd_config
        build_urfd
        check_urfd_config
        start_urfd
    fi
    if [ "$WITH_TCD" = true ]; then
        build_tcd
        ask_tcd_serials
        write_tcd_config
        start_tcd
    fi
    summary
}

main "$@"
