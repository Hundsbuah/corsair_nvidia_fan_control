# Code-Review Findings — corsair_fan_control

**Zweck dieser Datei:** Maschinell verwertbare Anweisungen, um die im Code-Review
gefundenen Probleme zu beheben. Jede KI/ein Entwickler soll die Findings **in der
angegebenen Reihenfolge** (Prio 1 = M-001, M-002, M-003, L-001, dann INFO) einzeln
fixen. Pro Fix: 1) Änderung umsetzen, 2) Verifikation durchführen, 3) hier die
`STATUS:`-Zeile auf `FIXED` setzen.

**Metadaten**
- Projekt: `G:/GIT/corsair_fan_control`
- Geprüft bei Commit: `738b267` (main)
- Sprache/Plattform: C11, Win32 (Windows 10+), UNICODE
- Review-Methodik: vollstatische Analyse (kein Lauf, keine Hardware)
- **Wichtig:** Zeilennummern beziehen sich auf den Stand bei Commit `738b267`.
  Vor jedem Fix die Zeilen neu auflösen (Funktionen per Name suchen, nicht blind
  per Zeilennummer).

**Allgemeine Regeln für Fixes**
1. Kein Refactoring jenseits des konkreten Findings.
2. Keine neue Dependencies, keine Build-Änderungen (außer explizit im Finding verlangt).
3. Bestehenden Code-Stil beibehalten (C11, Win32-Unicode-APIs, `swprintf` mit
   Count-Argument — UCRT/MSVC-C99-Semantik, das ist korrekt so).
4. Nach jedem Fix: `graphify update .` (lt. AGENTS.md).
5. Keine Findings aus der Sektion „Unverified Concerns“ fixen, ohne die dort
   genannte Verifikation vorher durchgeführt zu haben.

---

## Prio 1 — M-001: Fake-Device-Testpfad (CFC_FAKE) ist vollständig tot

- **Severity:** Medium | **Confidence:** CONFIRMED | **Kategorie:** Correctness
- **STATUS:** FIXED
- **Fix (Commit-Stand nach 738b267):** In `open_controller` wird vor dem Fake-Check `controller->device.info = app->devices[device_index];` gesetzt (Kopie-Lösung, empfohlen). Damit sieht `is_fake_device(&controller->device)` den gescannten Fake-Pfad; für echte Geräte überschreibt `corsair_open` `dev->info` erneut (harmlos). Alle 5 `is_fake_device`-Zweige sind damit wieder erreichbar. Keine weitere Änderung.
- **Verifikation:** `temp/verify_m001.py` + `temp/verify_m001_apply.py` (Fake via `CFC_FAKE` + Test-Build mit leerem `corsair_find_devices`), Registry über `temp/reg_guard.py` gesichert/wiederhergestellt. Ergebnis: Device „Commander Pro (Test) (PID 0C10)“ wird geöffnet, „1 of 1 controllers initialized and updated“, Firmware 1.2.3/State Online, RPM>0 mit Jitter pro 5-s-Tick, Slider+Apply ändert simulierte RPM (~2480), Regression ohne `CFC_FAKE` unverändert (echtes Gerät wird weiter korrekt gelistet/geöffnet).
- **Betroffene Dateien:** `src/main.c` (primär)

### Problem
Mit gesetzter Env-Var `CFC_FAKE` wird ein Fake-Gerät `fake://commander-pro-test`
in die Geräte-Liste injiziert (main.c ~Zeile 1298–1310 in `scan_devices`), kann
aber **nie** geöffnet werden. `open_controller` (main.c ~1256) prüft
`is_fake_device(&controller->device)` — aber `controller->device.info.path` ist
dort immer leer:

- `scan_devices` schreibt den Fake-Pfad nur in `app->devices[0].path`
  (main.c ~1306: `L"fake://commander-pro-test"`).
- `controller->device` wird durch `ZeroMemory(app->controllers, ...)` geleert.
- `corsair_open` (corsair_hid.c:402) setzt `dev->info = *info` erst am SEHR
  ENDE — erreicht nur wenn `CreateFileW` succeeds.
- `CreateFileW(L"fake://commander-pro-test", ...)` schlägt jedoch immer fehl
  (Fehler 2, Datei nicht gefunden) → `corsair_open` liefert false →
  `info` bleibt leer → `is_fake_device(&controller->device)` ist für immer false.

Folge: Alle `is_fake_device(...)`-Zweige sind toter Code:
- main.c:1163 (`close_controller`)
- main.c:1256–1257 (`open_controller` — `fake_fill_base` unerreichbar)
- main.c:1454 (`refresh_status` — `fake_refresh` unerreichbar)
- main.c:1510 (`apply_one`)
- main.c:1629 (`apply_nvidia_curve_to_controller`)

Symptom: Die App zeigt stattdessen „Could not open HID device: 2. Close iCUE…“
(misleading), und der einzige Offline-Testpfad ohne Hardware funktioniert nicht.
Einziger funktionsfähiger Fake-Teil: Fake-GPU-Temperatur in `refresh_gpu_status`
(main.c ~1027, prüft `fake_device_enabled()` direkt — das ist korrekt und bleibt).

### Root Cause
Adressat-Fehler: Der Fake-Check liest aus dem leeren `CorsairDevice`-Zustand des
Controllers statt aus dem gescannten `app->devices[device_index]`. Es gibt
keinen Punkt, der den Fake-Pfad in `controller->device.info` kopiert.

### Fix-Anleitung
1. Alle 5 Aufrufstellen von `is_fake_device(&controller->device)` in main.c
   ersetzen durch einen Check gegen das gescannte Geräte-Info-Objekt:
   `is_fake_device(&app->devices[device_index])`.
   Alternativ (gleicher Effekt): bei `open_controller` vor dem
   `corsair_open`-Aufruf den Info-Block kopieren
   (`controller->device.info = app->devices[device_index]`) — dann funktionieren
   die Existierenden Checks unverändert. Die Kopie-Lösung ist weniger
   invasive. **Empfohlen: Kopie-Lösung.**
