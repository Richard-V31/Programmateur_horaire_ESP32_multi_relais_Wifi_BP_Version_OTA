// ============================================================================
//  PROGRAMMATEUR HORAIRE ESP32 - N RELAIS AVEC INTERFACE WEB (version flexible)
//   ET MISE A JOUR OTA
// ============================================================================
//  Ce programme transforme un ESP32 en programmateur horaire connecté :
//  - Il pilote un nombre CONFIGURABLE de relais indépendants (voir le tableau
//    "programmateurs[]" plus bas : ajoutez/retirez une ligne pour changer le
//    nombre de relais, aucune autre modification n'est nécessaire).
//  - Chaque relais peut fonctionner en mode AUTOMATIQUE (horaires programmés)
//    ou en mode MANUEL (forçage ON/OFF par l'utilisateur)
//  - Une page web embarquée (servie directement par l'ESP32, sans carte SD
//    ni système de fichiers pour le HTML/CSS/JS) permet de piloter et configurer le
//    tout depuis un navigateur, sur le réseau local. La page s'adapte
//    automatiquement au nombre de relais déclarés dans le tableau ci-dessous.
//  - Les réglages (horaires, modes, états) sont sauvegardés dans la mémoire
//    flash NVS (via la bibliothèque Preferences), pour survivre aux coupures de courant.
// ============================================================================

// --- BIBLIOTHÈQUES ---
#include <WiFi.h>              // Gestion de la connexion WiFi de l'ESP32
#include "arduino_secrets.h"   // Fichier séparé contenant le nom du réseau (SSID), le mot de passe WiFi et le mot de passe OTA
#include <ESPAsyncWebServer.h> // Serveur web asynchrone (ne bloque pas la boucle principale)
#include <Preferences.h>       // Stockage clé/valeur en mémoire flash NVS (pour sauvegarder les réglages)
#include <ArduinoJson.h>       // Sérialisation/désérialisation JSON (lecture/écriture de config.json)
#include <time.h>              // Fonctions de gestion de l'heure système (NTP, strftime...)
#include <ESPmDNS.h>           // Permet d'accéder à l'ESP32 via un nom local (ex: http://richardv.local)
#include <ArduinoOTA.h>        // 🔄Mise à jour du firmware par WiFi (OTA), sans câble USB
#include <Wire.h>              // Bus I2C, utilisé pour communiquer avec l'écran OLED
#include <Adafruit_GFX.h>      // Bibliothèque graphique de base (texte, formes...) pour l'écran OLED
#include <Adafruit_SSD1306.h>  // Pilote pour l'écran OLED SSD1306 (0.96" I2C)

// --- CONFIGURATION GÉNÉRALE ---

// 🔒Liste des réseaux WiFi connus (définis dans arduino_secrets.h). 
// L'ESP32 scannera les réseaux disponibles et se connectera à celui de cette liste
// qui offre le meilleur signal (RSSI). SECRET_SSID3/PASS3 sont optionnels :
// il suffit de les décommenter dans arduino_secrets.h pour ajouter un 3e réseau ou un 4e réseau
struct WifiNetwork {
  const char* ssid;
  const char* pass;
};

// 🚩4️⃣ Types utilisés par la gestion WiFi non bloquante (voir plus bas, section
// "RECHERCHE ET CONNEXION AU MEILLEUR RÉSEAU WIFI CONNU"). Déclarés ici, tout
// en haut du fichier, à cause d'une contrainte de l'IDE Arduino : elle génère
// automatiquement les prototypes de toutes les fonctions du sketch et les
// insère près du début du fichier (avant le code qui suit) — un type
// personnalisé utilisé comme type de retour d'une fonction doit donc être
// défini AVANT ce point d'insertion, sous peine d'erreur de compilation
// ("'WifiConnResult' does not name a type").
enum WifiConnStep { WCS_IDLE, WCS_SCANNING, WCS_CONNECTING };
enum WifiConnResult { WCR_PENDING, WCR_CONNECTED, WCR_FAILED };
enum BetterNetStep { BNS_IDLE, BNS_SCANNING };

WifiNetwork knownNetworks[] = {
  { SECRET_SSID,  SECRET_PASS },
  { SECRET_SSID2, SECRET_PASS2 },
#ifdef SECRET_SSID3
  { SECRET_SSID3, SECRET_PASS3 },
#endif
};
const int knownNetworksCount = sizeof(knownNetworks) / sizeof(knownNetworks[0]);

// Nom d'hôte local : une fois connecté, l'ESP32 est joignable via
// 👨 http://richardv.local (en plus de son adresse IP), grâce au mDNS.
const char* hostname = "richardv";

const char *TZ_INFO = "CET-1CEST,M3.5.0,M10.5.0/3"; // Fuseau horaire (France, avec passage heure été/hiver automatique)

// ============================================================================
//  ⚙️  ZONE DE CONFIGURATION DES RELAIS — C'EST ICI QUE VOUS ADAPTEZ LE
//      NOMBRE DE PROGRAMMATIONS / RELAIS À VOTRE CARTE
// ============================================================================
//  Chaque ligne du tableau ci-dessous représente UN relais piloté par l'ESP32.
//  Pour ajouter un relais : dupliquez une ligne, changez au minimum l'id
//  (unique, sans espace, ex: "12.") et la broche GPIO. Pour en retirer un,
//  supprimez la ligne correspondante. Aucune autre partie du code n'a besoin
//  d'être modifiée : la page web, les routes HTTP, la sauvegarde NVS et
//  l'écran OLED s'adaptent automatiquement au nombre de lignes ici présentes.
//
//
//  ⚠️ Broches GPIO utilisables en sortie sur un ESP32 DevKit classique :
//     4,5,13,14,16,17,18,19,21,22,23,25,26,27,32,33
//     (SDA=21 et SCL=22 sont déjà utilisés par l'écran OLED, ne pas les réutiliser)
//  ⚠️ À éviter : GPIO 34 à 39 (entrée seule, pas de sortie possible), et les
//     broches de boot (0, 2, 12, 15) qui peuvent perturber le démarrage si un
//     relais y est branché et tire la ligne à un état inattendu.
//  ⚠️ Nombre maximal réaliste : dépend surtout du nombre de broches GPIO
//     libres sur votre carte (une quinzaine sur un ESP32 classique). Au-delà,
//     il faut passer par un module d'extension I2C (ex: PCF8574) — code non
//     inclus ici mais la structure logique ci-dessous s'y prêterait bien.

struct Programmateur {
  const char* id;        // Identifiant unique utilisé dans les routes web et le stockage NVS (court, sans espace -> Ex: 1.)
  const char* nom;       // Nom affiché sur la page web (ex: "Programmation 1")
  const char* sousNom;   // Sous-titre affiché sous le nom (ex: "Cuisine")
  const char* couleur;   // Couleur d'accent (code hexadécimal) pour ce relais dans l'interface
  int pin;               // Broche GPIO reliée au relais
  String heureDebut;     // Heure de début par défaut (écrasée par la NVS si déjà configurée)
  String heureFin;       // Heure de fin par défaut
  bool modeAuto;         // true = mode automatique (horaires) par défaut
  bool relayState;       // État par défaut (false = éteint)
  int pinBP;             // Broche GPIO du bouton poussoir de forçage physique (câblé entre GND et cette broche).
                         // -1 = pas de bouton poussoir pour ce programmateur.
};

//  👉🚩Champs : { id, nom affiché, sous-titre, couleur (hex), broche GPIO,
//              heure de début par défaut, heure de fin par défaut,
//              mode auto par défaut, état par défaut, broche GPIO du bouton poussoir (-1 = aucun) }
//
//  🔘 Bouton poussoir de forçage physique (optionnel, uniquement câblé ici sur "1." à "4.") :
//     câblage : une broche du bouton sur GND, l'autre sur la broche GPIO indiquée
//     (la broche interne est configurée en INPUT_PULLUP, pas de résistance externe nécessaire).
//     Chaque appui bref inverse l'état du relais ET bascule automatiquement en mode manuel,
//     exactement comme le bouton "FORCER ON/OFF" de la page web (voir checkPhysicalButtons()).
Programmateur programmateurs[] = {
  { "1.", "Programmation 1", "Cuisine",  "#f59e0b", 32, "08:05", "12:00", true, false, 14 }, //#f59e0b est un code couleur hexadécimal R V B
  { "2.", "Programmation 2", "Portail",  "#06b6d4", 33, "12:00", "13:15", true, false, 16 },
  { "3.", "Programmation 3", "Relais 3", "#0CE892", 25, "14:00", "16:26", true, false, 17 },
  { "4.", "Programmation 4", "Relais 4", "#ef4444", 26, "22:00", "04:15", true, false, 18 },

  // 👉 Exemples de lignes supplémentaires à décommenter/adapter pour aller au-delà de 4 relais :
  //🔘 (mettre la broche du GPIO en dernier pour un programmateur avec bouton poussoir ou -1 pour sans Bouton poussoir)
  // { "5.",  "Programmation 5",  "Relais 5",  "#a78bfa", 27, "12:00", "22:00", true, false, -1 },
  // { "6.",  "Programmation 6",  "Relais 6",  "#f472b6", 14, "13:00", "23:00", true, false, -1 },
  // { "7.",  "Programmation 7",  "Relais 7",  "#38bdf8", 12, "07:00", "17:00", true, false, -1 },
   //{ "8.",  "Programmation 8",  "Relais 8",  "#fbbf24", 13, "06:00", "16:00", true, false, -1 },
};
const int NB_PROGRAMMATEURS = sizeof(programmateurs) / sizeof(programmateurs[0]);

// --- 🔘 ÉTAT INTERNE POUR L'ANTI-REBOND (DEBOUNCE) DES BOUTONS POUSSOIRS ---
// Un tableau par programmateur (même s'il n'a pas de bouton, pour garder les
// index en concordance avec programmateurs[]). Non utilisé si pinBP == -1.
const unsigned long BP_DEBOUNCE_MS = 40;        // Délai anti-rebond en millisecondes
bool bpDernierEtatLu[NB_PROGRAMMATEURS];       // Dernière lecture brute de la broche (avant stabilisation)
bool bpEtatStable[NB_PROGRAMMATEURS];          // Dernier état stabilisé (après anti-rebond)
unsigned long bpDerniereBascule[NB_PROGRAMMATEURS]; // Instant (millis) du dernier changement de lecture brute

// Recherche une Programmation par son id
// Renvoie null si l'id est inconnu.
Programmateur* findProg(const String &id) {
  for (int i = 0; i < NB_PROGRAMMATEURS; i++) {
    if (id == programmateurs[i].id) return &programmateurs[i];
  }
  return nullptr;
}

