# Steinel Mesh Gateway für ESP32-C3

[English](README.md) · [Polski](README_PL.md) · [Français](README_FR.md)

> **Eigenständiges ESP32-C3 Bluetooth-Mesh-Gateway für lokale Steinel-Steuerung, Diagnose, Firmware-Aktualisierungen und Home-Assistant-Integration.**

```text
Steinel-Geräte <-> Bluetooth Mesh <-> ESP32-C3 Gateway -> Home Assistant
```

Eigenständiges Bluetooth-Mesh-Gateway für Steinel-Installationen mit lokaler Weboberfläche und Home Assistant über die native ESPHome-API. **2.0.0** unterstützt mehrere Geräte aus einem Netzwerk.

Autor und Maintainer: **Bartosz Supcziński** — <bartek@env.pl>

## Funktionsweise

Steinel-Connect-Mesh-Geräte verwenden Bluetooth Mesh, während Home Assistant über ein IP-Netzwerk kommuniziert. Der ESP32-C3 verbindet beide Umgebungen, tritt der vorhandenen Installation bei und veröffentlicht Gerätebefehle und Status über ESPHome.

Importieren Sie das bestehende Netzwerk aus Steinel Cloud oder einer von der App exportierten JSON-Sicherung. Wählen Sie Geräte per Checkbox in der Weboberfläche. Nach dem Neustart werden ihre unterstützten Entitäten in ESPHome registriert und von Home Assistant erkannt. Keine manuelle Schlüsselextraktion, gerätespezifische YAML-Dateien, MQTT-Broker oder eigene HA-Integration erforderlich.

Cloud-Zugriff dient der Einrichtung. Der tägliche Betrieb erfolgt lokal über Bluetooth Mesh und WLAN.

## Abbildungen

### ESP32-C3 Super Mini

![ESP32-C3 Super Mini](docs/images/esp32-c3-super-mini.jpg)

### Lokale Weboberfläche

Die integrierte Seite bietet Einrichtung, Steuerung, Diagnose und Firmware-Aktualisierungen im Browser.

![Lokale Steinel-Mesh-Gateway-Weboberfläche](docs/images/steinel-web-interface.png)

### Gerät in Home Assistant

Die standardmäßige ESPHome-Integration stellt jedes ausgewählte Gerät separat in Home Assistant bereit. Die folgende Abbildung zeigt NightmatIQ Plus.

![NightmatIQ-Gerät in Home Assistant](docs/images/home-assistant-device.png)

### Optionaler Home-Assistant-Steuerdialog

Ein optionales Frontend-Modul fasst Sensorzustand, Beleuchtungsstärke, Betriebsart und Dämmerungsschwelle in einem kompakten Dialog für NightmatIQ Plus zusammen.

![NightmatIQ-Steuerdialog in Home Assistant](docs/images/home-assistant-control.png)

## Funktionen

### Lokale Bluetooth-Mesh-Integration

- Import einer Steinel-Netzwerksicherung aus der Cloud oder einer von der App exportierten JSON-Datei.
- Wiederherstellung von Netzwerk-, Anwendungs- und Geräteschlüsseln, IV Index und Gerätekatalog.
- Direkte Kommunikation mit ausgewählten Geräten über Bluetooth Mesh.
- Lesen von Ausgangszustand, Beleuchtungsstärke, Dämmerungsschwelle, Firmwareversion, Hardwareversion und Produktidentität.
- Steuerung von Betriebsart, Ausgang, Helligkeit und Automatik entsprechend den erkannten Funktionen.
- Einstellung von Lichtschwelle und Nachlaufzeit entsprechend dem Geräteprofil.

### Zuverlässige Adress- und Sitzungsverwaltung

- Auswahl einer Gateway-Mesh-Adresse aus dem unbelegten Teil des Provisioner-Bereichs.
- Automatische Wiederherstellung, wenn Mesh-Teilnehmer eine wiederverwendete Quelladresse ablehnen.
- Dauerhafte Bestätigung der ersten funktionierenden Quelladresse, damit spätere Neustarts oder eine vorübergehende Nichterreichbarkeit des Sensors keinen unnötigen Adresswechsel auslösen.
- Erhalt der Mesh-Einstellungen bei normalen Neustarts und OTA-Aktualisierungen.
- Begrenzte Wiederholungsversuche und kontrollierte Neustarts beim Wechsel zwischen Cloud- und Bluetooth-Betrieb.

