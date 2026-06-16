<p align="center">
  <img src="https://github.com/user-attachments/assets/057d7090-08d1-4f1d-8575-eb0081d24a46" alt="1" />
</p>

# Piano Lights, bridge BLE/MIDI entre Synthesia et un ruban LED

Firmware Arduino pour ESP32 WROOM : périphérique **BLE MIDI** reconnu par
Windows et utilisable comme **sortie MIDI dans Synthesia** (fonction key
lights). Les notes reçues allument les LEDs correspondantes d'un ruban
**WS2812B**. La calibration et les couleurs se règlent via une **page web
embarquée** (WiFi STA avec repli AP automatique).

## 1. Installation (Arduino IDE)

1. Installer le support de carte **esp32 by Espressif Systems**
   (Gestionnaire de cartes).
2. Installer via le Gestionnaire de bibliothèques :
   - **MIDI Library**        (Francois Best)
   - **BLE-MIDI**            (lathoub, mais à remplacer après)
   - **NimBLE-Arduino**      (h2zero)
   - **FastLED**             (Daniel Garcia)
   - **ArduinoJson**         (Benoit Blanchon)
   - **ESP Async WebServer** (ESP32Async)
   - **Async TCP**           (ESP32Async)
3. Sélectionner la carte **ESP32 Dev Module** et surtout dans **Tools** :
   **Partition Scheme → "Minimal SPIFFS (1.9MB APP with OTA/128KB SPIFFS)"**.
   En effet, avec BLE + WiFi, le binaire dépasse la partition app par défaut.
4. Ouvrir `PianoLights.ino` (le fichier `PianoLights.h` doit être dans
   le même dossier, il apparaîtra comme second onglet) et téléverser.

> **Remplacement de la bibliothèque BLE-MIDI** :
> La version de `BLE-MIDI` (lathoub) distribuée par le gestionnaire n'est pas
> compatible en l'état avec NimBLE tel qu'utilisé ici (le correctif porte sur
> `src/hardware/BLEMIDI_ESP32_NimBLE.h`). Une version déjà corrigée est fournie
> avec le projet dans `libraries/BLE-MIDI.zip`. Pour l'installer :
>
> 1. Fermer Arduino IDE.
> 2. Décompresser `libraries/BLE-MIDI.zip` (du dossier du projet) : l'archive
>    contient un dossier `BLE-MIDI/`.
> 3. Remplacer **intégralement** le contenu du dossier
>    `C:\Users\%USERNAME%\Documents\Arduino\libraries\BLE-MIDI`
>    par le contenu de cette archive. Concrètement : supprimer l'éventuel
>    dossier `BLE-MIDI` existant (s'il a été installé par le gestionnaire),
>    puis copier le dossier `BLE-MIDI/` issu de l'archive à sa place, de sorte
>    d'obtenir `...\Arduino\libraries\BLE-MIDI\src\hardware\BLEMIDI_ESP32_NimBLE.h`.
>    On remplace tout le dossier (et pas seulement ce fichier) pour éviter tout
>    mélange de versions.
> 4. Rouvrir Arduino IDE : la bibliothèque corrigée est alors prise en compte.

## 2. Première mise en route

1. Au premier démarrage (aucun WiFi enregistré), l'ESP32 ouvre le point
   d'accès **`Piano-Lights-AP`** (ouvert, sans mot de passe).
2. S'y connecter, ouvrir **http://192.168.4.1**, renseigner le SSID/mot de
   passe de votre box dans la section WiFi → l'ESP32 redémarre en STA.
3. Ensuite, la page est accessible sur **http://pianolights.local** (ou via
   l'IP affichée dans le moniteur série à 115200 bauds).
4. En cas d'échec de connexion (mot de passe erroné, box éteinte), le mode
   AP se réactive automatiquement après 15 s.

## 3. Appairage Windows + Synthesia

1. Windows : **Paramètres → Bluetooth et appareils → Ajouter un appareil →
   Bluetooth** → appairer **Piano-Lights**. (Windows 10/11 requis : le BLE MIDI
   passe par l'API UWP / WinRT MIDI.)
2. Activer la prise en charge de WinRT pour que Synthesia voie le bridge BLE/MIDI :
   - maintenir la touche **Shift** enfoncée pendant le lancement de
     **Synthesia** pour ouvrir la fenêtre de configuration
   - dans la liste déroulante **Setting**, chercher
     **`Midi.UseWinRTMidi`**
   - cocher la case **Value**, puis fermer la configuration et relancer Synthesia
3. Synthesia : **Settings → Music Devices** → dans les sorties, activer
   **Piano-Lights (MIDI OUT)** et y activer la fonction d'éclairage des touches
   (*key lights*). À ce jour, il n'y a pas de mode qui sépare les mains par canal.
4. Reporter les **numéros de canaux** main gauche / main droite affichés
   par Synthesia dans la page web (section Couleurs). Par défaut le
   firmware attend gauche = canal 1, droite = canal 2 ; tout autre canal
   prend la troisième couleur.
5. Désactiver les fonctions autre que l'éclairage des touches pour ne pas avoir
   le feedback du piano lui-même (*Prevent "local" notes* ne semble pas le faire).

## 4. Calibration

Dans la section Alignement de la page web :

- Choisir un **preset de densité** (60 / 96 / 120 / 144 / 240 / 332 LEDs/m)
  qui préremplit « LED par touche », puis affiner ce ratio **en décimal** : si
  l'alignement dérive progressivement le long du clavier, ajuster de
  ±0,01–0,05.
- **Offset** : aligne la première LED sur la première touche.
- **Inversé** : si le ruban est posé avec son câble du côté droit.
- Cliquer sur les touches du **clavier virtuel** allume réellement les LEDs.
« Enregistrer les préférences » sauvegarde le tout en flash.

## 5. Architecture

| Fichier        | Rôle                                                        |
|----------------|-------------------------------------------------------------|
| `PianoLights.ino` | BLE MIDI, mapping notes→LEDs, WiFi STA/AP, API HTTP, NVS    |
| `PianoLights.h`   | Page de configuration (HTML/CSS/JS autonome, en PROGMEM)    |

API HTTP : `GET /api/config`, `POST /api/config`, `POST /api/test`
(`{note, on, ch}`), `POST /api/alloff`, `POST /api/wifi`, `POST /api/reboot`.
