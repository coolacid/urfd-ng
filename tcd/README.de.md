# tcd — Hybrider Transcoder für digitale Sprache

[English](README.md) | **Deutsch**

**Version 0.2.0** — der Transcoder des [urfd](../README.de.md)-Reflektors, im Ordner `tcd/` desselben Repositorys. Fork von [n7tae/tcd](https://github.com/n7tae/tcd).

tcd wandelt Sprache zwischen den digitalen Betriebsarten eines URF-Reflektors um, damit ein D-Star-Nutzer einen DMR-Nutzer hört und umgekehrt. Für D-Star (AMBE) und DMR/YSF (AMBE2+) nutzt er DVSI-AMBE-Hardware, für M17 (Codec2), P25/NXDN (IMBE) und optional DMR/YSF (MD380) Software-Vocoder. Der Reflektor gibt das Audio seiner transcodierten Module über TCP an tcd und bekommt alle anderen Betriebsarten zurück.

## Inhalt

- [Installation](#installation)
- [Hardware](#hardware)
- [AMBE-Geräte auswählen](#ambe-geräte-auswählen)
- [Konfiguration (tcd.ini)](#konfiguration-tcdini)
- [Pegel](#pegel)
- [Überwachung](#überwachung)
- [Betrieb und Fehlersuche](#betrieb-und-fehlersuche)
- [Änderungen gegenüber dem Original](#änderungen-gegenüber-dem-original)

## Installation

### Mit dem Setup-Script

Am einfachsten: Das Setup-Script im Wurzelverzeichnis des Repositorys installiert Reflektor und Transcoder zusammen oder den Transcoder allein auf einem zweiten Rechner, stellt ein paar Fragen und schreibt `tcd.ini`. Siehe [docs/INSTALL.de.md](../docs/INSTALL.de.md).

```bash
git clone https://github.com/jcmerg/urfd-ng.git urfd
cd urfd
sudo ./setup.sh
```

### Mit build.sh

Nur den Transcoder bauen und installieren:

```bash
git clone https://github.com/jcmerg/urfd-ng.git urfd
cd urfd
sudo bash tcd/build.sh               # two or more AMBE devices
sudo bash tcd/build.sh --with-md380  # a single DV3000, DMR/YSF in software
```

`build.sh` installiert die Build-Werkzeuge und die Vocoder-Bibliotheken ([imbe_vocoder](https://github.com/jcmerg/imbe_vocoder) und mit `--with-md380` [md380_vocoder](https://github.com/jcmerg/md380_vocoder): nativ auf armhf, über [dynarmic](https://github.com/jcmerg/md380_vocoder_dynarmic) auf x86_64 und arm64), baut tcd als Eigentümer des Klons und installiert `tcd`, `tcdmon`, `agc-analyze` und die systemd-Unit. Ein laufender tcd wird neu gestartet. Bei der ersten Installation landet die Vorlage unter `/usr/local/etc/tcd.ini`; die bitte anpassen (siehe [Konfiguration](#konfiguration-tcdini)) und mit `sudo systemctl start tcd` starten.

Aktualisieren: im Klon `git pull` und `build.sh` erneut aufrufen, oder `sudo ./setup.sh update`.

### Von Hand

Mit installierten Bibliotheken:

```bash
cd urfd/tcd
make -j$(nproc)             # without MD380
make -j$(nproc) md380=true  # with the MD380 software vocoder
sudo make install
```

tcd baut gegen die urfd-Quellen, die er mit dem Reflektor teilt (`TCPacketDef.h`, `TCSocket.*`, ...), aus `../reflector`; beide Seiten sprechen also immer dasselbe Protokoll. `config/tcd.ini` ist die kommentierte Vorlage, `config/tcd.service` die systemd-Unit.

### Voraussetzungen

- Linux mit systemd: Debian, Ubuntu oder Raspberry Pi OS; x86_64, arm64 oder armhf
- DVSI-AMBE-USB-Geräte (siehe [Hardware](#hardware)); sie laufen über den Kernel-Treiber `ftdi_sio`, ein zusätzlicher Treiber ist nicht nötig
- Geringe Latenz zu den AMBE-Geräten: tcd läuft direkt auf dem System, nicht in Docker. Er kann auf dem Rechner des Reflektors laufen oder auf einem anderen, z.B. einem Raspberry Pi neben der Funktechnik.

## Hardware

Jedes transcodierte Modul braucht einen D-Star-Kanal und einen DMR/YSF-Kanal. Ein DV3000 (ThumbDV, DVstick-30, USB-3000) hat einen Kanal, ein DV3003 (DVstick-33, USB-3003) drei. Fehlt AMBE-Hardware für DMR/YSF, übernimmt der Software-Vocoder MD380 diesen Teil (bauen mit `md380=true` / `--with-md380`).

| Hardware | Build | Module | Hinweis |
|---|---|---|---|
| 1× DV3000 oder 1× DV3003 | MD380 nötig | 1 | D-Star auf dem Gerät, DMR/YSF in Software |
| DV3000 + DV3003 | mit oder ohne MD380 | 2 | Mischbetrieb, empfohlen |
| 2× DV3000 | mit oder ohne MD380 | 1 | eines für D-Star, eines für DMR/YSF |
| 2× DV3003 | mit oder ohne MD380 | bis zu 3 | eines für D-Star, eines für DMR/YSF |

Ein einzelnes Gerät braucht immer den MD380-Build; ohne ihn startet tcd nicht. Bei zwei Geräten ermöglicht MD380 zusätzlich das Neukodieren von DMR (siehe [`DmrGainMode`](#pegel-dmrysf--dmrysf-dmrgainmode)). tcd erkennt die Geräte und wählt den Betrieb selbst.

### Mischbetrieb: DV3000 + DV3003

Zwei Module gleichzeitig, die Kanäle werden automatisch belegt:

| Kanal | Verwendung |
|---|---|
| DV3000 ch0 | D-Star, erstes Modul |
| DV3003 ch0 | D-Star, zweites Modul |
| DV3003 ch1 | DMR/YSF, erstes Modul |
| DV3003 ch2 | DMR/YSF, zweites Modul |

```ini
Modules = FS            # first = D-Star on the DV3000, second = D-Star on the DV3003
DeviceSerial = DQ015SBR # DV3000
DeviceSerial = DKB7FXGE # DV3003
```

### Einzelnes Gerät mit MD380

Ein Modul, D-Star auf dem DV3000 oder DV3003, DMR/YSF über den MD380. In diesem Betrieb wird DMR/YSF → DMR/YSF nach der AGC **nicht neu kodiert**, weil der MD380 nicht im selben Ablauf dekodieren und neu kodieren kann (gemeinsamer Zustand von Kodierer und Dekodierer). Für diesen Weg die Bitstrom-Verstärkung (`DmrGainMode = ambe`) verwenden; alle Wege zwischen verschiedenen Betriebsarten bekommen weiterhin die volle AGC. Neukodieren bei DMR/YSF → DMR/YSF braucht ein zweites AMBE-Gerät.

### Hinweise zum DV3003

- **Companding aus**: Bei jedem DV3003-Kanal wird Companding ausdrücklich abgeschaltet. Der DV3003 startet mit eingeschaltetem Companding (Pull-up an CP_ENABLE), der DV3000 mit ausgeschaltetem; ohne die Abschaltung bekamen die DMR/YSF-Kanäle µ-law-PCM, das als linear gelesen wurde, und das Audio war verzerrt.
- **Seriellen Puffer leeren**: Vor dem Soft-Reset leeren 350 Null-Bytes einen hängengebliebenen seriellen Zustand.
- **Codec pro Kanal**: Ein DV3003 kann gleichzeitig D-Star- und DMR/YSF-Kanäle betreiben (Mischbetrieb).

## AMBE-Geräte auswählen

```bash
tcd --list-devices
```

listet die seriellen USB-Geräte aus `/dev/serial/by-id` mit ihren Seriennummern und markiert die, die nach AMBE-Geräten aussehen.

Ohne `DeviceSerial` in `tcd.ini` verwendet tcd jedes Gerät, das er am Namen als AMBE erkennt. Hängen weitere serielle USB-Geräte am Rechner (GPS, CAT-Kabel, ...), die AMBE-Geräte ausdrücklich eintragen. `DeviceSerial` muss exakt einem davon entsprechen:

- der USB-Seriennummer (`DKB7FXGE`)
- der älteren libftd2xx-Form mit dem Port-Buchstaben von Mehrport-Geräten (`DA1234A`)
- dem Namen unter `/dev/serial/by-id`
- der Gerätedatei (`/dev/ttyUSB0`)

tcd öffnet die Geräte über den Kernel-Treiber `ftdi_sio` und setzt ihren USB-Latenz-Timer auf 1 ms (der Standard des Treibers ist 16 ms, zu langsam für das AMBE-Timing; das Log zeigt den Wert). Geräte, die ein älterer, auf libftd2xx basierender tcd vom `ftdi_sio`-Treiber gelöst hat, werden beim Start wieder gebunden; ein Umstieg braucht also kein erneutes Einstecken.

## Konfiguration (tcd.ini)

`/usr/local/etc/tcd.ini`; die Vorlage in [`config/tcd.ini`](config/tcd.ini) erklärt jeden Schlüssel. Nach Änderungen tcd neu starten (`sudo systemctl restart tcd`), oder die Audio-Einstellungen live im [Web-Dashboard](#web-dashboard) ändern.

### Verbindung und Geräte

| Schlüssel | Vorlage | Bedeutung |
|---|---|---|
| `ServerAddress` | `127.0.0.1` | Adresse des urfd-Reflektors |
| `Port` | `10100` | `[Transcoder] Port` des Reflektors |
| `Modules` | `FS` | transcodierte Module, dieselben Buchstaben wie `[Transcoder] Modules` im Reflektor |
| `DeviceSerial` | — | zu verwendendes AMBE-Gerät, je eine Zeile (siehe [oben](#ambe-geräte-auswählen)) |

### Verstärkung

Alle Werte in dB, -40 bis +40.

| Schlüssel | Vorlage | Wirkt auf |
|---|---|---|
| `OutputGainDStar` | `0` | D-Star-Ausgang, nach der AGC |
| `OutputGainDMR` | `0` | DMR/YSF-Ausgang, nach der AGC |
| `OutputGainM17` | `0` | M17/Codec2-Ausgang, nach der AGC |
| `OutputGainIMBE` | `0` | P25/NXDN-Ausgang, nach der AGC |
| `OutputGainUSRP` | `0` | USRP-Ausgang, nach der AGC |
| `DStarGainIn` / `DStarGainOut` | `0` | im DVSI-Chip, D-Star dekodieren / kodieren |
| `DmrGainIn` / `DmrGainOut` | `0` | im DVSI-Chip, DMR dekodieren / kodieren |
| `UsrpGainIn` / `UsrpGainOut` | `0` | USRP-Audio ein / aus |

Die SVX-Verstärkung wird in der `urfd.ini` des Reflektors eingestellt (`RxGain`/`TxGain`), nicht hier.

### Pegel DMR/YSF → DMR/YSF (`DmrGainMode`)

Audio von DMR nach DMR/YSF läuft normalerweise als AMBE2+ durch, ohne dekodiert zu werden; die AGC kommt daran nicht heran. `DmrGainMode` legt fest, wie sein Pegel angepasst wird:

| Modus | Braucht | Wirkung |
|---|---|---|
| `off` | — | keine Anpassung |
| `ambe` | — | ändert den Verstärkungsparameter b2 (delta-gamma) direkt im AMBE2+-Frame, mit `AmbeGainDb` (-30 bis 0 dB, etwa 2 dB pro Stufe). Kein Dekodieren, kein Qualitätsverlust, repariert nebenbei Golay-FEC-Fehler. Experimentell. |
| `reencode` | MD380 | vollständiges Dekodieren und Neukodieren über den MD380, mit AGC und `DmrReencodeGain` (gleicht die Lautheit des Neukodierens aus, etwa -16 dB). Ohne MD380 Rückfall auf `ambe`. |

| Schlüssel | Vorlage | Standard im Code |
|---|---|---|
| `DmrGainMode` | `ambe` | `ambe` |
| `AmbeGainDb` | `-2` | `-2` |
| `DmrReencodeGain` | `-16` | — |

### AGC

| Schlüssel | Vorlage | Standard im Code | Bedeutung |
|---|---|---|---|
| `AGC` | `true` | `false` | automatische Pegelregelung ein/aus |
| `AGCTarget` | `-16` | `-16` | Zielpegel in dBFS |
| `AGCAttack` | `50` | `50` | ms, um lautes Audio abzusenken |
| `AGCRelease` | `500` | `500` | ms, um leises Audio anzuheben |
| `AGCMaxGainUp` | `30` | `30` | höchste Verstärkung in dB |
| `AGCMaxGainDown` | `24` | `24` | höchste Absenkung in dB |
| `AGCNoiseGate` | `-55` | `-55` | unterhalb dieses Pegels (dBFS) wird die Verstärkung gehalten |

### Überwachung und Protokoll

| Schlüssel | Vorlage | Bedeutung |
|---|---|---|
| `Monitor` | `true` | Web-Dashboard und Statistik-Socket ein/aus |
| `MonitorHttpPort` | `8080` | Web-Dashboard |
| `MonitorStatsPort` | `8081` | Statistik-Socket (JSON, für `tcdmon`) |
| `StatsLog` | `false` | CSV-Dateien pro Stream zum Einstellen der AGC |
| `StatsLogDir` | `/tmp/tcd-stats` | Ablageort |
| `StatsLogRetain` | `24` | Stunden, bis sie gelöscht werden |

## Pegel

Jede Umwandlung läuft über PCM:

```
D-Star/DMR (DVSI hardware):
  AMBE --[DVSI GainIn]--> PCM --[AGC]--> PCM --[OutputGain]--> [DVSI GainOut] --> target AMBE

M17/P25 (software vocoder):
  codec --[decode]--> PCM --[AGC]--> PCM --[OutputGain]--> target codec

USRP/SVX (PCM):
  PCM --[UsrpGainIn]--> PCM --[AGC]--> PCM --[OutputGain]--> target codec

DMR -> DMR/YSF with DmrGainMode = reencode:
  AMBE2+ --[decode]--> PCM --[AGC]--> PCM --[OutputGainDMR]--> [DmrReencodeGain] --> MD380 --> AMBE2+
```

**Mit AGC** können die festen Verstärkungen meist alle auf 0 bleiben: Sie gleicht unterschiedliche Mikrofone, Funkgeräte und Codecs pro Aussendung aus. Ohne sie braucht jeder Weg seine eigene feste Verstärkung.

Die AGC arbeitet pro Stream (pro Aussendung):
- RMS über ein gleitendes Fenster von 3 Frames (60 ms), folgt also Silben statt einzelnen Lauten
- ein langsamer Mittelwert (etwa 2 s) der Verstärkung, die jeder Sprecher braucht; in Pausen läuft die Verstärkung dorthin zurück, damit die erste Silbe nach einer Pause gleich richtig kommt
- schnelleres Anheben in den ersten 5 Frames nach dem Öffnen des Gates
- ein Begrenzer bei -0,1 dBFS, es übersteuert also nie

Die Grenzen sind absichtlich ungleich: Absenken ist unkritisch, Anheben hebt auch das Rauschen. Typisches DMR-Audio liegt um -35 dBFS, für ein Ziel von -16 dBFS braucht es also etwa +20 dB.

## Überwachung

### Web-Dashboard

`http://<tcd-host>:8080`:

- Signalweg des aktiven Pfads mit Live-Pegeln an jeder Stufe
- VU-Meter vor und nach der AGC, mit Spitzenwert-Anzeige
- AGC- und Verstärkungsregler, die sofort wirken, und ein Knopf, um sie in `tcd.ini` zu speichern
- DVSI-Geräte: Seriennummer, Typ, Rolle, belegte Kanäle mit ihren Modulen, Puffertiefe
- MD380-Status und -Zähler, Verbindung zum Reflektor und Paketzähler

### tcdmon

Monitor im Terminal, praktisch über SSH:

```bash
tcdmon                      # this machine, port 8081
tcdmon 192.0.2.20 8081      # another machine
```

Zeigt Pegelbalken, Signalweg, AGC-Zustand, die DVSI-Kanäle mit ihren Modulen, die MD380-Zähler und die Ausgangsverstärkungen. `q` beendet.

### Statistik-CSV und agc-analyze

Mit `StatsLog = true` schreibt tcd pro Stream eine CSV-Datei nach `StatsLogDir` (z.B. `F_20260402_003512_43521.csv`, Spalten `ms,module,codec,rms_in,peak_in,rms_out,peak_out,agc_gain,gate`). `agc-analyze` wertet sie aus:

```bash
agc-analyze               # details, default directory /tmp/tcd-stats
agc-analyze -s            # one line per stream
agc-analyze -s -c dmr     # by codec: dstar, dmr, p25, usrp, svx
agc-analyze -s -m F       # by module
agc-analyze file.csv      # one file
```

Es zeigt pro Stream Dauer, Sprach- und Gate-Frames, Ein- und Ausgangspegel, den Bereich der AGC-Verstärkung und den Anteil an Übersteuerung.

## Betrieb und Fehlersuche

```bash
sudo systemctl status tcd
sudo systemctl restart tcd
journalctl -u tcd -f
```

| Meldung / Problem | Prüfen |
|---|---|
| `Could not find an AMBE device!` | `tcd --list-devices`; ist `DeviceSerial` gesetzt, muss es exakt passen (siehe [oben](#ambe-geräte-auswählen)) |
| `No USB serial devices found` | kein Gerät eingesteckt, oder der Treiber `ftdi_sio` fehlt (`lsmod \| grep ftdi_sio`) |
| Warnung `USB latency timer is 16 ms` | tcd konnte den Latenz-Timer nicht setzen; er muss als root laufen (die systemd-Unit tut das) |
| `Lost the AMBE device ..., shutting down` | das Gerät wurde abgezogen oder zurückgesetzt; systemd startet tcd nach 5 s neu, und er findet das Gerät wieder |
| der Reflektor zeigt keinen Transcoder | `ServerAddress` und `Port` in `tcd.ini`, auf beiden Seiten dieselben `Modules`, und Port 10100/tcp vom tcd-Rechner aus erreichbar |

## Änderungen gegenüber dem Original

### Hardware und Geräte
- **Serieller Zugriff ohne libftd2xx**: über den Kernel-Treiber `ftdi_sio` und `/dev/serial/by-id` (POSIX-Anbindung von Jason Kendall), RTS/CTS-Flusskontrolle, USB-Latenz-Timer auf 1 ms; von einem älteren tcd gelöste Geräte werden wieder gebunden; ein verlorenes Gerät startet tcd über systemd neu
- **Geräteauswahl**: `--list-devices`, `DeviceSerial` mit exaktem Vergleich
- **Mischbetrieb DV3000 + DV3003**: zwei Module gleichzeitig mit Codec-Einstellung pro Kanal
- **D-Star auf dem DV3003**: Companding aus, seriellen Puffer leeren, Kodierung pro Kanal
- **SVX-Codec-Pfad**: eigener `ECodecType::svx` für SVX-Audio

### Audio
- **Ausgangsverstärkung pro Codec**: nach der AGC, für D-Star, M17/Codec2, DMR/YSF, P25/NXDN und USRP, angewendet auf Kopien (der gemeinsame Puffer wird nie verändert)
- **AMBE2+-Bitstrom-Verstärkung** (experimentell): `AmbeGainDb` ändert den Parameter b2 im AMBE2+-Frame, mit neu berechnetem Golay(24,12)-FEC für die A-Partition
- **AGC v3**: gleitendes RMS-Fenster, Verfolgung pro Sprecher, Gate mit Rücklauf, schnelles Anheben nach dem Gate, ungleiche Grenzen, Noise-Gate, Begrenzer, Änderungen live aus dem Dashboard
- **DMR neu kodieren** (`md380=true`): `DmrGainMode = reencode` mit AGC und `DmrReencodeGain` für DMR → DMR/YSF
- **MD380 pro Stream getrennt**: Kodierer-Zustand pro Stream gesichert, MD380-Aufrufe nacheinander

### Überwachung und Konfiguration
- **Web-Dashboard**: eingebauter HTTP/WebSocket-Server mit Signalweg, VU-Metern, Verstärkungs- und AGC-Reglern, Geräte- und MD380-Status, Speichern in die INI
- **tcdmon**: Monitor im Terminal
- **Statistik-CSV** mit automatischem Aufräumen und dem Werkzeug `agc-analyze`
- **Umbenannte Schlüssel**: `DmrGainIn/Out` (war `DmrYsfGainIn/Out`), `UsrpGainIn/Out` (war `UsrpRxGain/TxGain`), Verstärkungen erweitert auf ±40 dB

### Zuverlässigkeit
- **MD380 optional**: Ohne ihn sind zwei AMBE-Geräte nötig, und Neukodieren gibt es nicht
- **Leistung**: blockierendes Lesen mit Timeout statt Dauerabfrage, etwa 5 % CPU auf einem Raspberry Pi
- **Threadsicherheit**: Sperre um den IMBE-Vocoder, SIGPIPE ignoriert für saubere Neuverbindungen
- **Fehlerbehandlung**: Wiederholung zum Reflektor mit wachsender Pause, Verwerfen bei Überlauf der Warteschlange, Erholung bei Timeouts beim Lesen
- **Wachsende Pause beim Neuverbinden**: 0,5 s bis 5 s, solange der Reflektor nicht erreichbar ist
- **Build**: Teil des urfd-Repositorys, `build.sh` baut als Eigentümer des Klons

## Copyright

- Copyright (c) 2021-2023 Thomas A. Early N7TAE, Doug McLain AD8DP
- POSIX-Anbindung: Jason Kendall (2026)
- Copyright (c) 2026 Jens-Christian Merg DL4JC (Verbesserungen des Forks)