// --- 🖥️CONFIGURATION DE L'ÉCRAN OLED (SSD1306, 0.96", I2C, adresse 0x3C) ---
// Câblage utilisé : broches I2C par défaut de l'ESP32 (SDA = GPIO21, SCL = GPIO22)
#define SCREEN_WIDTH   128
#define SCREEN_HEIGHT  64
#define OLED_RESET     -1     // Pas de broche RESET dédiée (partagée avec le reset de l'ESP32)
#define SCREEN_ADDRESS 0x3C   // Adresse I2c de l'écran
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
bool oledOK = false; // Passe à true si l'écran a été détecté correctement au démarrage

// Nombre de lignes "relais" affichables par page sur l'écran OLED 
// (le reste de l'écran est pris par l'en-tête réseau/IP). Si NB_PROGRAMMATEURS dépasse
// cette valeur, l'affichage bascule automatiquement en pages tournantes.
const int OLED_LIGNES_PAR_PAGE = 5;

// ============================================================================
//  PAGE WEB EMBARQUÉE (HTML + CSS + JAVASCRIPT)
// ============================================================================
//  Toute la page est stockée dans cette chaîne de caractères, placée en
//  mémoire flash (PROGMEM) plutôt qu'en RAM, et plutôt que dans un fichier
//  séparé sur un système de fichiers. Elle est envoyée telle quelle au
//  navigateur quand celui-ci demande la route "/".
//  IMPORTANT : cette page ne connaît PAS à l'avance le nombre de relais. Au
//  chargement, elle interroge la route "/get-config" pour savoir combien de
//  programmations existent et comment les afficher (nom, couleur...), puis
//  construit ses lignes dynamiquement. Vous pouvez donc changer le nombre de
//  relais dans le tableau "programmateurs[]" sans jamais toucher à ce code HTML.
const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="fr">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">
<title>ESP32 Control</title>
<style>
:root{
  --bg1:#0d0f16; --bg2:#141826; --card:rgba(255,255,255,.055); --line:rgba(255,255,255,.09);
  --txt:#eef1f6; --sub:#8891a0; --ok:#34d399; --off:#4b5568;
  /* 🎨 Couleurs du texte "Extinction dans..." / "Allumage dans..." (regroupées ici avec les autres couleurs) */
  --remain-on:#ff8787;  /* relais actuellement ON -> compte à rebours avant extinction */
  --remain-off:#2ecc71; /* relais actuellement OFF -> compte à rebours avant allumage */
}
*{box-sizing:border-box;-webkit-tap-highlight-color:transparent;}
html,body{height:100%;}
body{
  margin:0; font-family:system-ui,-apple-system,"Segoe UI",Roboto,Arial,sans-serif;
  color:var(--txt);
  background:
    radial-gradient(ellipse 500px 300px at 15% -10%, rgba(99,102,241,.18), transparent 60%),
    radial-gradient(ellipse 500px 300px at 100% 0%, rgba(6,182,212,.14), transparent 55%),
    linear-gradient(180deg,var(--bg1) 0%,var(--bg2) 100%);
  display:flex; justify-content:center;
  padding:10px;
}
.page{width:100%; max-width:430px; display:flex; flex-direction:column; gap:8px;}

