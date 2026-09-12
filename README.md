# Programmateur horaire ESP32 — multi-relais avec interface web Mode OTA
![Platform](https://img.shields.io/badge/Platform-ESP32-green)
![Framework](https://img.shields.io/badge/Framework-Arduino-blue)
![Status](https://img.shields.io/badge/Status-Active-green)
![Release](https://img.shields.io/badge/Release-v1.0.0-orange)
Ce programme transforme un ESP32 en programmateur horaire connecté, capable de piloter plusieurs relais indépendants (portail, éclairage, arrosage, chauffage...) selon des horaires programmables, avec une interface web embarquée accessible depuis n'importe quel navigateur du réseau local.

## Fonctionnalités

- **Nombre de relais configurable** : de 1 à une quinzaine, en ajoutant/retirant simplement une ligne dans un tableau — aucune autre partie du code n'a besoin d'être modifiée (page web, routes HTTP, sauvegarde et écran OLED s'adaptent automatiquement).
- **Deux modes par relais** :
  - **Automatique** : le relais suit une plage horaire programmée (`Début` → `Fin`), y compris les plages qui traversent minuit (ex: 22:00 → 06:00).
  - **Manuel** : l'utilisateur force l'état ON/OFF, indépendamment des horaires.
- **Interface web** responsive (pensée pour mobile), avec :
  - une carte par relais (nom, sous-titre, horaires, voyant d'état ON/OFF, bouton de bascule AUTO/MANUEL) ;
  - des actions groupées "Tout ON", "Tout OFF", "Tout AUTO" ;
  - un compte à rebours avant le prochain changement d'état ("Allumage dans..." / "Extinction dans...") ;
  - la qualité du signal WiFi affichée en %, à côté du bouton d'informations système ;
  - un popup "Infos système" (état WiFi, SSID, nom mDNS, adresse IP, adresse MAC, RSSI).
- **Boutons poussoirs physiques** (optionnels) pour forcer l'état ON/OFF de chaque relais directement sur le boîtier, avec anti-rebond logiciel.
- **Écran OLED** (SSD1306 I2C, en option) affichant le réseau WiFi utilisé, l'adresse IP, l'heure, et l'état de chaque relais — avec pagination automatique si le nombre de relais dépasse l'espace disponible.
- **Sauvegarde persistante** des horaires, modes et états dans la mémoire flash NVS (survit aux coupures de courant), avec une écriture ciblée par relais pour limiter l'usure de la flash.
- **Connexion WiFi robuste** : scan et connexion au meilleur réseau parmi plusieurs réseaux connus (par signal RSSI), reprise automatique après coupure, bascule vers un meilleur réseau redevenu disponible, liste noire temporaire des réseaux en échec — le tout en scan **asynchrone**, sans jamais bloquer le reste du programme (boutons, écran, serveur web).
- **Horloge synchronisée** via NTP, avec fuseau horaire français (heure d'été/hiver automatique).
- **Accès par nom local** (mDNS, ex: `http://richardv.local`), en plus de l'adresse IP.
- **Mise à jour du firmware par WiFi (OTA)** : une fois le premier flash fait par USB, les évolutions suivantes du programme peuvent être téléversées directement par WiFi depuis l'IDE Arduino, sans rouvrir le boîtier — protégeable par mot de passe.

## Matériel nécessaire

- Une carte ESP32 (DevKit ou équivalent).
- Un ou plusieurs modules relais (compatibles 3.3V logique, actifs à l'état bas pour la plupart des modules bon marché à base de SRD-05VDC).
- (Optionnel) Un écran OLED SSD1306 0.96" I2C (adresse `0x3C`).
- (Optionnel) Un bouton poussoir par relais à commander physiquement.

### Câblage

| Élément | Broches |
|---|---|
| Écran OLED (I2C) | SDA = GPIO21, SCL = GPIO22 (broches I2C par défaut de l'ESP32) |
| Relais | Une broche GPIO dédiée par relais (voir le tableau `programmateurs[]`, broches utilisables : 4, 5, 13, 14, 16, 17, 18, 19, 21\*, 22\*, 23, 25, 26, 27, 32, 33 — \*sauf si déjà prises par l'écran OLED) |
| Bouton poussoir | Une broche entre le bouton et GND (l'entrée interne est en `INPUT_PULLUP`, aucune résistance externe nécessaire) |

⚠️ À éviter : les GPIO 34 à 39 (entrée seule, pas de sortie possible) et les broches de boot (0, 2, 12, 15), qui peuvent perturber le démarrage si un relais y est branché.

## Bibliothèques Arduino requises

À installer via le gestionnaire de bibliothèques de l'IDE Arduino :

- `ESPAsyncWebServer` (+ sa dépendance `AsyncTCP`)
- `Preferences` (fournie avec le core ESP32)
- `ArduinoJson`
- `ESPmDNS` (fournie avec le core ESP32)
- `ArduinoOTA` (fournie avec le core ESP32, pour la mise à jour par WiFi)
- `Adafruit GFX Library`
- `Adafruit SSD1306`

## Configuration

### 1. Identifiants WiFi (`arduino_secrets.h`)

Créez, à côté du fichier `.ino`, un fichier `arduino_secrets.h` (non fourni, à ne jamais partager publiquement) contenant vos réseaux connus :

```cpp
#define SECRET_SSID  "Nom_du_reseau_1"
#define SECRET_PASS  "mot_de_passe_1"
#define SECRET_SSID2 "Nom_du_reseau_2"
#define SECRET_PASS2 "mot_de_passe_2"

// Optionnel : décommentez pour un 3e réseau connu
// #define SECRET_SSID3 "Nom_du_reseau_3"
// #define SECRET_PASS3 "mot_de_passe_3"

// Optionnel mais recommandé : protège la mise à jour OTA (voir plus bas)
#define SECRET_OTA_PASSWORD "votre_mot_de_passe_ota"
```

L'ESP32 scanne ces réseaux au démarrage et se connecte à celui qui offre le meilleur signal.

### 2. Nom d'hôte et fuseau horaire

En haut du fichier `.ino` :

```cpp
const char* hostname = "richardv";                                  // http://richardv.local
const char *TZ_INFO = "CET-1CEST,M3.5.0,M10.5.0/3";                  // France, heure d'été/hiver automatique
```

### 3. Les relais (tableau `programmateurs[]`)

C'est le cœur de la configuration. Chaque ligne représente un relais :

```cpp
Programmateur programmateurs[] = {
  { "1.", "Programmation 1", "Cuisine",  "#f59e0b", 32, "08:05", "12:00", true, false, 14 },
  { "2.", "Programmation 2", "Portail",  "#06b6d4", 33, "12:00", "13:15", true, false, 16 },
  // ...
};
```

Champs, dans l'ordre : identifiant unique (court, sans espace), nom affiché, sous-titre, couleur d'accent (hexadécimal), broche GPIO du relais, heure de début par défaut, heure de fin par défaut, mode automatique par défaut, état par défaut, broche GPIO du bouton poussoir (`-1` si aucun).

Pour ajouter un relais : dupliquez une ligne et adaptez au minimum l'`id` et la broche GPIO. Pour en retirer un : supprimez la ligne. Rien d'autre à modifier.

## Installation

1. Renseignez `arduino_secrets.h` (voir ci-dessus).
2. Adaptez le tableau `programmateurs[]` à votre installation (nombre de relais, broches, noms).
3. Installez les bibliothèques listées plus haut.
4. Sélectionnez votre carte ESP32 dans l'IDE Arduino (Outils > Type de carte).
5. Téléversez le programme par USB.
6. Ouvrez le moniteur série (115200 bauds) pour vérifier la connexion WiFi et récupérer l'adresse IP attribuée.
7. Accédez à l'interface depuis un navigateur du même réseau : `http://<adresse-IP>` ou `http://<hostname>.local`.

## Mise à jour du firmware par WiFi (OTA)

Une fois ce premier flash fait par USB, les mises à jour suivantes peuvent se faire directement par WiFi :

1. Définissez `SECRET_OTA_PASSWORD` dans `arduino_secrets.h` (voir Configuration) pour protéger l'accès — sans lui, n'importe quel appareil du réseau WiFi pourrait reflasher l'ESP32.
2. L'ESP32 et l'ordinateur doivent être sur le même réseau WiFi.
3. Dans l'IDE Arduino, sélectionnez le port réseau qui apparaît dans **Outils > Port** (ex: `richardv at 192.168.1.44 (ESP32)`), à la place du port USB.
4. Téléversez normalement : une fenêtre demande le mot de passe OTA, puis le transfert démarre. L'écran OLED (si présent) affiche la progression, puis l'ESP32 redémarre automatiquement.

⚠️ Le port réseau ne sert qu'au transfert du programme : le moniteur série (`Serial.print`) reste accessible uniquement via le port USB. Repassez sur le port USB pour consulter les logs.

## Utilisation de l'interface web

- Chaque carte de relais affiche son nom, son état (voyant ON/OFF), son mode (AUTO/MANUEL) et le temps restant avant le prochain changement d'état en mode automatique.
- En mode **AUTO** : modifiez les heures de début/fin puis validez avec le bouton ✓.
- En mode **MANUEL** : un bouton "⚡ Forcer ON ou OFF" bascule directement l'état du relais.
- Le bouton central de chaque carte bascule entre AUTO et MANUEL.
- Les boutons "Tout ON" / "Tout OFF" / "Tout AUTO" en haut de page agissent sur l'ensemble des relais en une fois.
- Le bouton d'informations système (icône WiFi, à côté du pourcentage de signal) ouvre un popup avec l'état de la connexion, le réseau utilisé, l'adresse IP/MAC et la puissance du signal (dBm et %).

## Routes HTTP exposées par l'ESP32

| Route | Méthode | Rôle |
|---|---|---|
| `/` | GET | Sert la page web principale |
| `/get-config` | GET | Liste des relais déclarés (id, nom, sous-titre, couleur) |
| `/get-data` | GET | État complet de tous les relais + heure courante + % de signal WiFi (interrogée chaque seconde) |
| `/get-info` | GET | Informations système (WiFi, IP, MAC, RSSI) |
| `/toggle-mode?id=...` | GET | Bascule un relais entre AUTO et MANUEL |
| `/force-state?id=...` | GET | Force l'état ON/OFF d'un relais (passe en MANUEL) |
| `/save?id=...` | POST | Enregistre les nouveaux horaires (`debut`, `fin`) d'un relais |

## Stockage des réglages

Les horaires, le mode et l'état de chaque relais sont sauvegardés dans la mémoire flash NVS (namespace `config`, via la bibliothèque `Preferences`), et restaurés automatiquement au démarrage. Chaque action (bascule de mode, forçage, changement d'horaire, appui sur un bouton poussoir) ne réécrit que les réglages du relais concerné, afin de limiter l'usure de la mémoire flash.

## Notes et limitations

- **Aucune authentification sur l'interface web** : toute personne connectée au même réseau WiFi peut piloter les relais depuis l'interface web. À garder en tête si l'un des relais commande un équipement sensible (portail, alarme...).
- **OTA sans mot de passe défini** : si `SECRET_OTA_PASSWORD` n'est pas renseigné, la mise à jour à distance reste ouverte à tout appareil du même réseau WiFi — pensez à le définir (voir Configuration).
- L'écran OLED et les boutons poussoirs sont optionnels : le programme fonctionne sans, il suffit de ne pas les câbler (l'écran est simplement désactivé si non détecté au démarrage, et un relais sans bouton se déclare avec `pinBP = -1`).