2. In `open_controller` (main.c ~1242): vor `if (is_fake_device(...))` den
   Controller-Info aus `app->devices[device_index]` setzen, z. B.
   `controller->device.info = app->devices[device_index];` — aber nur im
   Fake-Fall nötig ist, ob das für echte Geräte stört, prüfen: `corsair_open`
   überschreibt `dev->info` sowieso (corsair_hid.c:402), daher ist die frühe
   Kopie für echte Geräte harmlos.
3. `close_controller` (main.c ~1159): `is_fake_device(&controller->device)`
   funktioniert nach Schritt 1/2 korrekt (info ist dann gefüllt).
4. Optional (konsistenz): `fake_fill_base` sollte `g_fake_duty[i]` pro
   Öffnen neu auf 50 setzen — tut es bereits.

### Verifikation
```bat
set CFC_FAKE=1
build-clang\corsair_nvidia_fan_control.exe
```
- Nach Start: Geräte-Combo zeigt „Commander Pro (Test) (PID 0C10)“.
- Status-Label: „1 of 1 controllers initialized and updated.“ (KEIN
  „Could not open HID device: 2…“).
- Controller-Panel zeigt Firmware 1.2.3, State „Online“, Fan-RPM > 0.
- Alle 5 s (REFRESH_TIMER) ändern sich RPM/Temperatur-Werte leicht (Jitter).
- Slider + „Apply all“ ändert den simulierten RPM-Wert.
- Ohne `CFC_FAKE`: Verhalten unverändert (echte Geräte-Pfad).

---

## Prio 2 — M-002: Overview-Tabellen leer nach 2× Öffnen/Schließen

- **Severity:** Medium | **Confidence:** CONFIRMED | **Kategorie:** Correctness / Lifetime
- **STATUS:** FIXED
- **Fix (Commit-Stand nach 738b267):** `ui_panel_attach_list` (src/ui.c) recycelt jetzt Slots, deren Panel-HWND tot ist (`!IsWindow(g_panel_lists[i].panel)`), analog zu `ui_dark_listview`. `g_panel_list_count` wird nur bei Neuanlage inkrementiert, damit `ui_panel_list` (Suche über `i < g_panel_list_count`) korrekt bleibt. `ui_panel_list` selbst unverändert.
- **Verifikation:** `temp/verify_m002.py` + `temp/ccount.c/.exe` (C-Helfer für `LVM_GETITEMCOUNT` — der lokale Python-3.14-ctypes liefert cross-prozess 0 für `LVM_GETITEMCOUNT` zurück) + Test-Build `build-m002test` (temporärer Patch: `corsair_find_devices` liefert 0, damit `CFC_FAKE` auf Maschine mit echtem Gerät injiziert; Patch NICHT committen). Ergebnis: 3 vollständige Overview-Zyklen (WM_CLOSE → `overview_wnd_proc` zerstört das eigene Fenster; **cross-process `DestroyWindow` schlägt fehl** — der Test hätte sonst Scheinzyklen erzeugt, weil `show_overview_window` das offene Fenster wiederverwendet und die Tabelle nie voll wurde), jeweils 6 FANS-/1 SENSOR-Zeile (Item-Count) und Pixelprobe der Zeilenfläche (Header ausgeschlossen): dark ≥ 0.90 + Text ≥ 0.3 % → Custom-Draw aktiv. **Negativ-Test (Fix gestashed):** Zyklus 3 bricht sichtbar ein (FANS dark=0.83/bright=0.17, SENSORS partiell) — Test ist bug-sensitiv. Regression: Zyklus 1–2 unverändert; M-001-Fake-Pfad im selben Lauf grün.
- **Betroffene Dateien:** `src/ui.c` (primär)

### Problem
`ui_panel_attach_list` (ui.c:1740) hängt Panel↔ListView-Mappings an ein
statisches Array `g_panel_lists[UI_LIST_MAX]` (ui.c:15, `UI_LIST_MAX 4`,
ui.c:1727) mit monoton wachsendem `g_panel_list_count` (ui.c:1742, 1745–1747).
Es gibt **kein Freeing und kein Recycling** bei `WM_DESTROY`.

Jede Overview-Instanz ruft `ui_panel_attach_list` zweimal auf
(main.c:1977 FANS-List, main.c:2004 SENSORS-List) → belegt 2 Slots.
Zwei Overview-Zyklen (Öffnen+Schließen × 2) erschöpfen die 4 Slots.
Ab der 3. Overview-Instanz: `ui_panel_attach_list` liefert still (Return ohne
Eintrag), `ui_panel_list` (ui.c:1730) findet den neuen List-HWND nicht mehr,
`panel_proc` (ui.c, `case WM_NOTIFY` / `case WM_CTLCOLORLISTVIEW`) behandelt
NM_CUSTOMDRAW nicht mehr → `CDRF_DODEFAULT` → die Zeilen existieren, aber
keine Zelltexte werden gerendert (die Daten stammen ausschließlich aus dem
`text_fn`-Callback, nicht aus dem ListView-Item-Text).

Symptom: Header-Zeile (eigener `header_proc`, recycelt korrekt per `IsWindow`
in `g_lists`, ui.c:1951–1958) bleibt sichtbar/dunkel; FANS- und SENSORS-Datenzeilen
sind leer. Funktion „Live-Overview aller Controller“ ist danach bis zum
App-Neustart unbrauchbar. Da die App als Tray-/Autostart-App über Tage läuft
(README: `--tray` Autostart), ist der Trigger realistisch.

Verwandte (langsamere) Schwäche desselben Musters: `g_registry` (ui.c:22,
`UI_REGISTRY_MAX 256`) — Control-Einträge werden nie gelöscht
(`ui_reg_find`/`ui_reg_new` ui.c:256–270, kein Cleanup in `WM_DESTROY` der
Controls). Nach ~20+ Overview-Zyklen (jeweils ~10 Controls) erhalten neue
Controls kein Theme-Font/CTLCOLOR mehr (nur visuell).