### Weboberfläche des Geräts

- Netzwerkimport aus Steinel Cloud oder lokaler Sicherung und Geräteauswahl per Checkbox.
- Direkte Steuerung und manuelle Statusabfrage.
- Anzeige der installierten Konfiguration und erweiterter Diagnosedaten.
- Mesh-RSSI, Übertragungs-, Antwort- und Timeout-Zähler sowie freier RAM und größter freier Block.
- Vier Websprachen mit gespeicherter Auswahl.
- Nur-Lese-Geräteberichte als JSON ohne Schlüssel und private Netzwerkkennungen.
- Kennwortgeschützte Browser-Aktualisierung.
- Gateway-Verwaltung für automatische und manuelle Firmware-Aktualisierungen, Administratorkennwort, WLAN-Einstellungen und vollständiges Zurücksetzen auf Werkseinstellungen.

### Home-Assistant-Integration

Die standardmäßige ESPHome-API veröffentlicht:

- bestätigten Ausgangszustand;
- Helligkeit, Automatik und Nachlaufzeit, sofern unterstützt;
- Bewegung/Präsenz, sofern gemeldet;
- Verfügbarkeit jedes Geräts;
- gemessene Beleuchtungsstärke;
- Betriebsart;
- Dämmerungsschwelle;
- Bluetooth-Mesh-Bereitschaft und Status;
- Signalstärke und aktuelle Gateway-IP-Adresse;
- installierte Firmware und Hardwareversion;
- Hersteller, Company ID und Product ID;
- eine Aktion zur manuellen Aktualisierung.

Home Assistant zeigt jeden ausgewählten Mesh-Knoten als separates Gerät mit seinen eigenen Entitäten an. Die native ESPHome-Integration benötigt keinen MQTT-Broker, keine eigene Integration und keine YAML-Datei je Lampe.

## Hardware und Kompatibilität

Erforderlich sind ein ESP32-C3 Super Mini mit 4 MB Flash, eine 2,4-GHz-WLAN-Verbindung und eine Steinel-Connect-Mesh-Installation mit Zugriff auf Cloud oder App-Netzwerksicherung. Für Erstinstallation und Wiederherstellung wird die native USB/JTAG-Seriell-Verbindung verwendet.

Unter Linux erscheint die USB-Schnittstelle normalerweise als Espressif USB JTAG/serial (`303a:1001`) und als `/dev/ttyACM*`.

Die Firmware ist für ESP32-C3 und ESP-IDF ausgelegt. Erweiterte Bluetooth-5-Funktionen sind deaktiviert, weil Bluetooth Mesh den BLE-4.2-Advertising-Pfad verwendet. Die Konfiguration berücksichtigt den begrenzten Arbeitsspeicher des ESP32-C3.

### Unterstützte Funktionen

| Profil / Modell | Funktionen |
|---|---|
| Gerätemodus | Auto / Always On / Always Off |
| Generic OnOff Server `1000` | Bestätigter Ausgangszustand, On/Off-Auswahl |
| Light Lightness Server `1300` | Helligkeit als Number-Entität, 0–100% |
| Light LC Server `130F` | Automatikschalter, Lichtschwelle und Nachlaufzeit |
| Sensor Server `1100` | Beleuchtungsstärke und Bewegung/Präsenz, sofern das Gerät diese Eigenschaften meldet |
| Scene / Scheduler | In Metadaten erkannt; kein allgemeiner Szeneneditor oder Zeitplanprogrammierer |

Das Gateway prüft **gebundene SIG-Modelle** und authentifizierte Composition Data: Elementanzahl, Company/Product IDs und importierte Fähigkeiten. Eine fehlende Antwort blockiert gültige Kommunikation mit dem importierten AppKey nicht; bestätigte Abweichungen sperren die Steuerung.