/* HEADER */
.top{
  display:flex; align-items:center; justify-content:space-between;
  padding:10px 14px; border-radius:16px;
  background:var(--card); border:1px solid var(--line); backdrop-filter:blur(10px);
}
.top .brand{display:flex; align-items:center; gap:10px;}
.top .brand-icon{
  width:34px; height:34px; border-radius:10px; font-size:16px; display:flex; align-items:center; justify-content:center;
  background:linear-gradient(135deg,#6366f1,#22d3ee);
}
.top h1{margin:0; font-size:.9rem; font-weight:800;}
.top p{margin:0; font-size:.62rem; color:var(--sub);}
.clock{font-family:"SFMono-Regular",Consolas,Menlo,monospace; font-size:1rem; font-weight:700; display:flex; align-items:center; gap:6px;}
.dot{width:7px; height:7px; border-radius:50%; background:var(--ok); box-shadow:0 0 6px var(--ok); animation:pulse 2.2s ease-in-out infinite;}
@keyframes pulse{0%,100%{opacity:1;}50%{opacity:.35;}}

/* QUICK ACTIONS */
.quick{display:grid; grid-template-columns:repeat(3,1fr); gap:6px;}
.qbtn{
  border:1px solid var(--line); background:var(--card); color:var(--txt);
/* 🚩Taille texte Tout ON Tout OFF et AUTO-> font-size:.66rem */
  border-radius:12px; padding:8px 4px; font-size:.85rem; font-weight:700;
  display:flex; flex-direction:column; align-items:center; gap:3px; cursor:pointer;
}
/* 🚩Taille des pastilles rouge et verte Tout ON Tout OFF <-font-size:1.6rem*/
.qbtn span.ic{font-size:1.6rem;}
.qbtn:active{transform:scale(.96);}
.qbtn.on:active, .qbtn.on{border-color:rgba(52,211,153,.5);}
.qbtn.off:active, .qbtn.off{border-color:rgba(251,113,133,.5);}
.qbtn.auto:active, .qbtn.auto{border-color:rgba(99,102,241,.5);}

/* RELAY ROWS */
.row{
  border-radius:16px; background:var(--card); border:1px solid var(--line);
  /* 🚨 Espace entre "Allumage/Extinction dans..." et le bas du bloc -> valeur en 3e position (bottom) */
  /* 1px → espace intérieur en haut du bloc */
  /* 12px → espace intérieur à droite  */
  /*  1px → espace intérieur en bas (marge entre "Allumage/Extinction" du bord inférieur)  */
  /* 12px → espace intérieur à gauche  */
  padding:11px 12px 1px 12px; backdrop-filter:blur(12px);
  border-left:3px solid var(--accent);
}
.row-top{display:flex; align-items:center; gap:8px;}
/* 🚨Taille couleur et position de Nom Affiché-Programmation 1 a 4 */
.name{font-size:1rem; font-weight:700; flex:1; min-width:0; color:#ffffff;text-align:center;}
/* 🚨Taille  couleur et position de sous titre-Relais 1 a 4 <-font-size:.99rem*/
.name small{display:block; font-size:.99rem; font-weight:500; color:#ffffff; text-align:center;}
.badge{font-size:.95rem; font-weight:900; min-width:34px; text-align:center;}
.badge.on{color:var(--ok); text-shadow:0 0 14px rgba(52,211,153,.5);}
.badge.off{color:#fb7185; text-shadow:0 0 12px rgba(251,113,133,.35);}
.badge.und{color:var(--off);}

/* 🚨VOYANT ROND ON/OFF (texte cerclé, fond coloré) */
.status-dot{
  width:34px; height:34px; border-radius:50%; flex-shrink:0; cursor:default;
  display:flex; align-items:center; justify-content:center;

  /* 🚩Taille du texte ON ou OFF->font-size:.9rem */
  font-size:.9rem; font-weight:900; letter-spacing:.02em; color:#fff;
  border:1px solid rgba(255,255,255,.15); transition:background .2s, box-shadow .2s;
}
/* 🚩Couleur du fond du bouton rond ON et OFF dans rgba(52,211,153,.6)*/
.status-dot.on{background:rgba(52,211,153,.9); box-shadow:0 0 14px rgba(52,211,153,.7); border-color:rgba(52,211,153,.9);}
.status-dot.off{background:rgba(237,92,92,0.9); box-shadow:0 0 12px rgba(237,92,92,0.7); border-color:rgba(237,92,92,0.9);}
.status-dot.und{background:rgba(255,255,255,.06); box-shadow:none; color:var(--sub);}

/* 🚩1️⃣ Bouton rectangulaire AUTO / MANUEL */
.mode-btn{
  min-width:92px; padding:8px 12px; border: 5px solid transparent; cursor:pointer;
  border-radius:12px; /* 🚨bords arrondis -> ajuster ce rayon */
  font-size:.85rem; font-weight:900; letter-spacing:.04em; text-transform:uppercase; color:#fff;
  text-align:center; transition:background .2s, box-shadow .2s;
}
/* 🚨Couleur fond et bordure Bouton mode AUTO Orange*/
.mode-btn.auto{
  background:rgba(251, 195, 12, 1);
  border-color: rgba(134, 119, 79, 1);
/* 🚨 Texte sombre (hérité de .mode-btn) : le fond orange clair
     offre un contraste trop faible avec du texte blanc (~1.6:1, sous le
     minimum WCAG AA de 3:1). Avec ce texte foncé, le contraste dépasse 12:1. */
  color:#1a1200;
}
/* 🚨Couleur fond et bordure Bouton mode MANUEL Vert */
.mode-btn.manuel{
  background:rgba(19, 151, 155, 1);
  border-color: rgba(15, 65, 73, 0.8);
  }

.row-body{display:flex; align-items:center; gap:8px; margin-top:8px;}
.tfield{flex:1; display:flex; flex-direction:column; gap:2px;}
/* 🚨 Taille du texte Debut et Fin*/
/* Ancien .tfield label{font-size:.58rem; text-transform:uppercase; color:var(--sub); letter-spacing:.05em;} */
.tfield label{font-size:.80rem; text-transform:uppercase; color:#ffffff; letter-spacing:.05em;}
input[type="time"]{
/* 🚨AGRANDIR TEXTE HEURE PROG font-size:.78rem -> 1.2; */
  width:100%; padding:6px 6px; font-size:1.4rem; color:var(--txt);
  background:rgba(255,255,255,.06); border:1px solid var(--line); border-radius:9px; color-scheme:dark;
}
input[type="time"]:focus{outline:none; border-color:var(--accent);}
/* 🚨Position Bouton Sauvegarder */
.savebtn {
  align-self: flex-end; border: none; cursor: pointer; border-radius: 9px; padding: 7px 10px;
  background: var(--accent); color: #0b0d12; font-size: .9rem; font-weight: 800; line-height: 1;
  margin-bottom: 9px; /* Ajustez margin-bottom (ex: 10px, 15px, 20px) pour le remonter plus ou moins */
}
/*  🚨Taille du texte ⚡ Forcer ON ou OFF -> font-size:.9rem */
.forcebtn{
  flex:1; border:1px solid var(--line); background:rgba(255,255,255,.05); color:var(--txt);
  border-radius:9px; padding:7px 10px; font-size:1.2rem; font-weight:700; cursor:pointer;
}
/* 🚨Taille du texte Sauvegardé ✓ -> msg{font-size:.85rem */
.forcebtn:active{background:rgba(var(--accent-rgb),.18); border-color:var(--accent);}
.msg{font-size:.85rem; color:#4ade80; height:12px; margin:2px 0 0; text-align:right;}
/* 🚨 Temps restant avant le prochain changement d'état (ON->OFF ou OFF->ON) ⏳ Allumage dans
   Couleur et taille de police : avant fixées en JS à chaque update(), maintenant
   ici en CSS (voir variables --remain-on/--remain-off dans :root) -> un seul
   endroit à modifier pour ajuster la taille ou les couleurs. */
.remain{font-size:1rem; text-align:center; margin:2px 0 0; min-height:0px;}
.remain.on{color:var(--remain-on);}
.remain.off{color:var(--remain-off);}

.foot{text-align:center; font-size:.6rem; color:var(--sub); padding:4px 0 0;}

/* 📶 Regroupe le % de signal WiFi et le bouton Infos système, pour qu'ils
   se déplacent ensemble comme un seul bloc dans l'en-tête (.top). */
.wifi-indicator{display:flex; align-items:center; gap:6px; flex-shrink:0;}
/* 🚨Taille et couleur du texte "xx%" à côté du bouton Infos -> font-size:.7rem */
.wifi-pct{font-size:.7rem; font-weight:700; color:var(--sub); min-width:2.6em; text-align:right;}
/* 🎨 Couleur selon la qualité du signal (mêmes seuils que la plupart des OS) */
.wifi-pct.good{color:#00FF00;}       /* >= 67% : bon signal (vert)*/
.wifi-pct.mid{color:#fbbf24;}          /* 34-66% : signal moyen  (jaune)*/
.wifi-pct.weak{color:#fb7185;}         /* < 34% : signal faible  (rouge)*/

/* 🚨BOUTON INFO 🛜  width:30px; height:30px  Largeur Hauteur */
.info-btn{
  width:35px; height:35px; border-radius:50%; border:1px solid var(--line);
  background:var(--card); color:var(--txt); font-size:.85rem; font-weight:800;
  display:flex; align-items:center; justify-content:center; cursor:pointer; flex-shrink:0;
}
.info-btn:active{transform:scale(.92);}

/* POPUP INFOS SYSTEME */
.modal-overlay{
  position:fixed; inset:0; background:rgba(0,0,0,.55); backdrop-filter:blur(2px);
  display:none; align-items:center; justify-content:center; padding:16px; z-index:50;
}
.modal-overlay.show{display:flex;}
.modal-box{
  width:100%; max-width:340px; border-radius:16px; background:var(--bg2);
  border:1px solid var(--line); padding:16px; box-shadow:0 12px 40px rgba(0,0,0,.5);
}
.modal-box h2{margin:0 0 10px; font-size:.95rem; font-weight:800; display:flex; align-items:center; gap:8px;}
.modal-row{
  display:flex; justify-content:space-between; align-items:center; gap:10px;
  padding:7px 0; border-bottom:1px solid var(--line); font-size:.78rem;
}
.modal-row:last-of-type{border-bottom:none;}
.modal-row span.lbl{color:var(--sub);}
.modal-row span.val{font-weight:700; text-align:right; word-break:break-all;}
.modal-close{
  margin-top:12px; width:100%; border:none; cursor:pointer; border-radius:9px; padding:9px;
  background:var(--accent,#6366f1); color:#0b0d12; font-size:.85rem; font-weight:800;
}
</style>
</head>
<body>
<div class="page">
  <div class="top">
    <div class="brand">
     <!-- <div class="brand-icon">⏱️</div> -->
      <div><h1>Programmation Horaire</h1></div>
    </div>
    <div class="wifi-indicator">
      <!-- 📶 Qualité du signal WiFi en %, mise à jour chaque seconde (voir update() plus bas) -->
      <button class="info-btn" onclick="openInfo()" title="Infos système">
<!-- 🚩 taille symbole 🛜 width="25" height="25" -->
        <svg viewBox="0 0 24 24" width="25" height="25" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round">
          <path d="M5 12.5a11 11 0 0 1 14 0"/>
          <path d="M8.3 16a6.5 6.5 0 0 1 7.4 0"/>
          <circle cx="12" cy="19.5" r="1.1" fill="currentColor" stroke="none"/>
        </svg>
      </button>
       <span class="wifi-pct" id="wifi-pct">--%</span>
    </div>
    <div class="clock"><span class="dot"></span><span id="time">--:--</span></div>
  </div>

  <div class="quick">
    <button class="qbtn on" onclick="allOn()"><span class="ic">🟢</span>Tout ON</button>
    <button class="qbtn off" onclick="allOff()"><span class="ic">🔴</span>Tout OFF</button>
    <button class="qbtn auto" onclick="allAuto()"><span class="ic">🔁</span>Tout AUTO</button>
  </div>

  <div id="rows"></div>

  <div class="foot"><h1>Appuyez sur une programmation pour la modifier et valider en appuyant sur ✓</h1></div>
</div>

<div class="modal-overlay" id="infoOverlay" onclick="if(event.target===this) closeInfo()">
  <div class="modal-box">
    <h2>📶 Infos système</h2>
    <div class="modal-row"><span class="lbl">WiFi</span><span class="val" id="info-wifi">---</span></div>
    <div class="modal-row"><span class="lbl">Box (SSID)</span><span class="val" id="info-ssid">---</span></div>
    <div class="modal-row"><span class="lbl">Nom (mDNS)</span><span class="val" id="info-host">---</span></div>
    <div class="modal-row"><span class="lbl">Adresse IP</span><span class="val" id="info-ip">---</span></div>
    <div class="modal-row"><span class="lbl">Adresse MAC</span><span class="val" id="info-mac">---</span></div>
    <div class="modal-row"><span class="lbl">Signal (RSSI)</span><span class="val" id="info-rssi">---</span></div>
    <button class="modal-close" onclick="closeInfo()">Fermer</button>
  </div>
</div>

<script>
// RELAYS est maintenant chargé dynamiquement depuis l'ESP32 (route /get-config)
// au lieu d'être codé en dur ici : la page s'adapte donc automatiquement au
// nombre de relais déclarés côté firmware (tableau programmateurs[]).
let RELAYS = [];
let lastData = null;

// Convertit une couleur hexadécimale ("#f59e0b") en triplet "r,g,b" pour les
// variables CSS --accent-rgb utilisées par les effets de survol/appui.
function hex2rgb(hex) {
  const h = hex.replace('#', '');
  const v = parseInt(h, 16);
  return `${(v >> 16) & 255},${(v >> 8) & 255},${v & 255}`;
}

async function initRelays() {
  const res = await fetch('/get-config');
  RELAYS = await res.json();

  const rowsEl = document.getElementById('rows');
  rowsEl.innerHTML = RELAYS.map(r => `
    <div class="row" id="${r.id}-row" style="--accent:${r.color}; --accent-rgb:${hex2rgb(r.color)};">
      <div class="row-top">
        <div class="name">${r.name}<small>${r.sub}</small></div>
        <span id="${r.id}relay-status" class="status-dot und">---</span>
        <div>
          <button type="button" class="mode-btn" id="${r.id}mode-btn" onclick="toggleMode('${r.id}')">---</button>
        </div>
      </div>
      <div class="row-body" id="${r.id}timer-ui">
        <div class="tfield"><label>Début</label><input type="time" id="${r.id}debut"></div>
        <div class="tfield"><label>Fin</label><input type="time" id="${r.id}fin"></div>
        <button class="savebtn" onclick="saveRelay('${r.id}')">✓</button>
      </div>
      <div class="row-body" id="${r.id}btn-ui" style="display:none;">
        <button class="forcebtn" onclick="fetch('/force-state?id=${r.id}')">⚡ Forcer ON ou OFF</button>
      </div>
      <p class="remain" id="${r.id}remain"></p>
      <p class="msg" id="${r.id}msg"></p>
    </div>
  `).join('');

  await update();
  setInterval(update, 1000);
}

// 🚩 Temps restant avant le prochain changement d'état --------
// Convertit une heure "HH:MM" en nombre de minutes depuis minuit.
function hmToMin(hm) {
  const [h, m] = hm.split(':').map(Number);
  return h * 60 + m;
}

// Formate un nombre de minutes en texte lisible ("1 h 23 min" ou "45 min").
function formatRemainMin(totalMin) {
  const h = Math.floor(totalMin / 60);
  const m = totalMin % 60;
  return h > 0 ? `${h} h ${String(m).padStart(2, '0')} min` : `${m} min`;
}

// Calcule le nombre de minutes avant le PROCHAIN changement d'état du
// relais (ON->OFF s'il est actuellement ON, ou OFF->ON s'il est OFF).
// S'appuie sur l'état "etat" déjà déterminé par l'ESP32 (qui gère lui-même
// les plages horaires à cheval sur minuit, ex: 22:00->06:00) : on n'a donc
// qu'à trouver la prochaine occurrence de l'heure de bascule concernée
// (aujourd'hui, ou demain si elle est déjà passée).
function remainingMinutes(d, nowMin) {
  const debutMin = hmToMin(d.debut);
  const finMin = hmToMin(d.fin);
  let targetMin = d.etat ? finMin : debutMin; // ON -> prochaine "fin" ; OFF -> prochain "debut"
  if (targetMin <= nowMin) targetMin += 1440; // déjà passée aujourd'hui -> c'est pour demain
  return targetMin - nowMin;
}

// 📶 Met à jour le badge "xx%" à côté du bouton Infos système, avec une
// couleur selon la qualité du signal. pct = -1 (ou absent) -> non connecté.
// Utilisée à la fois par update() (chaque seconde) et par le popup Infos.
function updateWifiPct(pct) {
  const el = document.getElementById('wifi-pct');
  if (!el) return;
  if (typeof pct !== 'number' || pct < 0) {
    el.textContent = '--%';
    el.className = 'wifi-pct';
    return;
  }
  el.textContent = pct + '%';
  el.className = 'wifi-pct ' + (pct >= 67 ? 'good' : pct >= 34 ? 'mid' : 'weak');
}

async function update() {
  try {
    const res = await fetch('/get-data');
    const data = await res.json();
    lastData = data;
    document.getElementById('time').innerText = data.actuelle;
    updateWifiPct(data.wifiPct);

    RELAYS.forEach(({ id: p }) => {
      const d = data[p];
      const modeBtn = document.getElementById(p + 'mode-btn');
      modeBtn.innerText = d.auto ? "AUTO" : "MANUEL";
      modeBtn.className = "mode-btn " + (d.auto ? "auto" : "manuel");
      document.getElementById(p + 'timer-ui').style.display = d.auto ? "flex" : "none";
      document.getElementById(p + 'btn-ui').style.display = d.auto ? "none" : "flex";

      const status = document.getElementById(p + 'relay-status');
      status.innerText = d.etat ? "ON" : "OFF";
      status.className = "status-dot " + (d.etat ? "on" : "off");

// 🚨⏳Temps restant avant le prochain changement d'état (uniquement en
// mode AUTO ; en mode MANUEL, le relais ne suit plus les horaires,
// donc l'info n'aurait pas de sens).
// 🎨 La couleur et la taille de police sont désormais définies en CSS
// (classes .remain.on / .remain.off, variables --remain-on / --remain-off
// dans :root) : ici on se contente de choisir la classe et le texte.
const remainEl = document.getElementById(p + 'remain');
if (d.auto && data.actuelle && data.actuelle !== '--:--') {
  const nowMin = hmToMin(data.actuelle);
  const remain = remainingMinutes(d, nowMin);

  remainEl.className = "remain " + (d.etat ? "on" : "off");
  remainEl.innerText = d.etat
    ? "⏰ Extinction dans " + formatRemainMin(remain)
    : "⏳ Allumage dans " + formatRemainMin(remain);
} else {
  remainEl.className = "remain";
  remainEl.innerText = "";
}
    });

    if (!window.loaded) {
      RELAYS.forEach(({ id: p }) => {
        document.getElementById(p + 'debut').value = data[p].debut;
        document.getElementById(p + 'fin').value = data[p].fin;
      });
      window.loaded = true;
    }
  } catch (e) { console.error("Erreur de synchronisation"); }
}

// 🚩 2️⃣Bascule AUTO/MANUEL : on attend la confirmation du serveur avant de
// resynchroniser l'affichage, pour éviter que le polling (update(), toutes
// les secondes) n'écrase le switch avec une valeur pas encore à jour et ne
// provoque un aller-retour visuel du curseur.
async function toggleMode(id) {
  await fetch('/toggle-mode?id=' + id);
  update(); // Rafraîchissement immédiat, sans attendre le prochain tick du setInterval
}

async function saveRelay(p) {
  const body = new FormData();
  body.append('debut', document.getElementById(p + 'debut').value);
  body.append('fin', document.getElementById(p + 'fin').value);
  const res = await fetch('/save?id=' + p, { method: 'POST', body: body });
  if (res.ok) {
    const msg = document.getElementById(p + 'msg');
    msg.innerText = "Sauvegardé ✓";
    setTimeout(() => msg.innerText = "", 2500);
  }
}

// --- Actions groupées (routes génériques, fonctionnent quel que soit le nombre de relais) ---
async function setAllState(desiredOn) {
  if (!lastData) return;
  for (const { id: p } of RELAYS) {
    const d = lastData[p];
    if (d.auto) await fetch('/toggle-mode?id=' + p);
    if (d.etat !== desiredOn) await fetch('/force-state?id=' + p);
  }
  update();
}
async function allOn() { setAllState(true); }
async function allOff() { setAllState(false); }
async function allAuto() {
  if (!lastData) return;
  for (const { id: p } of RELAYS) {
    if (!lastData[p].auto) await fetch('/toggle-mode?id=' + p);
  }
  update();
}

initRelays();

// --- Popup "Infos système" ---
async function openInfo() {
  const overlay = document.getElementById('infoOverlay');
  overlay.classList.add('show');
  try {
    const res = await fetch('/get-info');
    const info = await res.json();
    document.getElementById('info-wifi').innerText = info.connected ? "Connecté ✅" : "Déconnecté ❌";
    document.getElementById('info-ssid').innerText = info.ssid;
    document.getElementById('info-host').innerText = info.hostname;
    document.getElementById('info-ip').innerText = info.ip;
    document.getElementById('info-mac').innerText = info.mac;
    // 📶 dBm + % côte à côte, cohérent avec le badge de l'en-tête (voir updateWifiPct)
    document.getElementById('info-rssi').innerText =
      info.rssiPct >= 0 ? `${info.rssi} (${info.rssiPct}%)` : info.rssi;
  } catch (e) {
    document.getElementById('info-wifi').innerText = "Erreur";
  }
}
function closeInfo() {
  document.getElementById('infoOverlay').classList.remove('show');
}
</script>
</body>
</html>

)rawliteral";

// ============================================================================
//  SERVEUR WEB ET VARIABLES GLOBALES
// ============================================================================

AsyncWebServer server(80); // Instance du serveur web asynchrone, écoute sur le port 80 (HTTP standard)
Preferences preferences;   // Instance d'accès à la mémoire NVS (utilisée par saveSettings/loadSettings)

String now; // Heure courante au format "HH:MM", recalculée à chaque seconde dans loop()

// ============================================================================
//  SYSTÈME DE SAUVEGARDE / CHARGEMENT DES RÉGLAGES (Preferences / NVS)
// ============================================================================
//  La bibliothèque Preferences stocke des paires clé/valeur directement dans
//  la zone NVS (Non-Volatile Storage) de la mémoire flash de l'ESP32 — la
//  même zone qu'utilise en interne WiFi.begin() pour retenir les identifiants
//  réseau. Chaque groupe de réglages est rangé dans un "namespace" (ici
//  "config"), un peu comme un fichier .ini séparé. Les clés sont construites
//  dynamiquement à partir de l'id de chaque programmateur (ex: "1.debut"),
//  donc ce code fonctionne quel que soit le nombre de relais déclarés.
//  Note : NVS limite chaque clé à 15 caractères ; avec des id courts (type
//  "1.") et des suffixes courts ("debut","fin","auto",et"M"), on reste
//  largement en dessous même avec des id à deux chiffres.

// saveSettings() : écrit l'état actuel de tous les programmateurs dans la
// mémoire NVS, afin de le retrouver après un redémarrage ou une coupure de courant.
void saveSettings() {
  preferences.begin("config", false); // Ouvre le namespace "config" en lecture/écriture (false = read-write)

  for (int i = 0; i < NB_PROGRAMMATEURS; i++) {
    String id = programmateurs[i].id;
    preferences.putString((id + "debut").c_str(), programmateurs[i].heureDebut);
    preferences.putString((id + "fin").c_str(), programmateurs[i].heureFin);
    preferences.putBool((id + "auto").c_str(), programmateurs[i].modeAuto);
    preferences.putBool((id + "etM").c_str(), programmateurs[i].relayState);
  }

  preferences.end(); // Referme le namespace (valide et libère l'accès à la NVS)
  Serial.println("Paramètres sauvegardés dans la mémoire NVS");
}

// saveRelaySettings(p) : version CIBLÉE de saveSettings(), qui n'écrit en NVS
// que les 4 clés du relais concerné, au lieu de réécrire les clés des
// NB_PROGRAMMATEURS relais à chaque fois. 💾 Utile pour limiter l'utilisation de la
// mémoire flash (nombre de cycles d'écriture/effacement limité) : un simple
// appui sur un bouton poussoir ou un forçage ON/OFF n'a besoin de toucher
// qu'un seul relais, pas tout le tableau.
// Utilisée à la place de saveSettings() par toggle-mode, force-state, save
// et checkPhysicalButtons() — c'est-à-dire partout où UN SEUL relais change.
// saveSettings() reste disponible telle quelle si un jour une sauvegarde
// complète de tous les relais est nécessaire.
void saveRelaySettings(const Programmateur &p) {
  preferences.begin("config", false);
  String id = p.id;
  preferences.putString((id + "debut").c_str(), p.heureDebut);
  preferences.putString((id + "fin").c_str(), p.heureFin);
  preferences.putBool((id + "auto").c_str(), p.modeAuto);
  preferences.putBool((id + "etM").c_str(), p.relayState);
  preferences.end();
  Serial.printf("Paramètres de %s sauvegardés dans la mémoire NVS\n", p.id);
}

// loadSettings() : relit la mémoire NVS au démarrage pour restaurer les
// horaires, modes et états précédemment sauvegardés. Si une clé n'existe pas
// encore (premier démarrage, ou nouveau relais ajouté au tableau), la valeur
// par défaut définie dans programmateurs[] est conservée automatiquement.
void loadSettings() {
  preferences.begin("config", true); // Ouvre le namespace "config" en lecture seule (true = read-only)

  for (int i = 0; i < NB_PROGRAMMATEURS; i++) {
    String id = programmateurs[i].id;
    programmateurs[i].heureDebut = preferences.getString((id + "debut").c_str(), programmateurs[i].heureDebut);
    programmateurs[i].heureFin   = preferences.getString((id + "fin").c_str(), programmateurs[i].heureFin);
    programmateurs[i].modeAuto   = preferences.getBool((id + "auto").c_str(), programmateurs[i].modeAuto);
    programmateurs[i].relayState = preferences.getBool((id + "etM").c_str(), programmateurs[i].relayState);
  }

  preferences.end(); // Referme le namespace
  Serial.println("Paramètres chargés depuis la mémoire NVS");
}

// ============================================================================
//  RECHERCHE ET CONNEXION AU MEILLEUR RÉSEAU WIFI CONNU
// ============================================================================
//  Scanne tous les réseaux WiFi visibles, ne garde que ceux qui figurent
//  dans knownNetworks[] (donc ceux dont on a le mot de passe dans arduino_secrets.h),
//  et se connecte à celui qui a le meilleur signal (RSSI le plus proche de 0).
//  Renvoie true si la connexion a réussi, false sinon.
// Historique des échecs par réseau connu (indexé comme knownNetworks[]) :
// évite de retenter en boucle un réseau qui a le meilleur signal mais qui
// vient d'échouer (ex : box en panne/plantée mais qui continue quand même
// d'émettre son SSID). Le réseau est mis de côté pendant BLACKLIST_DURATION
// avant d'être de nouveau considéré comme candidat.
// 🚩4️⃣  Commutation Réseau
unsigned long lastFailTime[knownNetworksCount] = {0};
const unsigned long BLACKLIST_DURATION = 30000; // 30 s d'exclusion après un échec

// ----------------------------------------------------------------------------
//  🚩4️⃣ Commutation Réseau — VERSION NON BLOQUANTE
// ----------------------------------------------------------------------------
//  Le scan WiFi (WiFi.scanNetworks()) et l'attente de connexion (WiFi.begin()
//  puis boucle jusqu'à WL_CONNECTED) sont par défaut des opérations
//  BLOQUANTES de plusieurs secondes : tant qu'elles durent, tout le reste de
//  loop() est à l'arrêt (boutons poussoirs, écran OLED, réponses du serveur
//  web...). On utilise ici le mode de scan ASYNCHRONE de l'ESP32
//  (WiFi.scanNetworks(true)) : le scan démarre en tâche de fond et son
//  résultat est récupéré plus tard via WiFi.scanComplete(), sans jamais
//  attendre. La recherche + connexion est donc découpée en une petite
//  machine à états (WifiConnStep), avancée d'un cran à chaque appel de
//  pollBestNetworkConnect() — à appeler régulièrement (voir loop()) — au
//  lieu d'être exécutée d'un bloc du début à la fin.
//  (types WifiConnStep / WifiConnResult déclarés tout en haut du fichier,
//  voir la remarque juste après "struct WifiNetwork")
WifiConnStep wifiConnStep = WCS_IDLE;
int wifiConnTargetIndex = -1;        // Index (dans knownNetworks[]) du réseau auquel on tente de se connecter
unsigned long wifiConnStepStart = 0; // Instant (millis) du début de l'étape en cours (pour les timeouts)

// startBestNetworkConnect() : DÉMARRE une tentative de connexion (scan puis
// connexion au meilleur réseau connu trouvé). Ne bloque pas : revient
// immédiatement ; il faut ensuite appeler pollBestNetworkConnect() à chaque
// passage de loop() jusqu'à obtenir WCR_CONNECTED ou WCR_FAILED.
void startBestNetworkConnect() {
  // Force une déconnexion propre AVANT le scan. Indispensable : l'ESP32 a une
  // reconnexion automatique interne (activée par défaut) qui, si on ne la
  // coupe pas, continue de s'acharner en tâche de fond sur le dernier réseau
  // utilisé (même s'il ne répond plus) et entre en conflit avec le scan.
  WiFi.disconnect(true);
  Serial.println("Recherche des reseaux WiFi disponibles (async)...");
  WiFi.scanNetworks(true); // true = scan ASYNCHRONE : démarre en tâche de fond, ne bloque pas
  wifiConnStep = WCS_SCANNING;
  wifiConnStepStart = millis();
}

// pollBestNetworkConnect() : fait avancer d'UN CRAN, sans bloquer, une
// tentative démarrée par startBestNetworkConnect(). À appeler régulièrement
// (chaque passage de loop(), ou au moins chaque seconde) tant qu'elle est en
// cours (wifiConnStep != WCS_IDLE). Renvoie l'état courant.
WifiConnResult pollBestNetworkConnect() {
  if (wifiConnStep == WCS_SCANNING) {
    int n = WiFi.scanComplete(); // -1 = en cours, -2 = échec, >=0 = nb de réseaux trouvés
    if (n == WIFI_SCAN_RUNNING) return WCR_PENDING; // Toujours en cours : on repassera

    if (n == WIFI_SCAN_FAILED) {
      wifiConnStep = WCS_IDLE;
      return WCR_FAILED;
    }

    Serial.printf("%d reseau(x) detecte(s)\n", n);
    int bestIndex = -1;   // Index (dans knownNetworks[]) du meilleur réseau connu trouvé
    int bestRSSI = -1000; // Meilleur RSSI trouvé jusqu'ici (plus proche de 0 = meilleure réception)

    for (int i = 0; i < n; i++) {
      String foundSSID = WiFi.SSID(i);
      int foundRSSI = WiFi.RSSI(i);
      Serial.printf("  - %s (%d dBm)\n", foundSSID.c_str(), foundRSSI);

      // Ce réseau détecté fait-il partie de nos réseaux connus ?
      for (int k = 0; k < knownNetworksCount; k++) {
        // On ignore un réseau récemment en échec (mis en liste noire
        // temporaire), même si son signal est le meilleur : mieux vaut se
        // connecter à une box un peu moins bien captée mais qui fonctionne
        // réellement, plutôt que de retenter sans fin une box en panne.
        bool blacklisted = (lastFailTime[k] != 0 && millis() - lastFailTime[k] < BLACKLIST_DURATION);
        if (foundSSID == knownNetworks[k].ssid && foundRSSI > bestRSSI && !blacklisted) {
          bestRSSI = foundRSSI;
          bestIndex = k;
        }
      }
    }
    WiFi.scanDelete(); // Libère la mémoire utilisée par les résultats du scan

    if (bestIndex == -1) {
      Serial.println("Aucun reseau connu disponible (ou tous en liste noire temporaire).");
      wifiConnStep = WCS_IDLE;
      return WCR_FAILED;
    }

    Serial.printf("Connexion a '%s' (meilleur signal : %d dBm)...\n",
                  knownNetworks[bestIndex].ssid, bestRSSI);
    WiFi.begin(knownNetworks[bestIndex].ssid, knownNetworks[bestIndex].pass);
    wifiConnTargetIndex = bestIndex;
    wifiConnStepStart = millis();
    wifiConnStep = WCS_CONNECTING;
    return WCR_PENDING;
  }

  if (wifiConnStep == WCS_CONNECTING) {
    if (WiFi.status() == WL_CONNECTED) {
      lastFailTime[wifiConnTargetIndex] = 0; // Ce réseau fonctionne : on efface un éventuel historique d'échec
      wifiConnStep = WCS_IDLE;
      return WCR_CONNECTED;
    }

    // Délai maximum de 15 s pour ne pas rester bloqué indéfiniment sur ce réseau
    if (millis() - wifiConnStepStart >= 15000) {
      // Échec : on marque ce réseau comme temporairement suspect pour laisser
      // sa chance à un autre réseau connu au prochain essai (voir loop()).
      lastFailTime[wifiConnTargetIndex] = millis();
      wifiConnStep = WCS_IDLE;
      return WCR_FAILED;
    }
    return WCR_PENDING; // Toujours en cours de connexion, on repassera
  }

  return WCR_PENDING; // wifiConnStep == WCS_IDLE : rien en cours
}

// connectToBestNetwork() : version BLOQUANTE, gardée UNIQUEMENT pour le
// démarrage (setup()), où il n'y a de toute façon rien d'autre à faire tant
// que le WiFi n'est pas up — bloquer ici est donc sans conséquence. Elle
// s'appuie en interne sur les mêmes fonctions non bloquantes que loop() (pas
// de logique dupliquée) : elle démarre juste une tentative puis attend son
// résultat en la faisant avancer en boucle.
bool connectToBestNetwork() {
  startBestNetworkConnect();
  WifiConnResult result;
  do {
    delay(50); // Petite pause pour ne pas monopoliser le CPU en boucle serrée
    result = pollBestNetworkConnect();
  } while (result == WCR_PENDING);
  return result == WCR_CONNECTED;
}

// ============================================================================
//  RETOUR AUTOMATIQUE VERS LE MEILLEUR RÉSEAU QUAND IL REDEVIENT DISPONIBLE
// ============================================================================
//  connectToBestNetwork() n'est appelée que lorsqu'on est déconnecté : une
//  fois repliés sur un second réseau connu, on y restait donc pour toujours,
//  même si le réseau habituellement le plus fort redevenait disponible.
//  Cette fonction est appelée périodiquement (voir loop()) MÊME quand on est
//  déjà connecté, pour vérifier si un réseau connu avec un signal nettement
//  meilleur est réapparu, et basculer dessus si c'est le cas.
// 🚩4️⃣  Commutation Réseau
const unsigned long BEST_NETWORK_RECHECK_INTERVAL = 60000; // 60 s entre deux vérifications
const int RSSI_SWITCH_MARGIN = 8; // Marge (en dB) exigée avant de basculer, pour éviter les allers-retours (hystérésis)

// checkForBetterNetwork() : version NON BLOQUANTE. Gère elle-même son
// minuteur interne (plus besoin de le faire au point d'appel, voir loop()) :
// tant qu'aucune vérification n'est en cours, elle ne fait rien avant
// BEST_NETWORK_RECHECK_INTERVAL ; une fois ce délai écoulé, elle lance un
// scan ASYNCHRONE (WiFi.scanNetworks(true)) et revient immédiatement. Les
// appels suivants se contentent de vérifier si le résultat est prêt
// (WiFi.scanComplete()) jusqu'à ce qu'il le soit — sans jamais bloquer
// loop(). À appeler à chaque passage de loop() (ou au moins chaque seconde).
// (type BetterNetStep déclaré tout en haut du fichier, même raison que
//  WifiConnStep/WifiConnResult — voir la remarque après "struct WifiNetwork")
BetterNetStep betterNetStep = BNS_IDLE;

void checkForBetterNetwork() {
  static unsigned long lastCheckTime = 0;

  if (WiFi.status() != WL_CONNECTED) { betterNetStep = BNS_IDLE; return; } // Rien à faire si on n'est pas connecté (c'est déjà géré ailleurs)

  if (betterNetStep == BNS_IDLE) {
    if (millis() - lastCheckTime < BEST_NETWORK_RECHECK_INTERVAL) return; // Pas encore l'heure de revérifier
    lastCheckTime = millis();
    // Scan "à chaud" ASYNCHRONE (sans se déconnecter au préalable, contrairement
    // à startBestNetworkConnect()) : l'ESP32 gère seul la brève interruption que
    // cela implique, la connexion en cours n'est pas perdue.
    WiFi.scanNetworks(true);
    betterNetStep = BNS_SCANNING;
    return;
  }

  // BNS_SCANNING : un scan est en cours, on vérifie s'il est terminé
  int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING) return; // Toujours en cours, on repassera au prochain tick
  betterNetStep = BNS_IDLE;
  if (n == WIFI_SCAN_FAILED) return;

  String currentSSID = WiFi.SSID();
  int currentRSSI = WiFi.RSSI();
  int bestIndex = -1;
  int bestRSSI = -1000;

  for (int i = 0; i < n; i++) {
    String foundSSID = WiFi.SSID(i);
    int foundRSSI = WiFi.RSSI(i);
    for (int k = 0; k < knownNetworksCount; k++) {
      bool blacklisted = (lastFailTime[k] != 0 && millis() - lastFailTime[k] < BLACKLIST_DURATION);
      if (foundSSID == knownNetworks[k].ssid && foundRSSI > bestRSSI && !blacklisted) {
        bestRSSI = foundRSSI;
        bestIndex = k;
      }
    }
  }
  WiFi.scanDelete();

  // On ne bascule que si un AUTRE réseau connu a un signal nettement
  // meilleur (marge d'hystérésis) que le réseau actuel, pour éviter de
  // changer de réseau pour un écart de signal minime ou fluctuant.
  if (bestIndex != -1 &&
      String(knownNetworks[bestIndex].ssid) != currentSSID &&
      bestRSSI > currentRSSI + RSSI_SWITCH_MARGIN) {
    Serial.printf("Reseau '%s' (%d dBm) nettement meilleur que '%s' (%d dBm) : bascule...\n",
                  knownNetworks[bestIndex].ssid, bestRSSI, currentSSID.c_str(), currentRSSI);
    startBestNetworkConnect(); // Relance une connexion (non bloquante) vers ce meilleur réseau
  }
}

// ============================================================================
//  CONVERSION DE LA PUISSANCE DU SIGNAL WIFI (dBm) EN POURCENTAGE
// ============================================================================
//  Le RSSI (Received Signal Strength Indicator) fourni par WiFi.RSSI() est
//  exprimé en dBm, une échelle logarithmique négative (ex: -45 dBm = très bon
//  signal, -90 dBm = signal très faible), peu parlante pour un utilisateur.
//  On le convertit ici en pourcentage (0-100%) grâce au barème habituellement
//  utilisé par les systèmes d'exploitation (Windows, Android...) : -50 dBm ou
//  mieux = 100%, -100 dBm ou pire = 0%, et une relation linéaire entre les deux.
int rssiToPercent(int rssiDbm) {
  if (rssiDbm <= -100) return 0;
  if (rssiDbm >= -50) return 100;
  return 2 * (rssiDbm + 100); // Ex: -70 dBm -> 2*(30) = 60%
}

// ============================================================================
//  MISE À JOUR DE L'ÉCRAN OLED (adresse I2C 0x3C)
// ============================================================================
//  Affiche le réseau WiFi utilisé, l'adresse IP de l'ESP32, et pour chaque
//  relais : son mode (A=Auto / M=Manuel), son état ON/OFF, et sa plage
//  horaire programmée (uniquement en mode automatique).
//  L'écran étant petit (64 px de haut, ~5 lignes de relais visibles après
//  l'en-tête), l'affichage passe automatiquement en PAGES tournantes dès que
//  le nombre de relais dépasse OLED_LIGNES_PAR_PAGE : chaque page reste
//  affichée 4 secondes avant de passer à la suivante (voir loop()).

// Affiche une ligne d'état pour un relais donné.
void printRelayLine(const char* name, bool modeAuto, bool relayState,
                     const String &debut, const String &fin) {
  display.print(name);
  display.print(modeAuto ? " A " : " M ");     // A = Auto, M = Manuel

  if (relayState) {
    // Relais actif : "ON" affiché en vidéo inverse (fond blanc, texte noir)
    int16_t x = display.getCursorX();
    int16_t y = display.getCursorY();
    display.fillRect(x, y, 12, 8, SSD1306_WHITE);
    display.setTextColor(SSD1306_BLACK, SSD1306_WHITE);
    display.print("ON");
    display.setTextColor(SSD1306_WHITE);           // Remet la couleur normale pour la suite
    display.print("  ");                            // Padding pour aligner avec "OFF "
  } else {
    display.print("OFF ");
  }

  if (modeAuto) {
    // Plage horaire programmée, affichée seulement si elle est active (mode auto)
    display.print(debut);
    display.print("-");
    display.println(fin);
  } else {
    display.println(); // Mode manuel : pas d'horaire de programmation à afficher
  }
}

// page : numéro de page à afficher (0 = les OLED_LIGNES_PAR_PAGE premiers
// relais, 1 = les suivants, etc.). Recyclé automatiquement (modulo) selon le
// nombre total de pages nécessaires.
void updateOLED(int page) {
  if (!oledOK) return; // Écran non détecté au démarrage : on ne fait rien

  int nbPages = (NB_PROGRAMMATEURS + OLED_LIGNES_PAR_PAGE - 1) / OLED_LIGNES_PAR_PAGE;
  if (nbPages < 1) nbPages = 1;
  page = page % nbPages;

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);

  // Récupération de l'heure actuelle pour l'écran OLED
  struct tm timeinfo;
  char timeBuff[6]; // Assez grand pour stocker "HH:MM\0"
  if (getLocalTime(&timeinfo)) {
    strftime(timeBuff, sizeof(timeBuff), "%H:%M", &timeinfo);
  } else {
    sprintf(timeBuff, "--:--");
  }

  // Ligne 1 : nom du réseau WiFi actuellement utilisé et l'Heure courante 
  display.print(WiFi.status() == WL_CONNECTED ? WiFi.SSID() : "non connecte");
  display.print(" - "); // Espace de séparation
  display.println(timeBuff); // Affiche l'heure et passe à la ligne suivante

   // Ligne 2 : adresse IP locale 
      display.println(WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : "--");

  // Ligne 3 : indicateur de page si plusieurs pages sont nécessaires, sinon ligne vide ex: 1/2 2/2
  if (nbPages > 1) {
    display.print("Page ");
    display.print(page + 1);
    display.print("/");
    display.println(nbPages);
  } else {
    display.println();
  }

  // Lignes suivantes : un relais par ligne, pour la page courante uniquement
  int start = page * OLED_LIGNES_PAR_PAGE;
  int end = min(start + OLED_LIGNES_PAR_PAGE, NB_PROGRAMMATEURS);
  for (int i = start; i < end; i++) {
    printRelayLine(programmateurs[i].id, programmateurs[i].modeAuto, programmateurs[i].relayState,
                   programmateurs[i].heureDebut, programmateurs[i].heureFin);
  }

  display.display();
}

// ============================================================================
//  🔘 BOUTONS POUSSOIRS DE FORÇAGE PHYSIQUE (1. à 4. uniquement)
// ============================================================================
//  Câblage : GND --- Bouton Poussoir --- GPIO (broche déclarée dans "pinBP").
//  La broche est configurée en INPUT_PULLUP (voir setup()) : au repos elle
//  lit HIGH (tirée au +3.3V en interne), et lit LOW quand le bouton est
//  appuyé (il relie alors la broche au GND).
//
//  Cette fonction est appelée à CHAQUE passage de loop() (pas seulement une
//  fois par seconde) pour que l'appui soit détecté sans délai perceptible.
//  Elle applique un anti-rebond logiciel (debounce) : un changement de
//  lecture n'est pris en compte que s'il reste stable pendant BP_DEBOUNCE_MS.
//
//  Un appui détecté (passage stable à LOW) produit exactement le même effet
//  que le bouton "FORCER ON/OFF" de la page web : passage en mode manuel +
//  inversion de l'état du relais, puis sauvegarde en NVS.
void checkPhysicalButtons() {
  for (int i = 0; i < NB_PROGRAMMATEURS; i++) {
    Programmateur &p = programmateurs[i];
    if (p.pinBP < 0) continue; // Ce programmateur n'a pas de bouton poussoir câblé

    bool lecture = digitalRead(p.pinBP); // LOW = bouton appuyé (relié au GND)

    // Si la lecture brute vient de changer, on redémarre le chrono anti-rebond
    if (lecture != bpDernierEtatLu[i]) {
      bpDerniereBascule[i] = millis();
      bpDernierEtatLu[i] = lecture;
    }

    // La lecture est-elle stable depuis assez longtemps pour être validée ?
    if (millis() - bpDerniereBascule[i] > BP_DEBOUNCE_MS) {
      if (lecture != bpEtatStable[i]) {
        bpEtatStable[i] = lecture; // Nouvel état stable retenu

        if (bpEtatStable[i] == LOW) { // Front descendant = appui réellement détecté
          p.modeAuto = false;             // Passe en mode manuel (comme /force-state)
          p.relayState = !p.relayState;   // Inverse l'état du relais
          digitalWrite(p.pin, p.relayState ? HIGH : LOW); // Applique immédiatement sur la sortie physique
          saveRelaySettings(p);            // 💾 Sauvegarde ciblée : seul ce relais est réécrit en NVS
          Serial.printf("Bouton poussoir %s (GPIO%d) : forcage manuel -> %s\n",
                        p.id, p.pinBP, p.relayState ? "ON" : "OFF");
        }
      }
    }
  }
}

// ============================================================================
//  🔄MISE À JOUR DU FIRMWARE PAR WIFI (OTA)
// ============================================================================
//  Permet de reflasher le programme depuis l'IDE Arduino SANS câble USB, une
//  fois l'ESP32 installé dans son emplacement définitif (ex: boîtier
//  électrique). Configurée dans setup() (voir setupOTA()) et servie à chaque
//  passage de loop() via ArduinoOTA.handle(). Après un premier flash par USB
//  avec l'OTA actif, la carte apparaît ensuite comme un "port réseau" dans
//  Outils > Port de l'IDE Arduino, tant qu'elle reste sur le même réseau WiFi.
void setupOTA() {
  ArduinoOTA.setHostname(hostname); // Même nom que le mDNS web (ex: "richardv")

  // 🔒 Mot de passe OTA (fortement recommandé) : sans lui, n'importe quel
  // appareil du réseau WiFi pourrait reflasher l'ESP32. Définissez
  // SECRET_OTA_PASSWORD dans arduino_secrets.h pour l'activer, par exemple :
  //   #define SECRET_OTA_PASSWORD "votre_mot_de_passe"
#ifdef SECRET_OTA_PASSWORD
  ArduinoOTA.setPassword(SECRET_OTA_PASSWORD); // 🔒 Mot de passe OTA défini dans arduino_secrets.h
#else
  Serial.println("ATTENTION : OTA sans mot de passe (definissez SECRET_OTA_PASSWORD dans arduino_secrets.h pour le securiser)");
#endif

  // 🖥️ Callbacks : affichent la progression sur l'écran OLED (si présent) et sur
  // le moniteur série pendant le transfert du nouveau firmware.
  ArduinoOTA.onStart([]() {
    String type = (ArduinoOTA.getCommand() == U_FLASH) ? "programme" : "systeme de fichiers";
    Serial.println("Debut de la mise a jour (" + type + ")...");
    if (oledOK) {
      display.clearDisplay();
      display.setCursor(5, 12);  // Décale de 4 caractères vers la droite et 2 lignes vers le bas
      display.print("   Mise a jour...");
      display.setCursor(5, 28);
      display.print("  Ne pas eteindre !");
      display.display();
      delay(2500);
    }
  });

  //******* 🔄Modes OTA
    ArduinoOTA.onEnd([]() {
    Serial.println("\nMise a jour terminee, redemarrage...");
    delay(1500); // Delais entre 100% et message Mise a jour OK   
    const char* txtOk = "  Mise a jour OK...";    // --- Configuration des textes et calcul du centrage ---
    const char* txtRedem = "  Redemmarage... ";  
    int16_t x1, y1;
    uint16_t largTexte, hautTexte;
    display.setTextSize(1);   // Calcul automatique de la position X pour centrer "Redemmarage..."
    display.getTextBounds(txtRedem, 0, 0, &x1, &y1, &largTexte, &hautTexte);   
    int16_t posX_Redem = (SCREEN_WIDTH - largTexte) / 2; // Centrage horizontal au pixel près
    int16_t posY_Redem = 38;                             // Positionné environ 2 lignes en dessous de la ligne 12  
    for (int i = 0; i < 5; i++) {  // --- Boucle de clignotement (5 cycles d'alternance) ---
      display.clearDisplay();   // ÉTAPE A : "Mise a jour OK..." + "Redemmarage..." en Normal (Blanc sur Noir)
      display.setTextColor(SSD1306_WHITE, SSD1306_BLACK); // Ligne du haut (Fixe)
      display.setCursor(2, 12);
      display.print(txtOk);   
      display.setCursor(posX_Redem, posY_Redem);    // Ligne du bas (Normal)
      display.print(txtRedem);   
      display.display();
      delay(500); // Durée de l'état normal
      display.clearDisplay(); // ÉTAPE B : "Mise a jour OK..." + "Redemmarage..." en Vidéo Inversée (Noir sur Blanc)
      display.setTextColor(SSD1306_WHITE, SSD1306_BLACK);// Ligne du haut (Reste fixe en Normal)
      display.setCursor(2, 12);
      display.print(txtOk);
      display.setTextColor(SSD1306_BLACK, SSD1306_WHITE);// Ligne du bas (Bascule en Vidéo Inversée)
      display.setCursor(posX_Redem, posY_Redem);
      display.print(txtRedem);     
      display.display();
      delay(500); // Durée de l'état inversé
    }
    display.setTextColor(SSD1306_WHITE, SSD1306_BLACK); // Remet la couleur par défaut pour la suite du programme
  });

  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    unsigned int pct = (total > 0) ? (progress * 100 / total) : 0;
    Serial.printf("Progression OTA : %u%%\r", pct);
    if (oledOK) {
      display.clearDisplay();
      display.setCursor(12, 16);  // Décale de 2 caractères vers la droite et 2 lignes vers le bas
      display.printf("Mise a jour : %d%%", pct);
      display.drawRect(12, 32, 100, 10, 1); 
      //Dessine la barre de progression. La largeur du rectangle plein est égale à --> 'pct' (de 0 à 100 pixels)
      display.fillRect(12, 32, pct, 10, 1); 
      display.display();
    }
  });

  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("Erreur OTA [%u] : ", error);
    if (error == OTA_AUTH_ERROR) Serial.println("Authentification echouee");
    else if (error == OTA_BEGIN_ERROR) Serial.println("Echec au demarrage");
    else if (error == OTA_CONNECT_ERROR) Serial.println("Echec de connexion");
    else if (error == OTA_RECEIVE_ERROR) Serial.println("Echec de reception");
    else if (error == OTA_END_ERROR) Serial.println("Echec a la finalisation");
  });

  ArduinoOTA.begin();
  Serial.println("OTA pret : mise a jour possible via le WiFi depuis l'IDE Arduino");
}

// ============================================================================
//  INITIALISATION (exécutée une seule fois au démarrage de l'ESP32)
// ============================================================================
void setup() {
  Serial.begin(115200);          // Démarre la liaison série (pour le moniteur série, débit 115200 bauds)

  // On charge d'abord les réglages sauvegardés (horaires, modes, états),
  // AVANT de toucher aux broches. On connaît ainsi l'état voulu de chaque
  // relais avant même de configurer sa broche en sortie.
  loadSettings();

  // Configure la broche de chaque relais déclaré dans programmateurs[] en
  // sortie numérique. Astuce anti-glitch : on appelle digitalWrite() AVANT
  // pinMode(OUTPUT). Sur l'ESP32/Arduino, cela pré-charge le registre de
  // sortie avec le bon niveau, de sorte que dès que la broche bascule en
  // sortie, elle prend directement l'état voulu — sans repasser un court
  // instant par LOW par défaut (ce qui, avec un module relais "actif à
  // l'état bas" comme la plupart des modules bon marché à base de
  // SRD-05VDC, active brièvement le relais avant qu'il ne retombe).
  for (int i = 0; i < NB_PROGRAMMATEURS; i++) {
    digitalWrite(programmateurs[i].pin, programmateurs[i].relayState ? HIGH : LOW);
    pinMode(programmateurs[i].pin, OUTPUT);
  }

  // 🔘 Configure la broche du bouton poussoir de forçage physique (si déclarée)
  // en entrée avec résistance de tirage interne au +3.3V (INPUT_PULLUP) : le
  // bouton se contente donc de relier la broche au GND, sans résistance externe.
  // Initialise aussi les tableaux d'anti-rebond correspondants.
  for (int i = 0; i < NB_PROGRAMMATEURS; i++) {
    bpDernierEtatLu[i] = HIGH;
    bpEtatStable[i] = HIGH;
    bpDerniereBascule[i] = 0;
    if (programmateurs[i].pinBP >= 0) {
      pinMode(programmateurs[i].pinBP, INPUT_PULLUP);
    }
  }

  // Remarque : contrairement à LittleFS, la bibliothèque Preferences ne
  // nécessite pas d'initialisation globale ici — chaque appel à
  // preferences.begin()/end() (dans saveSettings/loadSettings) gère seul
  // son accès à la mémoire NVS.

  // Initialise le bus I2C (broches par défaut ESP32 : SDA=21, SCL=22) et l'écran OLED
  Wire.begin();
  if (!display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS)) {
    Serial.println("Ecran OLED non detecte a l'adresse 0x3C (verifier le cablage)");
    oledOK = false;
  } else {
    oledOK = true;
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 0);
    display.println("Demarrage...");
    display.display();
  }

  // (loadSettings() et l'application de l'état des relais ont été déplacés
  // plus haut, avant la configuration des broches — voir remarque ci-dessus)

  // Connexion WiFi : scanne les réseaux disponibles et se connecte à celui,
  // parmi les réseaux connus (arduino_secrets.h), qui offre le meilleur signal.
  WiFi.mode(WIFI_STA);          // Mode "station" (client), nécessaire avant le scan
  WiFi.setHostname(hostname);   // Nom affiché côté routeur/box
  // Désactive la reconnexion automatique interne de l'ESP32 : c'est notre
  // fonction connectToBestNetwork() qui doit seule décider à quel réseau se
  // connecter (sinon l'ESP32 s'acharne en interne sur le dernier réseau
  // utilisé même s'il ne répond plus, ce qui empêchait le repli automatique
  // vers un autre réseau connu).
  // 🚩4️⃣  Commutation Réseau
  WiFi.setAutoReconnect(false);

  if (!connectToBestNetwork()) {
    // Aucun réseau connu n'a pu être rejoint : on redémarre l'ESP32 pour
    // retenter proprement depuis le début plutôt que de rester bloqué.
    Serial.println("Echec de connexion WiFi, redemarrage...");
    if (oledOK) {
      display.clearDisplay();
      display.setCursor(0, 0);
      display.println("Pas de reseau WiFi");
      display.println("connu. Redemarrage...");
      display.display();
    }
    delay(3000);
    ESP.restart();
  }

  Serial.println("WiFi connected.");
  Serial.print("Reseau utilise : ");
  Serial.println(WiFi.SSID());
  Serial.print("IP address: ");
  // Affiche l'adresse IP locale attribuée à l'ESP32 (à utiliser dans le navigateur)
  Serial.println(WiFi.localIP());

  // Démarre le service mDNS : l'ESP32 devient joignable via http://richardv.local
  // en plus de son adresse IP (pratique si l'IP change au fil du temps).
  if (MDNS.begin(hostname)) {
    Serial.print("mDNS actif : http://");
    Serial.print(hostname);
    Serial.println(".local");
    MDNS.addService("http", "tcp", 80); // Annonce le service web sur le port 80
  } else {
    Serial.println("Erreur lors du demarrage du mDNS");
  }

  // 📡 Configure et démarre la mise à jour du firmware par WiFi (voir setupOTA()
  // plus haut). Placée après la connexion WiFi et le mDNS, dont l'OTA a besoin.
  setupOTA();

  updateOLED(0); // Première mise à jour de l'écran avec le réseau/IP obtenus

  // Synchronise l'horloge interne de l'ESP32 via NTP (serveurs de temps en ligne),
  // en appliquant le fuseau horaire français défini plus haut (TZ_INFO)
  configTzTime(TZ_INFO, "pool.ntp.org", "time.google.com");

 // 🚩3️⃣🚨 Attente active de la synchronisation de l'heure NTP
  Serial.print("Attente de la synchronisation NTP ");
  if (oledOK) {
    display.clearDisplay();
    display.setCursor(0, 0);
    display.println("Synchro heure ...");
    display.display();
  }

  struct tm timeTesting;
  int tentative = 0;
   // On met une limite à 20 tentatives (10 secondes) pour éviter de bloquer l'ESP32 si Internet est en panne
  while (!getLocalTime(&timeTesting) && tentative < 20) {
    delay(500);
    Serial.print(".");
    tentative++;
  }
  Serial.println("");

  if (tentative >= 20) {
    Serial.println("⏰ NTP Timeout : Demarrage sans heure valide (verifiez Internet)");
  } else {
    char afficheHeure[30];
    strftime(afficheHeure, sizeof(afficheHeure), "%H:%M:%S", &timeTesting);
    Serial.printf("⏰ Heure synchronisee avec succes : %s\n", afficheHeure);
  }

  updateOLED(0); // Fin de 3️⃣ Première mise à jour de l'écran avec le réseau/IP obtenus et la bonne heure

  // --------------------------------------------------------------------
  //  DÉFINITION DES ROUTES HTTP DU SERVEUR WEB
  //  Ces routes sont désormais GÉNÉRIQUES : une seule route par action,
  //  paramétrée par "id" (ex: /toggle-mode?id=Re3), au lieu d'une route
  //  dédiée par relais. Elles fonctionnent donc pour n'importe quel nombre
  //  de relais déclarés dans programmateurs[].
  // --------------------------------------------------------------------

  // Route "/" (GET) : sert la page HTML principale, directement depuis la
  // mémoire flash (PROGMEM), sans passer par un système de fichiers.
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(200, "text/html", index_html);
  });

  // Route "/get-config" (GET) : décrit à la page web la liste des relais à
  // afficher (id, nom, sous-titre, couleur). Appelée une fois au chargement
  // de la page, avant de construire les lignes de l'interface.
  server.on("/get-config", HTTP_GET, [](AsyncWebServerRequest *request) {
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    for (int i = 0; i < NB_PROGRAMMATEURS; i++) {
      JsonObject o = arr.add<JsonObject>();
      o["id"] = programmateurs[i].id;
      o["name"] = programmateurs[i].nom;
      o["sub"] = programmateurs[i].sousNom;
      o["color"] = programmateurs[i].couleur;
    }
    String json;
    serializeJson(doc, json);
    request->send(200, "application/json", json);
  });

  // Route "/get-data" (GET) : renvoie en JSON l'état complet de TOUS les
  // programmateurs (un objet imbriqué par id) + l'heure courante. Interrogée
  // chaque seconde par le JavaScript de la page (fonction update()).
  server.on("/get-data", HTTP_GET, [](AsyncWebServerRequest *request) {
    JsonDocument doc;

    for (int i = 0; i < NB_PROGRAMMATEURS; i++) {
      JsonObject o = doc[programmateurs[i].id].to<JsonObject>();
      o["debut"] = programmateurs[i].heureDebut;
      o["fin"] = programmateurs[i].heureFin;
      o["auto"] = programmateurs[i].modeAuto;
      o["etat"] = programmateurs[i].relayState;
    }

    // Récupère et formate l'heure courante (HH:MM) pour l'affichage
    struct tm timeinfo;
    char buff[10];
    if (getLocalTime(&timeinfo)) strftime(buff, sizeof(buff), "%H:%M", &timeinfo); // Heure valide : formatage
    else sprintf(buff, "--:--");                                                    // Heure non synchronisée : placeholder
    doc["actuelle"] = String(buff);

    // 📶 Qualité du signal WiFi en %, affichée à côté du bouton "Infos système"
    // (voir rssiToPercent() plus haut). -1 = non connecté, pour que le JS affiche "--".
    doc["wifiPct"] = (WiFi.status() == WL_CONNECTED) ? rssiToPercent(WiFi.RSSI()) : -1;

    String json;
    serializeJson(doc, json);                          // Convertit le document JSON en chaîne de caractères
    request->send(200, "application/json", json);      // Renvoie la réponse HTTP 200 avec le JSON
  });

  // Route "/get-info" (GET) : renvoie en JSON les informations système
  // (état WiFi, nom mDNS, IP, adresse MAC, puissance du signal). Utilisée
  // par le popup "?" affiché sur la page web.
  server.on("/get-info", HTTP_GET, [](AsyncWebServerRequest *request) {
    JsonDocument doc;

    bool connected = (WiFi.status() == WL_CONNECTED);
    doc["connected"] = connected;
    doc["ssid"] = connected ? WiFi.SSID() : "--"; // Nom du réseau WiFi actuellement connecté
    doc["hostname"] = String(hostname) + ".local";
    doc["ip"] = connected ? WiFi.localIP().toString() : "--";
    doc["mac"] = WiFi.macAddress(); // Adresse MAC de la carte ESP32 (toujours disponible)
    doc["rssi"] = connected ? (String(WiFi.RSSI()) + " dBm") : "--";
    doc["rssiPct"] = connected ? rssiToPercent(WiFi.RSSI()) : -1; // Même conversion que /get-data, pour le popup

    String json;
    serializeJson(doc, json);
    request->send(200, "application/json", json);
  });

  // Route "/toggle-mode?id=..." (GET) : bascule le programmateur désigné par
  // "id" entre mode automatique et mode manuel, puis sauvegarde le changement.
  server.on("/toggle-mode", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!request->hasParam("id")) { request->send(400, "text/plain", "id manquant"); return; }
    Programmateur* p = findProg(request->getParam("id")->value());
    if (!p) { request->send(404, "text/plain", "id inconnu"); return; }
    p->modeAuto = !p->modeAuto;  // Inverse l'état du mode
    saveRelaySettings(*p);        // 💾 Sauvegarde ciblée : seul ce relais est réécrit en NVS
    request->send(200, "text/plain", "OK");
  });

  // Route "/force-state?id=..." (GET) : appelée par le bouton "FORCER ON/OFF".
  // Bascule directement l'état du relais désigné et impose le mode manuel
  // (puisqu'on force manuellement l'état, on quitte le mode automatique).
  server.on("/force-state", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!request->hasParam("id")) { request->send(400, "text/plain", "id manquant"); return; }
    Programmateur* p = findProg(request->getParam("id")->value());
    if (!p) { request->send(404, "text/plain", "id inconnu"); return; }
    p->modeAuto = false;            // Passe en mode manuel
    p->relayState = !p->relayState; // Inverse l'état du relais (ON<->OFF)
    saveRelaySettings(*p);           // 💾 Sauvegarde ciblée : seul ce relais est réécrit en NVS
    request->send(200, "text/plain", "OK");
  });

  // Route "/save?id=..." (POST) : reçoit les nouveaux horaires saisis dans le
  // formulaire de la page web (champs "debut"/"fin") pour le programmateur
  // désigné par "id", et les enregistre.
  server.on("/save", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (!request->hasParam("id")) { request->send(400, "text/plain", "id manquant"); return; }
    Programmateur* p = findProg(request->getParam("id")->value());
    if (!p) { request->send(404, "text/plain", "id inconnu"); return; }

    if (request->hasParam("debut", true) && request->hasParam("fin", true)) {
      p->heureDebut = request->getParam("debut", true)->value(); // Récupère la valeur envoyée
      p->heureFin = request->getParam("fin", true)->value();
      saveRelaySettings(*p); // 💾 Sauvegarde ciblée : seul ce relais est réécrit en NVS
      request->send(200, "text/plain", "OK");
    } else {
      // Si un paramètre manque, on répond explicitement plutôt que de ne rien envoyer
      // (évite l'erreur "Handler did not handle the request")
      request->send(400, "text/plain", "Parametres manquants");
    }
  });

  // Route "attrape-tout" : répond proprement (404) à toute URL non reconnue
  // par les routes ci-dessus, plutôt que de laisser une réponse vide.
  server.onNotFound([](AsyncWebServerRequest *request) {
    request->send(404, "text/plain", "Not found");
  });

  server.begin(); // Démarre effectivement le serveur web (les routes deviennent actives)
}