### Root Cause
Per-Window-Zustand in statischen Arrays ohne Entsorgung/Recycling; Kapazität
(4 bzw. 256) für eine Langzeit-Tray-App zu klein.

### Fix-Anleitung
**Option A (empfohlen, minimal):** `ui_panel_attach_list` wie `ui_dark_listview`
recyceln: vor dem Anlegen prüfen, ob ein bestehender Slot zu einem **toten**
Panel-HWND gehört (`!IsWindow(panel)`) und diesen Slot wiederverwenden.
Pseudocode:
```c
void ui_panel_attach_list(HWND panel, HWND list)
{
    for (int i = 0; i < UI_LIST_MAX; ++i)
        if (!g_panel_lists[i].panel || !IsWindow(g_panel_lists[i].panel)) {
            g_panel_lists[i].panel = panel;
            g_panel_lists[i].list = list;
            return;
        }
    /* voll: bestehende Logik/erweitern */
}
```
Wichtig: Auch `ui_panel_list` (Suche) weiter über alle `UI_LIST_MAX` Einträge
laufen lassen (macht es bereits).

**Option B (gründlicher):** Genaues Recycling + zusätzlich `g_registry`
bei `WM_DESTROY` der Custom-Controls bereinigen (bzw. bei `ui_reg_find` tote
`IsWindow`-Einträge recyceln). Option B ist mehr Arbeit; für M-002 genügt A.

**Kein Fix an:** `g_lists`/`header_proc` (recyceln bereits korrekt).

### Verifikation
1. App starten (mit `CFC_FAKE=1` für ohne-Hardware-Test nach M-001-Fix).
2. „Overview“ → Fenster schließen (X). × **3** Wiederholen.
3. Beim 3. Öffnen müssen FANS- und SENSORS-Tabellen vollständige Zeilendaten
   zeigen (Controller-Namen, RPM, Firmware, Temperaturen).
4. Optional: `ListView_GetItemCount` > 0 und Custom-Draw-Zeilen sichtbar
   (Pixelprobe wie in `temp/verify.py`, automatisierbar).
5. Regression: 1. und 2. Öffnen funktionieren unverändert.

---

## Prio 3 — M-003: Synchrone HID-/NVAPI-I/O auf UI-Thread → mehrsekündige Freezes

- **Severity:** Medium | **Confidence:** CONFIRMED (Codepfad; Dauer hardwareabhängig)
- **Kategorie:** Reliability / Performance
- **STATUS:** FIXED
- **Fix (Commit-Stand nach 738b267):** Stufe 1 vollständig implementiert. (1) `refresh_status`: `corsair_refresh` läuft nur alle `REFRESH_EVERY_TICKS` (=2) Polls; fehlschlagende Controller (`ControllerRuntime.refresh_failed`) werden übersprungen, bis alle `REFRESH_FAIL_RETRY_EVERY` (=4) Runden erneut versucht wird → kein Close/Reopen/Initialize-Zyklus pro 5-s-Tick bei defektem Gerät. Fake-Device (CFC_FAKE) behält vollen 5-s-Takt (kein HID-I/O). (2) `send_command` nimmt pro-Aufruf-Mutex-Timeout an; Read-only-Transaktionen in `corsair_refresh` nutzen `MUTEX_READ_TIMEOUT_MS` (=500 ms), Writes/Initialize bleiben bei `MUTEX_TIMEOUT_MS` (2000 ms). Stufe 2 (Worker-Thread) nicht erforderlich.
- **Verifikation:** `temp/verify_m003_dead.py` + Test-Build `build-m003dead` (temporärer Patch: injiziertes „echtes“ CFC_TEST_SLOW-Device, dessen jede Transaktion 800 ms stottert und fehlschlägt; Patch NICHT committen). Ergebnis: Backoff exakt (I/O nur auf Tick 2, 4, 8; Tick-dt 796–812 ms, **kein Tick ≥ 1000 ms**), Fehlermeldung sichtbar im Status-Label („Simulated hung device: I/O timeout.“), App bei allen 6/6 Zehntel-Samples alive mit lesbarem Status. Regression: `temp/verify_m003_healthy.py` am echten Gerät (Clean-Build `build-clang`): RPM-Werte ändern sich im 2-Tick-Zyklus (2 Changes in 30 s), App alive.
- **Betroffene Dateien:** `src/main.c` (Timer/Poll), `src/corsair_hid.c` (Timeouts)

### Problem
`REFRESH_TIMER` (main.c:41, 5000 ms) → `refresh_status` (main.c:1423) läuft
im **UI-Thread** (einziger Thread der App) und blockiert dort:

- Jede `send_command`-Transaktion (corsair_hid.c:286):
  `WaitForSingleObject(dev->io_mutex, 2000 ms)` (corsair_hid.c:308)
  + `write_report` ≤ 300 ms Timeout (corsair_hid.c:210)
  + `read_report` ≤ 300 ms Timeout
  → Worst Case ~2,6 s **pro Transaktion**.
- `corsair_refresh` (corsair_hid.c:463): bis zu 13 Transaktionen/Controller
  (4 Temp + 6 RPM + 3 Volt).
- `apply_nvidia_curve_to_controller` (main.c:1597): bis zu 6 weitere.
- `corsair_initialize`: ≥ 4 Transaktionen.
- Bei Scan/Refresh eines hängenden Geräts: serielle Schleife über alle
  Controller → potenziell >10 s komplette GUI-Sperrung („Not Responding“).

Zusätzlich: `nvidia_temp_read` (nvidia_temp.c:209) lädt pro Tick die DLL
(LoadLibraryExW/Initialize/Unload/FreeLibrary) — s. auch I-001.

### Root Cause
Asynchrones (Overlapped) I/O-Setup wird strikt synchron im UI-Thread
abgehandelt; konservative Timeouts (300 ms / 2000 ms) skalieren linear mit
Transaktionen × Controllern; kein Backoff bei wiederholten Fehlern
(hängendes Gerät → pro Tick Close + Reopen + volle Initialize).

