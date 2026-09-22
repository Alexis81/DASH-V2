# DASH-V2

Afficheur pour un ESP32-S3 Waveshare Touch LCD 7 pouces. Il écoute un bus CAN classique à 1 Mbit/s et décode les trames moteur au format du module Jhoinrch RH-02 Plus.

Le dossier `Simulateur` fait émettre ces mêmes trames par le RH-02, branché en USB sur l'ordinateur, pour faire tourner l'écran sans le véhicule.

## Matériel

- Carte [Waveshare ESP32-S3-Touch-LCD-7](https://www.waveshare.com/wiki/ESP32-S3-Touch-LCD-7) : écran RGB 800×480, tactile GT911, flash 8 Mo, PSRAM octale.
- Bus CAN sur GPIO 19 (RX) et GPIO 20 (TX), 1 Mbit/s, identifiants standard 11 bits, 8 octets.
- Adaptateur Jhoinrch RH-02 Plus (firmware SLCAN CANable2) pour le simulateur.

CANH et CANL relient l'afficheur au RH-02 ou au faisceau. Une résistance de 120 Ω doit terminer chaque extrémité du bus. Le RH-02 a un interrupteur pour la sienne.

## Trames

Les entiers 16 bits sont en big-endian. Les octets marqués « brut » sont transmis tels quels.

| ID | Octets | Signal | Décodage |
| --- | --- | --- | --- |
| `0x3E8` | 0–1 | Régime (tr/min) | brut |
| `0x3E8` | 2–3 | Dépression collecteur (kPa) | brut − 100 |
| `0x3E8` | 4 | Température moteur (°C) | brut − 50 |
| `0x3E8` | 5 | Température air admission (°C) | brut − 50 |
| `0x3E8` | 6 | Tension calculateur (V) | brut × 0,1 |
| `0x3E8` | 7 | Température huile (°C) | brut − 50 |
| `0x3E9` | 0–1 | Papillon (%) | brut × 0,1 |
| `0x3E9` | 2–3 | Avance allumage (°) | (brut × 0,1) − 100 |
| `0x3E9` | 4 | Vitesse | brut |
| `0x3E9` | 5 | Pression huile | brut |
| `0x3E9` | 6 | Pression essence | brut |
| `0x3E9` | 7 | Température calculateur (°C) | brut − 50 |
| `0x3EA` | 0–1 | Lambda 1 | brut × 0,001 |
| `0x3EA` | 2–3 | Lambda 2 | brut × 0,001 |
| `0x3EA` | 4–5 | Position volant | (brut − 30000) / 10 |
| `0x3EA` | 6–7 | Pression atmosphérique (kPa) | brut × 0,1 |

## Firmware

ESP-IDF 6.1, cible `esp32s3`.

```bash
. $IDF_PATH/export.sh
idf.py set-target esp32s3
idf.py build flash monitor
```

`idf.py` télécharge LVGL et le pilote GT911 listés dans `main/idf_component.yml`. Les versions figées sont dans `dependencies.lock`.

Au démarrage, l'écran affiche le logo, puis les coordonnées du toucher. Les valeurs CAN sont lues en continu et écrites sur la console série toutes les deux secondes dès qu'une trame valide est arrivée.

## Simulateur

Le script parle au RH-02 en SLCAN et envoie `0x3E8`, `0x3E9` et `0x3EA` toutes les 20 ms. Le scénario dure 40 secondes et se répète : contact, démarrage, ralenti, accélérations, frein moteur, avec une montée en température.

```bash
python3 -m pip install -r Simulateur/requirements.txt
python3 Simulateur/simulateur.py
```

Le port CANable (`16D0:117E`) est choisi tout seul. La console USB de l'afficheur (puce WCH) est ignorée.

```bash
python3 Simulateur/simulateur.py --list
python3 Simulateur/simulateur.py --port /dev/cu.usbmodemXXXX
```

`Ctrl+C` ferme le canal CAN.