// ============================================================================
//  BOUCLE PRINCIPALE (exécutée en continu après setup())
// ============================================================================
void loop() {

  // static : ces variables conservent leur valeur d'un passage à l'autre de loop()
  static unsigned long lastCheck = 0;
  static unsigned long lastPageChange = millis();
  static int pageOLED = 0;

  // 🔘 Vérifie les boutons poussoirs de forçage physique à CHAQUE passage de
  // loop() (et non une fois par seconde comme le reste) pour une réactivité
  // immédiate à l'appui, avec anti-rebond géré en interne.
  checkPhysicalButtons();

  // 📡 Traite les requêtes de mise à jour OTA en attente. Comme checkPhysicalButtons(),
  // appelée à CHAQUE passage de loop() (et non une fois par seconde) pour que
  // l'ESP32 réponde sans délai à une demande de flash depuis l'IDE Arduino.
  ArduinoOTA.handle(); // 🔄 Mode OTA

  // N'exécute le bloc ci-dessous qu'une fois par seconde (1000 ms), pour ne
  // pas surcharger inutilement le processeur (millis() ne bloque jamais,
  // contrairement à delay())
  if (millis() - lastCheck >= 1000) {
    lastCheck = millis(); // Mémorise l'instant de ce passage pour la prochaine comparaison

    // Surveillance de la connexion WiFi : si elle a été coupée (box redémarrée,
    // hors de portée...), on relance une recherche + connexion au meilleur
    // réseau connu disponible. Version NON BLOQUANTE (voir startBestNetworkConnect/
    // pollBestNetworkConnect plus haut) : on ne démarre une nouvelle tentative
    // que toutes les 10 s, et on fait avancer d'un cran une tentative déjà en
    // cours à chaque passage ici, sans jamais figer loop() en l'attendant.
    static unsigned long lastWifiRetry = 0;
    if (WiFi.status() != WL_CONNECTED) {
      if (wifiConnStep == WCS_IDLE) {
        if (millis() - lastWifiRetry >= 10000) {
          lastWifiRetry = millis();
          Serial.println("WiFi deconnecte, nouvelle recherche de reseau...");
          startBestNetworkConnect();
        }
      } else {
        WifiConnResult r = pollBestNetworkConnect();
        if (r == WCR_CONNECTED) {
          Serial.print("Reconnecte a : ");
          Serial.println(WiFi.SSID());
        }
        // WCR_PENDING : rien à faire, on continuera au prochain passage
        // WCR_FAILED  : rien à faire non plus, un nouvel essai sera tenté dans 10 s
      }
    }

    // Même si on est déjà connecté, on vérifie de temps en temps si le
    // réseau habituellement le meilleur est redevenu disponible, pour ne pas
    // rester bloqué indéfiniment sur un réseau de repli. checkForBetterNetwork()
    // gère maintenant elle-même son minuteur de 60 s et son scan asynchrone
    // (voir plus haut) : on l'appelle donc simplement à chaque tick.
    checkForBetterNetwork(); // 🚩4️⃣  Commutation Réseau

    // Récupère l'heure courante et la formate en "HH:MM" (comparable aux
    // horaires de début/fin stockés dans les mêmes chaînes "HH:MM")
    struct tm timeinfo;
    if (getLocalTime(&timeinfo)) {
      char nowStr[6];
      strftime(nowStr, sizeof(nowStr), "%H:%M", &timeinfo);
      now = String(nowStr);
    }

    // --- Logique de programmation : identique pour tous les relais déclarés,
    //     appliquée en boucle sur le tableau programmateurs[] ---
        bool heureValide = (now.length() == 5); // "HH:MM" : sinon NTP pas encore synchronisé

for (int i = 0; i < NB_PROGRAMMATEURS; i++) {
  Programmateur &p = programmateurs[i];
  
//***🚨⌚️ Programmation horaire
  if (p.modeAuto && heureValide) { // 🔧 On n'applique la logique horaire QUE si l'heure est fiable
    bool newState; // En mode automatique : le relais suit les horaires programmés
    // Cas normal : la plage ne traverse pas minuit (ex: 08:00 à 18:00)
    if (p.heureDebut < p.heureFin) newState = (now >= p.heureDebut && now < p.heureFin);
     // Cas où la plage traverse minuit (ex: 22:00 à 06:00) : condition inversée (OR au lieu de AND)
    else newState = (now >= p.heureDebut || now < p.heureFin);
         // Ne modifie la sortie physique que si l'état calculé a changé
        // (évite d'écrire inutilement sur la broche à chaque seconde)
    if (newState != p.relayState) {
      p.relayState = newState;
      digitalWrite(p.pin, p.relayState ? HIGH : LOW);
    }
  } else {
    // Mode manuel, OU mode auto mais heure pas encore synchronisée :
    // on se contente de réappliquer l'état mémorisé, sans le recalculer.
    digitalWrite(p.pin, p.relayState ? HIGH : LOW);
  }
}

    // Avance la page de l'écran OLED toutes les 8 secondes (utile seulement
    // si le nombre de relais dépasse OLED_LIGNES_PAR_PAGE, sinon updateOLED
    // affichera toujours la même page unique). avant
    if (millis() - lastPageChange >= 8000) { //⏰ Tempo pour basculement page suivante
      lastPageChange = millis();
      pageOLED++;
    }
    updateOLED(pageOLED); // Rafraîchit l'écran OLED avec le réseau/IP actuels et l'état des relais
  }
}