### Fix-Anleitung (konzeptionell, aufwändiger Fix — in Stufen)
**Stufe 1 (minimal, empfohlen zuerst):**
- In `refresh_status` (main.c:1423): pro Controller `corsair_refresh` nur noch
  alle **zweiten** Polls ausführen (z. B. `app->poll_counter` toggeln, oder
  `last_poll_time`-Differenz nutzen) — halbiert die Worst-Case-Dauer.
- Backoff: Controller, dessen letztes `corsair_refresh` fehlgeschlagen ist,
  im nächsten Tick **überspringen** (erst jede 4.–8. Runde erneut versuchen).
  Damit kein Close/Reopen-Zyklus pro 5-s-Tick bei defektem Gerät.
- Mutex-Wait: Für READ-Only-Transaktionen (`corsair_refresh`) `MUTEX_TIMEOUT_MS`
  kürzer lassen (z. B. 500 ms), da Lesebefehle mit iCUE-Konflikt schnell
  scheitern dürfen; Schreibbefehle (Apply) bleiben bei 2000 ms.

**Stufe 2 (fundamental, wenn Stufe 1 nicht ausreicht):**
- HID-Poll in eine Worker-Thread verlagern (einzige schreibende Komponente
  bleibt UI; Daten per volatile-struct + `PostMessage` an UI-Thread
  zurückmelden) — größere Architektur-Änderung; erst nach Abwägung.

**Nicht tun:** Timeout-Werte global senken (geräteweise Reaktionszeiten!),
Poll-Intervall < 5 s (README dokumentiert 5 s; iCUE-Konflikt).

### Verifikation
1. `CFC_FAKE=1` (nach M-001-Fix) als Basis.
2. Simulation defektes Gerät: USB-Isolierung an echtem Gerät während Polls
   ODER Fake-Device temporär um eine künstliche Latenz/Timeout erweitern
   (nur für Test, nicht committen).
3. Messbar: UI-Thread (z. B. über `WM_TIMER`-Deltalog im Debug-Build) zeigt
   während eines defekten Polls keine Blöcke > 1 s; GUI bleibt malbar/
   bedienbar; kein „Not Responding“-Flag.
4. Regression: Bei gesundem Gerät ändern sich alle 5 s die Werte wie bisher.

---

## Prio 4 — L-001: Deaktivierte Controls sehen aus wie aktiv und reagieren nicht

- **Severity:** Low | **Confidence:** CONFIRMED | **Kategorie:** Correctness/UX
- **STATUS:** FIXED
- **Fix (Commit-Stand nach 738b267):** `button_proc`/`slider_proc`/`combo_proc` (src/ui.c) leiten den „enabled“-Zustand jetzt von `WS_DISABLED` ab statt vom immer-wahren `WS_VISIBLE`-Check: `BOOL enabled = (GetWindowLongW(hwnd, GWL_STYLE) & WS_DISABLED) == 0;`. Abweichung vom Fix-Vorschlag: der vorgeschlagene `WS_ENABLED`-Check funktioniert NICHT, weil das Bit 0x00010000 bei Tabstop-Controls auch im deaktivierten Zustand gesetzt bleibt; `WS_DISABLED` (0x08000000) ist die korrekte Negation. `WS_ENABLED`/`WS_DISABLED` als `#ifndef`-gegardete `#define`s ergänzt (werden von WIN32_LEAN_AND_MEAN ausgeschlossen). `WM_ENABLE` → `InvalidateRect` existierte bereits.
- **Verifikation:** `temp/verify_l001.py` + Test-Build `build-m003test` (temporärer Patch: `corsair_find_devices` liefert 0 Geräte; NICHT committen). State A (kein Gerät, Controls deaktiviert): bright_text=0.0000 auf Refresh, Apply-all, Apply-Fan1, Mode-Combo; Slider-Fill unterhalb der Accent-Schwelle. State B (`CFC_FAKE=1`, Controller aktiv): Labels hell (0.033–0.060), Slider-Accent-Fill sichtbar → Deaktiviert-Zustand wird sichtbar gerendert.
- **Betroffene Dateien:** `src/main.c`, `src/ui.c`

### Problem
`update_controls_enabled` (main.c:969–981) deaktiviert Controls via
`EnableWindow` → toggelt `WS_ENABLED`. Aber die Paint-Handler der Custom
Controls berechnen „enabled“ als `WS_VISIBLE`-Check:

- ui.c:439 (`button_proc`)
- ui.c:633 (`slider_proc`)
- ui.c:858 (`combo_proc`)
  ```c
  BOOL enabled = (GetWindowLongW(hwnd, GWL_STYLE) & WS_VISIBLE) != 0;  // immer TRUE
  ```
Die Controls werden nie `ShowWindow(SW_HIDE)`-versteckt, daher bleiben sie
optisch voll aktiv. Gleichzeitig verschluckt das OS Maus-/Tastatureingaben an
deaktivierte Fenster → keine `WM_COMMAND` → die Status-Guards in main.c
(„Select an initialized device first.“) werden nie erreicht → **null** Feedback.

Trigger: Jeder Zustand ohne geöffneten Controller (Start ohne Geräte,
iCUE-Konflikt, Open-Fehler) + Klick auf Refresh/Apply/Apply-all/Fan-Controls.

### Root Cause
Zwei widersprüchliche Deaktivierungskonzepte: `WS_ENABLED` (main.c) vs.
`WS_VISIBLE`-basiertes „enabled“ (ui.c).

### Fix-Anleitung
1. In allen drei ui.c-Stellen (439, 633, 858) `WS_VISIBLE` durch `WS_ENABLED`
   ersetzen:
   ```c
   BOOL enabled = (GetWindowLongW(hwnd, GWL_STYLE) & WS_ENABLED) != 0;
   ```
   (Die Paint-Handler haben bereits `WM_ENABLE` → `InvalidateRect`,
   ui.c:441/636/860 — RePaint nach Deaktivierung funktioniert damit.)