Gruppen, Nachbargruppen und Master/Slave werden in Steinel Connect konfiguriert. Das Gateway verwendet die vorhandene Netzwerkkonfiguration unverändert.

Ausgänge nutzen das erste geeignete Modell; ein Lightness-Ausgang hat Vorrang vor einem separaten OnOff-Element. Alle Sensor-Elemente werden abgefragt. Mehrere unabhängige Ausgänge eines Geräts werden nicht als getrennte Kanäle dargestellt. Sensoren bleiben unbekannt, bis die entsprechende Eigenschaft empfangen wird. Ein versendeter Befehl gilt nicht als bestätigter Gerätezustand.

### Geräte und Kompatibilität

| Geräte | Anbindung | Umfang / Voraussetzung |
|---|---|---|
| NightmatIQ Plus, IS 180 (Mesh-Variante) | SIG-Modelle | Steuerung und Messwerte entsprechend Ausstattung und Firmware des Geräts |
| L 800 SC, L 810 SC / C, L 820 SC, L 830 SC / C, L 835 SC / C, L 840 SC / C | SIG-Modelle | Steuerung und Messwerte entsprechend Ausstattung und Firmware des Geräts |
| L 40 SC / C, L 42 SC / C | SIG-Modelle | Steuerung und Messwerte entsprechend Ausstattung und Firmware des Geräts |
| L 270 digi SC, L 271 digi SC / C | SIG-Modelle | Steuerung und Messwerte entsprechend Ausstattung und Firmware des Geräts |
| RS 200 SC / C, GL 80 SC / C | SIG-Modelle | Steuerung und Messwerte entsprechend Ausstattung und Firmware des Geräts |
| Spot One SC, Spot Duo SC, Spot Way SC, Spot Garden SC, XLED home 2 SC | SIG-Modelle | Steuerung und Messwerte entsprechend Ausstattung und Firmware des Geräts |

Entscheidend sind die tatsächlichen Modelle und die Firmware, nicht allein der Produktname. Das Gateway prüft diese automatisch. Sensorlose C-Varianten erhalten dadurch keine eigenen Sensoren.