2. OPTIONAL (besseres Feedback): `WM_LBUTTONDOWN` auf deaktivierte Controls
   ignorieren weiterhin, aber per `WM_PARENTNOTIFY`/Statusmeldung in main.c
   melden — oder (einfacher) die Deaktivierung in `update_controls_enabled`
   durch `ShowWindow(SW_HIDE)`-free Ansatz ersetzen. Nicht zwingend für den
   Core-Fix.

### Verifikation
1. App starten OHNE `CFC_FAKE` und OHNE angeschlossenes Gerät (oder iCUE läuft).
2. Refresh/Apply-all/Apply-Buttons + Fan-Controls müssen sich sichtbar
   deaktiviert rendern (dimmed Fill/Text, laut Theme `t->dim`/`t->panel_brush`).
3. Nach erfolgreichem Geräte-Open (oder `CFC_FAKE=1`): Controls werden aktiv.
4. Kein visuelles Verhalten bei aktivem Controller (Regression).

---

## Prio 5 — INFO-Findings (optional, keine Fehlfunktionen)

### I-001: NVAPI alle 5 s load/init/unload
- **Severity:** Info | **Confidence:** CONFIRMED | **STATUS:** FIXED
- **Fix (Commit-Stand nach 738b267):** Statischer Session-Cache `g_nvapi` in `nvidia_temp.c`: `nvapi_ensure()` lädt `nvapi64.dll` und ruft `NvAPI_Initialize` nur einmal pro Prozess auf; `NvAPI_Unload`/`FreeLibrary` (`nvapi_release()`) nur bei fehlgeschlagenen Reads (stale Driver-State → nächster Read lädt neu) bzw. am Prozessende (OS räumt auf). `nvidia_temp_read` nutzt den Cache; Temperatur/Name/Fehlermeldungen unverändert.
- **Verifikation:** `temp/verify_i001.py` am echten Gerät (Clean-Build `build-verify`, kein `CFC_FAKE`): GPU-Temperatur live und pro 5-s-Tick wechselnd (49 → 40 → 39 → 38 °C) und `nvapi64.dll` in 23/23 Probesamples nach dem ersten Read im Prozess geladen (Vor-Fix: `FreeLibrary` pro Read → Modul zwischen Ticks nicht geladen; `tasklist /m` als Modul-Quelle, da `EnumProcessModulesEx` in der lokalen Python-3.14-Umgebung unzuverlässig war).
- **Betroffene Dateien:** `src/nvidia_temp.c`
- **Betroffen:** `src/nvidia_temp.c:145 (nvapi_load), 190 (nvapi_unload), 209 (nvidia_temp_read)`
- **Problem:** Pro `nvidia_temp_read`-Aufruf (alle 5 s + bei Scan/Apply):
  `LoadLibraryExW` → `NvAPI_Initialize` → `NvAPI_Unload` → `FreeLibrary`.
  Funktionsneutral (offizielles NVIDIA-Sample lädt dynamisch), aber unnötiger
  Overhead pro Tick.
- **Fix-Richtung:** Session-langes Load/Initialize mit statischem `NvApi`-Cache
  (einmalig laden, `FreeLibrary` erst am Prozess-Ende bzw. bei API-Fehler
  neu laden). Alternativ: LoadLibrary-Cache mit Handle-Prüfung.
- **Verifikation:** Profil mit `GetTickCount64`-Delta um `nvidia_temp_read`
  vor/nach Fix; Verhalten (Temperatur, Name, Fehlermeldung) unverändert.

### I-002: Native Checkbox (Autostart) im hellen System-Theme
- **Severity:** Info | **STATUS:** FIXED
- **Fix (Commit-Stand nach 738b267):** `wnd_proc` und `overview_wnd_proc`: `WM_CTLCOLORBTN` in die Fall-Durchgruppierung der `WM_CTLCOLORDLG`/`WM_CTLCOLOREDIT`/`WM_CTLCOLORSTATIC`-Cases aufgenommen → `ui_handle_ctrl_color`.
- **Verifikation:** `temp/verify_i002.py`: Message-Level — `WM_CTLCOLORBTN` wird mit echtem HBRUSH-Handle (>0xFFFF) beantwortet (Themed-Routing statt DefWindowProc-Color-Index); visuell — Checkbox mean_lum 33.3 (dunkel wie umliegende Controls), Label-Text sichtbar (themed Text-Color). **Umwelt-Hinweis:** Die Maschine läuft mit Windows-11-Dunkel-App-Theme (`AppsUseLightTheme=0`) → comctl32 rendert auch den Pre-Fix-Stand dunkel (sauberer Baseline-Build aus Commit 738b267: identisches Aussehen) → der gemeldete Defekt (helle Checkbox) manifestiert sich auf diesem System nicht und ist hier pixel-technisch nicht A/B-unterscheidbar; der Fix sichert den themed Brush auf Systemen mit hellem Theme. Cross-Process-`BM_SETCHECK`/`BM_GETCHECK`-Proben sind in dieser Umgebung unzuverlässig (mit minimaler Standalone-Button-App reproduziert, `temp/mini_probe*`) → keine Check-State-Manipulation im Test; In-App-Klickverhalten separat verifiziert (Registry + Status-Label toggeln korrekt, `temp/click_retry.py`).
- **Betroffen:** `src/main.c:2302 (autostart_checkbox, WC_BUTTONW)`, wnd_proc 2523
- **Problem:** `WM_CTLCOLORBTN` wird nicht an `ui_handle_ctrl_color`
  weitergeleitet (main.c ~2709 behandelt nur `WM_CTLCOLORDLG/EDIT/STATIC`)
  → „Start with Windows“ rendert hell auf dunklem Panel.
- **Fix-Richtung:** `WM_CTLCOLORBTN` in die Fall-Durchgruppierung der
  `WM_CTLCOLOREDIT`/`WM_CTLCOLORSTATIC`-Cases in `wnd_proc` (und optional
  `overview_wnd_proc`) aufnehmen.
- **Verifikation:** Visuell: Checkbox dunkel wie umliegende Controls.

### I-003: Overview = owned Window + Tray-Foreground auf Hidden Window
- **Severity:** Info | **STATUS:** PARTIAL — Problem 2 FIXED; Problem 1 = Produktentscheidung, bewusst offen
- **Fix (Commit-Stand nach 738b267, Problem 2):** `show_tray_menu` aktiviert vor `SetForegroundWindow(app->hwnd)` das bisherige Foreground-Fenster (`prev_foreground = GetForegroundWindow()` + `SetForegroundWindow(prev_foreground)`), damit der Prozess das Foreground-Recht hält und `TrackPopupMenu` Mouse-Input trackt (Standard-Workaround; Menü schließt auf Click-away).
  **Problem 1 (Owned Window vs. Top-Level) bewusst NICHT geändert:** Parent-Änderung hat Z-/Nebenwirkungen und ist eine Produktentscheidung (README nennt „resizable window“ → Top-Level/Parent `NULL` wäre plausibel; aktuell: kein Taskbar-/Alt-Tab-Eintrag, Overview immer über Hauptfenster). Vor einer Änderung: bewusste Entscheidung + `WM_DESTROY`-Cleanup in `wnd_proc` bleibt korrekt (destroys Overview manuell).
- **Verifikation:** `temp/verify_i003.py` (Clean-Build `build-verify`): App-Fenster per SendInput-Input-Recht + `SetForegroundWindow` in den Vordergrund gesetzt, Tray-Right-Click per `WM_TRAYICON`/`WM_RBUTTONUP` (was der `Shell_NotifyIcon`-`NIN_POPUPMENU`-Callback liefert) simuliert → Popup-Menü (`#32768`) erscheint; Klick daneben schließt das Menü und der UI-Thread blockiert nicht (TrackPopupMenu kehrt zurück). **Umwelt-Hinweis:** Die externe Simulation des Hidden-Window-Falls scheitert an der OS-Regel „nur der letzte Input-Prozess darf ein Fenster eines anderen Prozesses in den Vordergrund setzen“ (Hidden `app->hwnd` ist von außen nicht aktivierbar); der Hidden-Window-Pfad nutzt dasselbe Juggling-Code und ist im Source geprüft.
- **Betroffen:** `src/main.c:2036 (CreateWindowExW …, app->hwnd, …)`,
  `show_tray_menu` (main.c ~855)
- **Problem (1):** Overview-Fenster hat `app->hwnd` als Parent → owned window:
  kein Taskbar-/Alt-Tab-Eintrag, immer über Hauptfenster. README nennt es
  „resizable window“ → typisch wäre Top-Level (Parent `NULL`).
  **Achtung:** Parent-Änderung hat Nebenwirkungen: Zerstörung des Haupt-
  Fensters zerstört dann nicht mehr das Overview (WM_DESTROY in wnd_proc
  zerstört es bereits manuell — OK), Z-Behavior ändert sich. Bewusste
  Produktentscheidung nötig, nicht blind fixen.
- **Problem (2):** Im Tray-Modus ist Hauptfenster `SW_HIDE`;
  `SetForegroundWindow(app->hwnd)` in `show_tray_menu` schlägt fehl →
  Tray-Popup-Menü schließt bei „Klick daneben“ evtl. erst beim zweiten Klick
  nicht. Standard-Workaround: aktives Foreground-Fenster kurz aktivieren
  oder unsichtbares Helper-Window.
- **Verifikation:** Visuell/interaktiv; bei (1) Taskbar-Eintrag + Alt-Tab.

### I-004: Fake-Device: rand() ohne srand + ungedämpfter Temp-Random-Walk
- **Severity:** Info | **STATUS:** FIXED
- **Fix (Commit-Stand nach 738b267):** `fake_refresh` seedet einmalig `srand((unsigned)GetTickCount())` (static `seeded`-Flag) und clampt die Random-Walk-Temperaturen `temp_c[0]`/`temp_c[3]` auf 20–70 °C.
- **Verifikation:** `temp/verify_i004.py` + Test-Build `build-i004test` (temporärer Patch: `corsair_find_devices` liefert 0 Geräte, damit `CFC_FAKE` auf der Maschine mit echtem Gerät injiziert; Patch NICHT committen, `src/corsair_hid.c` danach wiederhergestellt und per `git diff --stat` verifiziert). Zwei `CFC_FAKE=1`-Runs à 5 Samples (t≈5–21 s): RPM-Jitter aktiv in beiden Runs (1234–1248 um Ziel 1240), die Sequenzen der Runs unterscheiden sich (seeded → nicht die identische unseeded-`rand()`-Sequenz), alle Fake-Sensor-Temperaturen im Bereich 20–70 °C (beobachtet 27.4–27.9 °C).
- **Betroffene Dateien:** `src/main.c`
- **Betroffen:** `src/main.c:529 (jitter), 536–537 (temp_c Random-Walk)`
- **Problem:** Deterministische Jitter-Sequenz; Fake-Temperaturen können über
  Stunden in unrealistische Bereiche driften (kein Clamp). Betrifft nur
  `CFC_FAKE`-Testbetrieb — wird relevant nach M-001-Fix.
- **Fix-Richtung:** `srand((unsigned)GetTickCount())` einmalig in
  `fake_fill_base` (oder beim ersten Scan) initialisieren; Temperatur-Walk
  auf plausible Spanne clampen (z. B. 20–70 °C).
- **Verifikation:** `CFC_FAKE=1`, lange Laufzeit: Temperaturen bleiben im
  plausiblen Bereich; Jitter-Sequenz startet nicht immer identisch.