Voraussetzungen für alle Einträge: gültiges Connect-Netzwerkbackup, Company ID `0x0563`, unterstützte Modelle mit importierter AppKey-Bindung und Einhaltung der folgenden Grenzen. Proprietäre ältere Bluetooth-Firmware, unverbundene Varianten, reine Z-Wave-/KNX-/DALI-Produkte und reine Client-Taster sind nicht eingeschlossen. Die Mesh-Konvertierung kann unwiderruflich sein und die Kompatibilität mit älteren Geräten verändern; vorher die [Steinel Connect FAQ](https://www.steinel-shop.de/service/produktinfos-beratung/faq/faq-connect-app/) prüfen.

Siehe [Kompatibilitätsdetails](docs/DEVICE_COMPATIBILITY.md) und die [Release-Beschreibung 2.0.0](docs/releases/v2.0.0.md).

## Identität und Funktionserkennung

Jedes ausgewählte Gerät hat dieselbe Webkarte, ausklappbare Details und ein eigenes HA-Gerät. Funktionen werden anhand authentifizierter Antworten oder Sensordeskriptoren erkannt. Schreibzugriffe erfordern eine aktuelle Geräteantwort. Ein LC-/Sensor-Modell allein erzeugt nicht sämtliche möglichen Entitäten. Neue Funktionen werden gespeichert und durch einen kontrollierten Neustart synchronisiert, da native ESPHome-Entitäten beim Start registriert werden müssen. Langsame oder unerreichbare Geräte können mehrere Abfragezyklen benötigen.

Die Details zeigen Modell, Hersteller, Company-/Product-ID und Mesh-Modelle sowie Firmware- und Hardware-Revisionen, sofern das Gerät diese bereitstellt. Mesh-Adressen und hexadezimale Kennungen verwenden Kleinbuchstaben. UUID und Composition VID werden in den normalen Webdetails nicht angezeigt.

Die Hauptliste enthält Steinel-Geräte. Einträge ohne Herstellerkennung erscheinen separat unter **Nicht erkannte Geräte**; Geräte mit einer anderen bekannten Herstellerkennung werden ausgeblendet. Nach dem Import sind Cloud-Anmeldung und lokaler Sicherungsimport unter **Netzwerk ändern oder erneut importieren** eingeklappt und bleiben für spätere Konfigurationsänderungen verfügbar.

Gewählte Einstellungen erscheinen sofort in der Weboberfläche und Home Assistant und werden im Hintergrund bestätigt. Tatsächlicher Ausgangszustand und Messwerte stammen aus Geräteantworten, nicht aus Berechnungen. Nach einer Steuerungsänderung hat die Ausgangsabfrage Vorrang und wird ohne Warten auf den nächsten regulären Abfragezyklus wiederholt. NightmatIQ Plus stellt Betriebsart, Dämmerungsschwelle, Beleuchtungsstärke und tatsächlichen Ausgangszustand bereit, ohne Helligkeitssteuerung oder Bewegungsnachlaufzeit.

## Grenzen und Speicher

- Ein Netzwerk, ein NetKey/AppKey-Paar, bis zu **16 importierte Geräte**, **8 Elemente je Gerät** und **16 Sensor-Publikationsgruppen**.
- Modelle mit anderem AppKey werden nicht für die Steuerung bereitgestellt.
- Eine Warteschlange mit 16 Einträgen fasst ungesendete Änderungen zusammen; ein Eintrag bleibt für Fortsetzung oder Wiederholung einer Transaktion reserviert.
- Nur eine bestätigungspflichtige Transaktion gleichzeitig; Prüfung von Adresse, Opcode und Eigenschaft. Begrenzte Schreibwiederholung mit derselben TID.
- Hintergrundabfragen werden schrittweise erzeugt, nicht als vollständige Geräteliste gepuffert.
- Ein 512-Byte-Flash-Lesepuffer vermeidet das vollständige JSON im RAM.
- Die Sicherung liegt vorübergehend in der **inaktiven OTA-Partition**. Deren bisheriges Firmware-Image wird überschrieben; die Arbeitskopie wird anschließend gelöscht.
- HTTPS und aktives Mesh verwenden getrennte Modi. Mesh vor erneutem Import deaktivieren.
- Geräteauswahländerungen benötigen einen kontrollierten Neustart. Eine leere Auswahl deaktiviert Mesh ohne Schlüsselverlust.
- Geräte und einzelne Werte laufen nach drei Minuten ab, mit einer begrenzten Verlängerung entsprechend der Anzahl bestätigter Funktionen in größeren Netzen. Große oder teilweise ausgefallene Netze können Abfragezyklen über 30 Sekunden benötigen.

Die Webdiagnose zeigt während des Betriebs den freien internen RAM und den größten Block. Der Build-Bericht enthält separat die statische RAM- und Flash-Belegung der Firmware.

Für ein weiteres Gerät in dessen Details **Gerätebericht erstellen** wählen, das Modell vom Etikett eingeben und die Aufzeichnung starten. Beobachtungen wie ein abgedeckter Lichtsensor oder ausgelöste Bewegung lassen sich markieren. **Bericht herunterladen** beendet die Aufzeichnung und speichert eine JSON-Datei für ein Issue. Die Nur-Lese-Sitzung dauert bis zu drei Minuten und funktioniert auch für importierte Geräte ohne HA-Auswahl. Schlüssel, Zugangsdaten, private Namen, UUIDs und Netzwerkadressen werden ausgelassen. Enthalten sind Composition Data, SIG-/Herstellermodelle, Rohantworten, Fehler und Kennzeichnungen für gekürzte oder fehlende Daten. Das Gateway hält einen begrenzten RAM-Puffer; der Browser sammelt den Verlauf. Die Seite daher geöffnet lassen. Auch bei Verbindungsverlust lassen sich erfasste Daten herunterladen; ein unbestätigter Stopp oder fehlgeschlagener letzter Abruf wird im Bericht markiert.

In Automationen die Entität **Available** des jeweiligen Geräts prüfen. Ausgang und Automatik nutzen eine On/Off-Auswahl, deren Zustand nach Ablauf des jeweiligen Messwerts unbekannt wird. Befehle ohne aktuelle Geräteantwort oder bei einer bestätigten Abweichung der Composition Data werden abgewiesen. Sensordiagnosen speichern bis zu acht Eigenschaften getrennt nach Element, ohne Flash-Schreibzugriffe; sie sind im Web und als standardmäßig deaktivierte Textentität **Sensor diagnostics** verfügbar.

## Sicherheit

- Das werkseitige Administratorkonto lautet `admin`, das Kennwort `12345678`. Ändern Sie es unmittelbar nach der WLAN-Einrichtung.
- Das Administratorkennwort schützt die lokale Seite und Firmware-Aktualisierungen und wird im ESP32-NVS gespeichert.
- Der Einrichtungs-Zugangspunkt verwendet das werkseitige Kennwort `12345678`.
- Einrichtung, Backup-Uploads und Updates nur im vertrauenswürdigen LAN. HTTP Digest authentifiziert, verschlüsselt HTTP aber nicht.
- Steinel-Zugangsdaten werden nur für HTTPS benutzt und nicht gespeichert.
- Netzwerk-/Geräteschlüssel und Konfiguration liegen im ESP32-NVS. Das Board enthält Netzwerkzugangsdaten.
- Die native API ist standardmäßig unverschlüsselt; eigene Builds können API-Verschlüsselung konfigurieren.
- Keine realen Backups, Passwörter, Schlüssel oder Bluetooth-Mitschnitte veröffentlichen.
- Werksreset entfernt WLAN-, Administrator- und Mesh-Einstellungen, nicht die installierte Firmware.

## Repository-Struktur

| Pfad | Zweck |
|---|---|
| `esphome/steinel-c3.yaml` | Hauptkonfiguration der ESPHome-Firmware |
| `esphome/components/steinel_mesh/` | Bluetooth Mesh, Gerätekatalog, Steinel-Cloud-Import und lokale Weboberfläche |
| `esphome/components/steinel_mesh/mesh_protocol.h` | Portable Protokoll- und Prüffunktionen |
| `esphome/components/steinel_mesh/steinel_nodes.cpp` | Geräteentitäten und Transaktionssteuerung |
| `scripts/` | Installation, Validierung, USB, OTA und Release-Vorbereitung |
| `tests/` | Protokoll-, Parser-, Transport- und Webtests |
| `home-assistant/` | Optionales Home-Assistant-Paket und kompakter Steuerdialog |
| `docs/images/` | Öffentliche README-Abbildungen |
| `docs/DEVICE_COMPATIBILITY.md` | Unterstützte Geräte und Kompatibilitätsdetails |
| `docs/releases/` | Release-Beschreibungen |
| `demo/` | Lokale Weboberflächen-Demo |

## Installation eines fertigen Firmware-Abbilds

Die empfohlene Erstinstallation erfordert keine ESPHome-Kompilierung:

1. Laden Sie die aktuelle Datei `steinel-mesh-esp32-c3-gateway-vX.Y.Z-factory.bin` von [GitHub Releases](https://github.com/supczinskib/steinel-mesh-gateway/releases/latest) herunter.
2. Öffnen Sie [ESPHome Web](https://web.esphome.io/) in einem WebSerial-fähigen Browser und verbinden Sie den ESP32-C3 über USB.
3. Wählen Sie das Board, anschließend **Install**, und öffnen Sie die heruntergeladene `-factory.bin`-Datei.
4. Fahren Sie danach mit **WLAN verbinden** und **Steinel Mesh verbinden** fort.

ESPHome Web verarbeitet die Datei lokal. `-factory.bin` ist für ein neues Board oder eine USB-Wiederherstellung bestimmt; spätere Browser-Aktualisierungen verwenden `-ota.bin`.

## Kompilieren aus dem Quellcode

Erforderlich sind Linux oder macOS, Python 3.12–3.14, USB-Zugriff für die Erstinstallation und Netzwerkzugriff auf den ESP32-C3. Internetzugriff ist für den Cloud-Import erforderlich. Home Assistant ist optional.

Der Installer erstellt eine isolierte, reproduzierbare Umgebung mit unverändertem ESPHome `2026.7.3`. Das installierte ESPHome-Paket wird nicht gepatcht.

```bash
sudo bash scripts/01_install_esphome.sh
bash scripts/03_validate_all.sh
```

Erstinstallation oder Wiederherstellung über USB:

```bash
sudo bash scripts/09_upload_usb.sh /dev/ttyACM0
```

Nach Möglichkeit sollte der stabile Pfad unter `/dev/serial/by-id/` verwendet werden. Dasselbe kompilierte Abbild funktioniert auf allen unterstützten ESP32-C3-Boards.

Der bereitgestellte Installer verwendet apt unter Debian/Ubuntu. Unter macOS eine virtuelle Python-Umgebung mit ESPHome `2026.7.3` verwenden. Die Hilfsskripte akzeptieren auch `ESPHOME` als Pfad zur ausführbaren Datei.

```sh
bash scripts/00_self_test.sh
bash scripts/11_test_mesh.sh
esphome compile esphome/steinel-c3.yaml
bash scripts/10_prepare_release.sh
```

Protokoll- und Backup-Parser-Tests nutzen AddressSanitizer/UndefinedBehaviorSanitizer mit synthetischen Daten. Getestet werden abgeschnittene Pakete/JSON, Komposition, Adressüberschneidungen, Gruppenlimits, Schlüsselbindung und Antwortzuordnung.

Bei fremder exportierter ESP-IDF-Umgebung eine saubere Shell nutzen oder `env -u IDF_PATH -u IDF_TOOLS_PATH esphome compile esphome/steinel-c3.yaml` ausführen.

Webtests: `cd tests && npm install && npm test`. jsdom wird nur für Tests benötigt und ist nicht Teil der Firmware. Geprüft werden vier Sprachen, gespeicherte Einstellungen, dynamische Gerätekarten, unveränderte Checkboxen und API-Befehlswerte.

## WLAN verbinden

1. Verbinden Sie sich mit `nightmatiq-gateway-XXXXXX` und dem Kennwort `12345678`.
2. Wählen Sie im Captive Portal das gewünschte 2,4-GHz-WLAN und geben Sie dessen Kennwort ein.
3. Warten Sie auf Neustart und Netzwerkverbindung des Gateways.
4. Öffnen Sie die vom Router zugewiesene Adresse oder den auf `.local` endenden Hostnamen.

Die WLAN-Konfiguration bleibt bei Firmware-Aktualisierungen erhalten.

## Steinel Mesh verbinden

1. Öffnen Sie die Gateway-Adresse und melden Sie sich mit `admin` / `12345678` an.
2. Ändern Sie unter **Gateway administration** im Bereich **Administrator access** das Administratorkennwort. Dasselbe Kennwort schützt spätere Updates.
3. Melden Sie sich nach dem automatischen Neustart erneut an.
4. Geben Sie Ihre Steinel-Cloud-Zugangsdaten ein und laden Sie die Netzwerkliste.
5. Wählen Sie die Installation, importieren Sie sie und warten Sie auf den Neustart. Alternativ die App-JSON unter dem lokalen Backup-Import hochladen; Mesh vorher deaktivieren.
6. Wählen Sie die Geräte für Home Assistant und speichern Sie die Auswahl mit Neustart.

Die anfängliche Geräteadresse ist optional und kann automatisch bestimmt werden. Der IV Index synchronisiert sich normalerweise mit dem Netz. Die Steinel-Zugangsdaten bleiben nur für die Einrichtungsanfragen im Browserformular.

Die Auswahl lässt sich jederzeit im selben Panel ändern. Nach Änderungen an Geräten oder Schlüsseln Mesh deaktivieren und neu importieren. Vorherige Auswahlen werden nach Geräteidentität möglichst beibehalten.

Die Oberfläche unterstützt **Englisch, Polnisch, Deutsch und Französisch**. Englisch ist voreingestellt; die Auswahl bleibt in diesem Browser gespeichert. Gerätenamen, Kontodaten und API-Befehlswerte werden nicht übersetzt.

## Firmware über WLAN aktualisieren

Beim Öffnen der Gateway-Seite wird das neueste stabile GitHub-Release geprüft; **CHECK FOR UPDATES** wiederholt die Prüfung. Ist eine neuere Version verfügbar, lädt **DOWNLOAD AND INSTALL** sie über HTTPS, prüft Größe und SHA-256-Wert, installiert sie und startet das Gateway neu. Bei einem Fehler bleibt die bisherige Firmware aktiv.

Unter **Manual firmware file** kann eine `-ota.bin`-Datei manuell installiert werden. `-factory.bin` ist ausschließlich für die Erstinstallation über USB bestimmt. Ein separates OTA-Kennwort ist nicht erforderlich; es gilt das Administratorkennwort.

Die Kommandozeilen-Aktualisierung fragt ebenfalls nach dem Administratorkennwort:

```bash
bash scripts/05_upload_ota.sh DEVICE_IP_ODER_HOSTNAME
```

Ein **OTA**-Image über die Weboberfläche installieren, kein Factory-Image. WLAN, Administratorzugang und gespeicherte Mesh-Konfiguration bleiben erhalten.

## Zurücksetzen auf Werkseinstellungen

**Factory reset** entfernt WLAN-, Administrator- und Bluetooth-Mesh-Einstellungen und stellt das werkseitige Konto sowie den Einrichtungs-Zugangspunkt wieder her, ohne die installierte Firmwareversion zu ändern.

## Home Assistant

Separate Geräte benötigen **Home Assistant 2025.7 oder neuer**, siehe [ESPHome-Untergeräte](https://www.home-assistant.io/blog/2025/07/02/release-20257/#noteworthy-improvements-to-existing-integrations).

Home Assistant erkennt das Gateway normalerweise automatisch über ESPHome. Andernfalls:

1. Öffnen Sie **Settings → Devices & services**.
2. Fügen Sie die Integration **ESPHome** hinzu.
3. Geben Sie IP-Adresse oder Hostname des Gateways ein.
4. Weisen Sie die ausgewählten Steinel-Geräte den gewünschten Bereichen zu.

Jeder ausgewählte Mesh-Knoten erscheint als eigenes Gerät mit Steuerung und Diagnose. Die Gateway-Diagnose enthält seine aktuelle IP-Adresse.

Beim Deaktivieren von Mesh oder im Firmware-Update-Modus bleiben ausgewählte Geräte und ihre Entitäten in Home Assistant registriert, einschließlich ihrer Bereichszuordnung. Ohne Kommunikation werden Messwerte nicht verfügbar; die Geräte werden nicht entfernt.

HA kann Einträge abgewählter Geräte behalten; unbenutzte Einträge gegebenenfalls dort entfernen.

## Optionaler Home-Assistant-Steuerdialog

Die Dateien unter `home-assistant/` ergänzen optional den abgebildeten kompakten Bereichskachel- und Steuerdialog:

1. Kopieren Sie `steinel-nightmatiq-package.yaml` in das Home-Assistant-Paketverzeichnis.
2. Kopieren Sie `steinel-nightmatiq-popup.js` nach `/config/www/`.
3. Fügen Sie `/local/steinel-nightmatiq-popup.js?v=200` als JavaScript-Modul zu den Dashboard-Ressourcen hinzu.
4. Laden Sie die Paketkonfiguration neu und aktualisieren Sie den Browser-Cache.

Das Modul erkennt sein Ziel automatisch, wenn genau ein passendes Gerät existiert. Bei mehreren Kandidaten `window.steinelNightmatiqEntities` im JavaScript und die Zuordnung `target_entities` in der Paket-YAML auf die gewünschten Entitäts-IDs setzen; auch Suffixe wie `_2` prüfen. Der optionale Dialog ist für NightmatIQ Plus gedacht; andere Geräte nutzen die Standardsteuerung von Home Assistant. Das optionale Modul greift in die Home-Assistant-Bereichsstrategie ein und kann nach einer zukünftigen Frontend-Aktualisierung eine Anpassung benötigen.

Die Bereichsansicht zeigt eine Sensorzustandskachel, die den Steuerdialog öffnet. Zusätzliche Kacheln desselben Sensors werden in dieser Ansicht ausgeblendet, ohne Entitäten oder Automationen zu deaktivieren. Die automatische Zielerkennung prüft die Gateway-Identität statt nur ähnliche Entitätsnamen anderer Integrationen.

## Mehrere Gateways

Beim Netzwerkimport leitet jedes Gateway seine Mesh-Adressrichtlinie aus der gewählten Installation und seiner Hardwareidentität ab. Durch den standardmäßigen MAC-Suffix erhalten mehrere Gateways eindeutige Host- und Zugangspunktnamen. Verwenden Sie für jedes Gateway ein eigenes Administratorkennwort.

## Fallback-Zugangspunkt

Ist das konfigurierte WLAN 60 Sekunden lang nicht verfügbar, startet das Gateway seinen kennwortgeschützten Zugangspunkt erneut. Verbinden Sie sich mit dem werkseitigen AP-Kennwort `12345678` und ändern Sie die WLAN-Konfiguration im Captive Portal. Die lokale Seite bleibt durch das auf dem Gerät festgelegte Administratorkennwort geschützt.

## Fehlerbehebung

### Keine Anzeige in Home Assistant

- Prüfen Sie die Erreichbarkeit des Gateways aus dem Home-Assistant-Netz.
- Fügen Sie ESPHome bei gefilterter Erkennung zwischen VLANs manuell über die IP-Adresse hinzu.
- Prüfen Sie den Online-Status des Gateways und starten Sie bei Bedarf die ESPHome-Integration neu.

### Mesh ist bereit, Werte fehlen jedoch

- Verringern Sie den Abstand zwischen ESP32-C3 und den ausgewählten Steinel-Geräten und prüfen Sie **Last Mesh RSSI**.
- Warten Sie nach dem Import auf die Synchronisierung des IV Index.
- Fordern Sie den aktuellen Zustand mit **Refresh devices** an.

### Steinel-Netzwerk kann nicht geladen werden

- Prüfen Sie, ob das Konto in der offiziellen Steinel-Anwendung Zugriff auf die Installation hat.
- Prüfen Sie Internetzugang, DNS und Systemzeit im Gateway-Netz.
- Warten Sie nach einem fehlgeschlagenen Einrichtungsversuch auf den Neustart und versuchen Sie es erneut.

### OTA-Aktualisierung schlägt fehl

- Prüfen Sie Zieladresse und Administratorkennwort.
- Verwenden Sie die Browser-Aktualisierung in einem vertrauenswürdigen LAN.
- Stellen Sie das Gerät über USB wieder her, wenn es keine WLAN-Verbindung mehr aufbaut.

## Verwandtes Projekt

Dieselbe Steinel-NightmatIQ-Plus-Funktion ist auch als optionale Integration im Projekt [AR01V3 RF/IR, ESP-RC01 & Steinel NightmatIQ Plus Gateway](https://github.com/supczinskib/athom-ar01v3-esp-rc01-gateway) verfügbar. Dieses Repository ist für eine kleine, dedizierte ESP32-C3-Installation vorgesehen.

## Lizenz

Copyright (C) 2026 Bartosz Supcziński.

Dieses Projekt steht ausschließlich unter der GNU General Public License Version 3 (`GPL-3.0-only`). Siehe [LICENSE](LICENSE).

## Support

- Autor und Betreuer: **Bartosz Supcziński**, <bartek@env.pl>.
- ESPHome-Projektkennung: `envpl.steinel_mesh_gateway`.

Geben Sie bei einer Fehlermeldung Firmwareversion, ESPHome-Version, Neustartursache und relevante Protokolle an. Entfernen Sie zuvor Kennwörter, Schlüssel, Autorisierungs-Header, private Sicherungen und Netzwerkkennungen.

Dies ist ein unabhängiges Community-Projekt und kein offizielles Produkt von Steinel, ESPHome oder Home Assistant.