### I-005: `strstr(cmd_line, "--tray")` — Substring, ANSI
- **Severity:** Info | **STATUS:** FIXED
- **Fix (Commit-Stand nach 738b267):** `WinMain` parst die Wide-Commandline via `GetCommandLineW` + `CommandLineToArgvW` (shell32, bereits gelinkt) und vergleicht exakt auf den Token `--tray` (`lstrcmpW`); ANSI-Substring-`strstr` entfernt (ANSI-Parameter nur noch `(void)`-referenziert).
- **Verifikation:** `temp/verify_i005.py` (Clean-Build `build-verify`): `--tray` → Hauptfenster hidden (Tray-Modus); Pfad-Argument `G:\...\temp\--tray` (enthält „--tray“ nur als Substring) → Fenster sichtbar (kein False-Trigger); Start ohne Argument → Fenster sichtbar (Regression).
- **Betroffene Dateien:** `src/main.c`
- **Betroffen:** `src/main.c:2769 (WinMain)`
- **Problem:** Tray-Modus per Substring auf ANSI-Commandline: Pfade/Args, die
  „--tray“ enthalten, aktivieren Tray-Modus; Nicht-ANSI-Pfade sind verzerrt.
- **Fix-Richtung:** `GetCommandLineW` + `CommandLineToArgvW` (shell32 — bereits
  gelinkt), exakter Token-Vergleich auf `--tray`.
- **Verifikation:** Start mit `--tray` (Tray-Modus), Start ohne (Fenster),
  Start mit Pfad-Argument, das „--tray“ enthält (darf Tray-Modus NICHT
  triggern).

---

## Unverified Concerns — NICHT blind fixen (erst verifizieren!)

Diese Punkte sind **nicht** als bestätigte Fehler klassifiziert. Für jedes:
erst die „Notwendige Verifikation“ durchführen, dann entscheiden.

### U-001: Stale manual-reset OVERLAPPED-Event nach CancelIoEx (corsair_hid.c:176–193)
- **Verdacht:** Nach Read/Write-Timeout + `CancelIoEx` kann ein spät
  einlaufendes Completion das Event *nach* dem `ResetEvent` der nächsten
  Transaktion setzen → `WaitForSingleObject` liefert sofort `WAIT_OBJECT_0`,
  `GetOverlappedResult(FALSE)` meldet dann `ERROR_IO_PENDING` für die neue
  Operation → falsche Fehlermeldung + Controller-Close.
- **Warum unbestätigt:** Window extrem klein, heilt sich per nächstem Tick
  (drain + Reopen); exaktes USB-Cancel-Timing statisch nicht beweisbar.
- **Notwendige Verifikation:** Hardware-Test mit erzwungenen Timeouts (z. B.
  Fake-Device mit Latenz) + Logging der Event-Zustände vor/nach Transaktionen.
- **Mögliche Fix-Richtung (falls bestätigt):** Event pro Transaktion neu
  anlegen/verwenden, oder `ResetEvent` erst NACH `GetOverlappedResult` des
  abgeschlossenen I/Os; alternativ `HasOverlappedIoCompleted`-Prüfung.

### U-002: `HidD_GetProductString(handle, dst->product, sizeof(dst->product))` (corsair_hid.c:159)
- **Verdacht:** `sizeof` liefert Byte-Zahl (256), API erwartet Zeichen-Zahl.
- **Warum unbestätigt:** USB-Spezifikation begrenzt Product-Strings auf ≤127
  Zeichen → praktisch kein Overflow möglich.
- **Notwendige Verifikation:** nur bei exotischen Geräten.
- **Mögliche Fix-Richtung (falls befestigt):**
  `sizeof(dst->product) / sizeof(dst->product[0])` übergeben.

### U-003: Gerätesentinel 0xFFFF bei Temperatur → -0.01 °C Anzeige
- **Verdacht:** `(double)(int16_t)u16_be(res,1)/100.0` (corsair_hid.c ~471)
  rendert 0xFFFF als -0.01 °C.
- **Warum unbestätigt:** Sentinel-Wert gerätespezifisch, ohne Hardware nicht
  verifizierbar.
- **Notwendige Verifikation:** `corsair_nvidia_probe` an echtem Gerät mit
  defektem/fehlendem Sensor.
- **Mögliche Fix-Richtung (falls bestätigt):** 0xFFFF (und ggf. 0x8000) als
  „keine Daten“ behandeln → 0.0 / nicht verbunden markieren.

### U-004: Global\ vs. session-lokaler Mutex unter gemischter Privilegienlage
- **Verdacht:** `corsair_open` (corsair_hid.c ~395–397) fällt auf
  `CorsairLinkReadWriteGuardMutex` (session-lokal) zurück, wenn
  `Global\…` nicht erstellbar ist → Peer in unterschiedlichen Object-Namespaces
  synchronisieren nicht.
- **Warum unbestätigt:** Randfall (erhöhter Prozess + nicht-erhöhter Peer);
  `drain_reports` + `WAIT_ABANDONED`-Handling mildern.
- **Notwendige Verifikation:** kein Defekt in dokumentierter Nutzung;
  nur bei beobachtetem iCUE-/FanControl-Konflikt prüfen.
- **Mögliche Fix-Richtung (falls bestätigt):** konsistenten Namespace wählbar
  machen oder beide Handles parallel halten.

### U-005: `wc.hbrBackground = t->ink_brush` (main.c:2781) — stale GDI-Handle nach ui_set_dpi
- **Verdacht:** `ui_release_brushes` (ui.c:68) löscht den beim Class-Register
  referenzierten Brush und ersetzt ihn bei `ui_set_dpi` → Default
  `WM_ERASEBKGND` nach Monitor-DPI-Wechsel könnte stale GDI-Handle verwenden.
- **Warum unbestätigt:** Farbwerte zwischen DPI-Wechseln identisch; sichtbare
  Auswirkung vom GDI-Handle-Reuse-Verhalten abhängig.
- **Notwendige Verifikation:** Multi-Monitor-DPI-Test (z. B. 100 % → 150 %
  Drag) mit GDI-Handle-Logging oder WinDbg-`!handle`.
- **Mögliche Fix-Richtung (falls bestätigt):** Class-Brush nie löschen/
  ersetzen (nur neu anlegen wenn Wert sich ändert) oder `wc.hbrBackground = NULL`
  und Erase vollständig in `WM_ERASEBKGND`/`WM_PAINT`-Logik.

### U-006: GpuCurvePts mit 13–16 Punkten: geparst, aber stumm verworfen
- **STATUS:** FIXED (Verifikation: statische Limit-Konsistenz, wie im Finding gefordert)
- **Verifikation + Fix:** Das Parser-Limit in `parse_curve_points` (main.c) ist jetzt die gemeinsame Konstante `UI_CURVE_MAX_POINTS` (12, ui.h:97) statt 16; `ui_curve_set_points` (ui.c) akzeptiert weiterhin max. 12 → die Grenzen sind identisch, und 13–16-Punkte-Strings werden beim Settings-Load abgelehnt (Fallback auf Defaults), statt geparst und stumm verworfen zu werden. Statisch verifiziert: beide Stellen verwenden dieselbe Konstante, die UI kann max. 12 Punkte erzeugen.
- **Verdacht:** `parse_curve_points` akzeptiert bis 16 Punkte (main.c:639,
  Grenze 654), `ui_curve_set_points` verwirft >12 (ui.c:1560) → Curve fällt
  auf Defaults zurück, ohne Hinweis.
- **Warum unbestätigt:** aus eigener UI nicht produzierbar (Max 12); nur via
  externem Registry-Schreiber; Verhalten ist defensiv.
- **Notwendige Verifikation:** Grenzen konsistent machen (16 → 12 in Parser).
- **Mögliche Fix-Richtung (falls befestigt):** Parser-Limit in main.c:654 auf
  `UI_CURVE_MAX_POINTS` (12) setzen ODER beide auf gemeinsamen Wert.

---

## Test- & Verifikationslücken (für Test-Aufträge an KI)

1. **Keine automatisierten Tests, kein CI existiert.** `temp/verify.py` ist
   ein einmaliges manuelles Screenshot-Skript (nicht wiederholbar).
2. **Unit-Tests für reine Funktionen** (trivial, alle vorhanden):
   - `parse_curve_points` (main.c:639): 1 Punkt (false), 2 Punkte (true),
     Duplikate, Werte >100/<0, Leerraum, 16-Punkte-Limit, leerer String.
   - `ui_curve_interpolate` (ui.c ~1111): Endpunkte, negative Temperatur,
     `t1 <= t0`-Guard, 2-Punkte-Minimum.
   - `device_settings_key` (main.c ~399): Hash-Stabilität über Aufrufe.
   - Settings-Read/Write-Runde inkl. Legacy-Migration (main.c:680, 727).
3. **Lebenszyklus-Test:** Overview 3× Create/Destroy → Zeilendaten sichtbar
   (fängt M-002).
4. **Fake-Device-Testpfad** (nach M-001-Fix): Open/Refresh/Apply/Close-Zyklus
   ohne Hardware (fängt M-001-Regressionen, M-003-Basis).
5. **Keine Concurrency-/Wettkampf-Tests** (U-001, Mutex-Kontention 2. Prozess).
6. **Keine Hardware-Tests** — HID-Protokoll nur gegen öffentliche Referenzen
   (Linux `corsair-cpro`, liquidctl) plausibilisiert.

---

## Positive Beobachtungen (NICHT ändern)

- Puffer-/Ressourcenhygiene durchgängig korrekt (alle `copy_wstr`/`swprintf`/
  `MultiByteToWideChar`-Aufrufe overflow-sicher; keine Buffer-Overflow-Finding).
- Curve-Editor-Invariante (strikt steigend, eindeutige Temperaturen) by
  construction (Drag-Clamping, Dedup, `t1 <= t0`-Guard → keine Division/0).
- Defensive Settings-Handhabung (Typ-Checks, Clamping, Legacy-Migration;
  Duty immer 0–100, Modes aus festem Enum → keine Out-of-Range-Hardware-Befehle).
- Sicherer DLL-Load (`LOAD_LIBRARY_SEARCH_SYSTEM32` zuerst, Bounds-geprüfter
  Fallback); NVAPI-Interface-IDs/-Layout exakt mit offizieller Definition
  identisch; `count=0` vor `NvAPI_EnumPhysicalGPUs` entspricht NVIDIA-Sample.
- Vollständige Ressourcen-Cleanup auf allen Pfaden; `corsair_close` idempotent.
- Prozessübergreifende Koordination via `Global\CorsairLinkReadWriteGuardMutex`
  mit Session-Fallback, `WAIT_ABANDONED`-Handling, Report-Drain.
- Fehlerisolierung pro Controller; Tray-Icon-Retry + `TaskbarCreated`-Reaktivierung.

## Externe Referenzen (für KI-Nachschlagen)

| Quelle | Relevanz |
|---|---|
| NVIDIA NVAPI Reference, `group__gpu.html` (docs.nvidia.com) | `NvAPI_EnumPhysicalGPUs` Parameter-Contract |
| NVIDIA-Sample `Sample_Code/DisplayColorControl/NVHelper.cpp` + `main.cpp` (GitHub `NVIDIA/nvapi`) | `count=0`-Initialisierung ist das Referenzmuster |
| `nvapi_lite_common.h` (GitHub `NVIDIA/nvapi`) | `NvAPI_EnumPhysicalGPUs`-Dokumentation |
| Windows SDK 10.0.22621.0 `um/winuser.h` | `WM_CTLCOLOR*`-Werte (0x132–0x138); `WM_CTLCOLORLISTVIEW` (0x013C) ist in SDK-Headern nicht definiert → `#define` mit `#ifndef`-Guard in ui.c ist korrekt, NICHT „fixen“ |
| UCRT `corecrt_wstdio.h` (SDK 10.0.26100) | `swprintf` ist ISO-C99-konform (Count-Argument) → `swprintf(label, 8, L"%d", …)`-Aufrufe in ui.c sind korrekt, NICHT „fixen“ |
