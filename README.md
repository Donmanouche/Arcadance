# MidiPad 2

Convertisseur manette (DualSense) → MIDI, écrit en C++20.

## État

| Étape | Contenu | État |
|---|---|---|
| 1 | Moteur (logique pure), accords, format de preset, tests | fait |
| 2 | Thread temps réel SDL3 (manette + touchpad) et RtMidi, exécutable sans interface | fait, validé sur DualSense |
| 3 | Interface (Dear ImGui) : vue animée, mapping, presets, options | fait |
| 4 | Enregistrement de boucles, humanisation | fait |
| 5 | Paquets Windows et macOS | à faire |

## Compiler

Prérequis : CMake ≥ 3.25, Ninja, un compilateur C++20 (GCC 13 testé). Les
dépendances (nlohmann/json, doctest, SDL3, RtMidi, Dear ImGui) sont téléchargées par CMake.
Sous Linux, il faut les en-têtes ALSA (`libasound2-dev`) ; JACK est optionnel.

```bash
cmake --preset release
cmake --build --preset release
ctest --preset release
```

## La fenêtre

```bash
build/release/src/app/midipad
```

Ouvre la fenêtre MidiPad (interface en anglais), une vue animée pensée pour la
scène, qui joue en même temps qu'elle édite. **F11** passe en plein écran,
**Ctrl+S** enregistre le preset, **Ctrl+Q** quitte.

- un module par partie de la manette : sticks gauche et droit, gâchettes,
  croix directionnelle, touchpad ; chacun a un écran animé qui suit la
  manette et des potentiomètres affichant les CC envoyés ;
- un bandeau de pads pour les boutons (✕ ○ □ △ aux couleurs PlayStation) :
  éclair à chaque appui, éclairage tant que la note ou l'accord sonne (les
  bascules verrouillées restent allumées) ;
- en haut : menu ☰, Panic, canal, tonalité, preset ;
- en bas : la sortie MIDI à gauche, l'état de la manette à droite ;
- un clic sur un contrôle (pad, direction, zone, potentiomètre d'axe, écran,
  panneau LOOP) ouvre ses réglages dans un panneau semi-transparent au-dessus
  de la vue. Il modifie le mapping du canal actif s'il en a un, sinon le
  mapping de base.

**Presets** : le menu déroulant PRESET (avec précédent / suivant) ouvre un
preset ; il propose aussi New…, Save as… et Move to trash… (le fichier est
déplacé dans le sous-dossier `trash`, jamais supprimé). SAVE apparaît quand le
preset est modifié. Un preset tient dans un seul fichier JSON, mappings par
canal compris.

**Key** (tonalité, facultative) : une fondamentale et une gamme (majeur, mineur
naturel, harmonique, mélodique, modes, pentatoniques, blues, par tons). Le
panneau de mapping ne propose alors que les notes de la gamme et, pour les
accords, un menu « Chord of the key » listant par degré tous les accords dont
les notes appartiennent à la gamme ; la saisie libre reste possible, et une
note ou un accord hors gamme est signalé. « None » : tout est proposé.

**Options** (menu ☰) : interface MIDI (avec le nom sous lequel chaque port est
vu, création des ports VirMIDI), délai de la croix directionnelle,
humanisation et dossier des presets.

Chaque modification s'entend tout de suite ; elle n'est écrite sur le disque
qu'à l'enregistrement. Les réglages (dossier des presets, dernier preset,
sortie, options) sont gardés dans `~/.local/share/MidiPad/MidiPad/settings.json`.
Le dossier des presets par défaut est `~/.local/share/MidiPad/MidiPad/presets`
(`midipad chemin/vers/un/preset.json` ouvre plutôt le dossier de ce preset).

### Ports VirMIDI (Linux)

Certains logiciels ne voient que les périphériques MIDI « bruts » du noyau
(`hw:`), pas les ports logiciels ALSA comme le port virtuel « MidiPad » (ni
Midi Through, rtpmidid…). Il faut alors passer par le module `snd-virmidi`.

MidiPad peut créer ces ports lui-même : menu ☰ › **Options** › MIDI INTERFACE ›
*VirMIDI ports*. On choisit le nombre de ports (1 à 4) et, si on le souhaite,
« Create them at every startup » ; le système demande alors le mot de passe
administrateur (via `pkexec`). Sans cette case, les ports disparaissent au
redémarrage ; avec, MidiPad écrit `/etc/modules-load.d/midipad-virmidi.conf` et
`/etc/modprobe.d/midipad-virmidi.conf`. « Remove » les supprime (il faut d'abord
fermer les logiciels qui les utilisent). À la main :
`sudo modprobe snd-virmidi midi_devs=4`.

Ensuite :

1. dans MidiPad, choisir la sortie `VirMIDI N-0` ;
2. dans le logiciel qui reçoit le MIDI, utiliser l'entrée `Virtual Raw MIDI/…`
   (`VirMIDI N-1` correspond à `Virtual Raw MIDI 2/…`, etc.).

Au démarrage, MidiPad se reconnecte au dernier port utilisé. S'il est
introuvable (ou au tout premier lancement), une fenêtre demande quel port
utiliser, en indiquant pour chaque port VirMIDI le nom sous lequel il est vu,
et propose de créer les ports VirMIDI s'il n'y en a aucun ; le port virtuel sert
en attendant. Le numéro de carte N change selon l'ordre de branchement ; MidiPad
retrouve son port VirMIDI malgré ce changement. Sous macOS, le port virtuel
« MidiPad » sera visible directement ; sous Windows, il faudra loopMIDI.

La manette reste lue quand la fenêtre MidiPad est en arrière-plan.

On n'appuie et on ne relâche jamais exactement en même temps les deux boutons
d'une diagonale de la croix : une nouvelle direction n'est donc jouée qu'après
être restée stable un court instant (15 ms par défaut, à l'appui comme au
relâchement, réglable dans Options), pour que la direction traversée entre
les deux ne sonne pas. Relâcher toute la croix coupe la note immédiatement.

## Jouer sans fenêtre

```bash
build/release/src/app/midipad --headless presets/PS5_Test.json
```

Crée un port MIDI virtuel « MidiPad » (ALSA) à relier au DAW, et lit la
première manette connectée (branchée avant ou après le lancement). Le preset
est rechargé dès que son fichier est enregistré ; s'il est invalide, l'ancien
reste actif et l'erreur est affichée.

| Option | Rôle |
|---|---|
| `-c NOM` | envoyer vers un port existant (ex. `-c "OXI ONE"`) au lieu du port virtuel |
| `-n NOM` | nom du port virtuel (ex. `-n "MidiPad Control"`) |
| `--channel N` | canal de départ (1-16) |
| `-m` | moniteur : affiche chaque message envoyé |
| `-i` | affiche aussi les boutons et les changements de freeze |
| `--stats` | temps de traitement mesurés, affichés à la sortie |
| `--list-ports` | liste les ports MIDI de sortie |

Le touchpad de la DualSense passe par `/dev/hidraw*` : la règle udev
donnant accès à la DualSense (par exemple `/etc/udev/rules.d/70-dualsense.rules`)
est nécessaire (elle est déjà installée sur cette machine). Sans elle, la manette fonctionne
mais sans touchpad, et midipad le signale à la connexion.

### Latence

Un thread dédié lit l'état de la manette 1000 fois par seconde (`--poll-hz`),
en priorité temps réel quand le système l'autorise, et envoie le MIDI
directement. Mesuré sur cette machine avec une DualSense, sur une session de
18 minutes : 21 µs de traitement par lecture en moyenne (0,5 ms au pire),
réveil en retard de 1,6 ms au pire, aucune erreur d'envoi.

Les événements manette de SDL sont volontairement désactivés : la boucle
d'événements du thread principal dessine l'interface, et l'affichage ne doit
pas retarder le MIDI. Pour la même raison, la manette ne pilote pas
l'interface (clavier et souris uniquement).

## Architecture

```
src/app/       midipad : lecture SDL3 (GamepadInput), sortie RtMidi (MidiOutput),
               thread temps réel (Runtime), mode console (Headless)
  gui/         fenêtre ImGui (GuiApp), vue animée (PerformanceView) et
               panneau de mapping (MappingEditor)
src/core/      bibliothèque sans dépendance système (testée)
  Engine       événements manette → messages MIDI ; aucune allocation à l'exécution
  Preset*      modèle de mapping, lecture/écriture JSON
  Chords/Notes théorie musicale
  PresetLibrary, AppSettings, Labels   dossier de presets, réglages, libellés
assets/fonts/  Roboto Bold et Roboto Condensed Bold (licence Apache 2.0), embarquées
tests/         tests unitaires (doctest), dont GamepadInput face à une
               manette virtuelle SDL
```

Les contrôles portent les noms de l'API gamepad de SDL3 (`south`, `left_x`,
`up_right`…), et non plus des numéros dépendant du pilote.

## Comportements à connaître

- **Accords** : `M` majuscule signifie majeur (`CM7` = Cmaj7, et non Cm7) ;
  `C6/9` est reconnu. L'octave de la fondamentale se règle dans le panneau de mapping
  (champ `octave` du preset, 4 par défaut : C4 = note MIDI 60) ; un accord
  dont une note sortirait de la plage MIDI est refusé. « Bass root » double la
  fondamentale une ou deux octaves plus bas pour asseoir la basse (champ
  `root_below` : 1 ou 2).
- **Boucles (Loop record)** : tant qu'un bouton « Loop record » est maintenu,
  MidiPad enregistre ce qui bouge : un stick (au-delà d'environ 12 % de
  course), un doigt sur le touchpad ou les gâchettes (leur course, pas les
  notes qu'elles jouent comme boutons), plusieurs à la fois si besoin ; au relâchement, chaque mouvement X/Y tourne en boucle,
  comme un LFO, sur le canal où il a été enregistré. Les moments où un doigt
  était levé sont enregistrés aussi et ne jouent rien. La boucle joue à côté
  du stick, du doigt ou des gâchettes, qui restent libres : elle a ses propres
  CC (70/71 et 72/73 pour les sticks, 74/75 et 76/77 pour les doigts, 78/79
  pour L2/R2 par défaut) et son mode de lecture (avant, arrière, aller-retour),
  réglés dans la fenêtre du stick, du touchpad ou des gâchettes (champ `loops`
  du preset). Le module TRIGGERS montre les CC et les notes de L2 et R2, et un
  écran LOOP avec leurs deux courbes. En mode 4 pads, un doigt
  enregistre les pads qu'il joue : la boucle les rejoue tels qu'ils sont
  mappés (notes, accords, CC, momentanés ou en bascule), et coupe ce qui sonne
  encore quand elle repart du début. Le module TOUCHPAD montre les CC (ou les
  pads) de chaque doigt et un écran LOOP où les gestes enregistrés sont tracés
  (une ligne par pad pour les boucles de pads), avec une frise des moments où
  chaque doigt touchait.
  Chaque canal a ses deux boucles (gauche et droite) : on peut enregistrer
  sur le canal 1, passer au canal 2 et enregistrer encore ; toutes jouent en
  même temps. Un appui sans bouger aucun stick arrête les boucles du canal
  actif ; Panic arrête celles de tous les canaux. Les panneaux LOOP de la vue
  animée montrent les boucles du canal actif et leur tête de lecture. Les
  boucles ne sont pas gardées d'une session à l'autre.
- **Freeze** : gèle l'axe à sa valeur courante. Dans la vue animée, la fleur
  reste bleu glacé et suit le stick ; une silhouette et un réticule, bleus
  pour le stick gauche, jaunes pour le droit, marquent l'endroit du gel
  jusqu'au dégel, pour y revenir avant de dégeler.
- **Inversion d'axe** (`axis_invert`) : inverse le sens du CC envoyé.
- **Gâchettes L2/R2** : utilisables à la fois comme axe (CC) et comme bouton.
- **Transport** : `stop` et `continue` fonctionnent aussi sur les boutons.
- **Notes collées** : une note est toujours éteinte sur le canal où elle a
  démarré, même après un changement de canal ou de preset en cours de jeu.
- **Mapping par canal** : un changement de canal en jeu (bouton `channel`)
  bascule aussi sur le mapping de ce canal.
- **Vélocité** : le champ `velocity` est respecté partout.
- **Axes** : un CC n'est renvoyé que si sa valeur change.
- **Humanisation** : dans Options, on choisit les canaux concernés, une plage
  de vélocité (en % de la vélocité du mapping, 100 % par défaut) et une plage
  de retard (0 à 500 ms). Chaque note, y compris chaque note d'un accord, tire
  sa vélocité et son retard au hasard dans ces plages ; son relâchement est
  retardé d'autant, si bien que sa durée ne change pas. Une affectation peut
  s'en passer (case « Humanize » du mapping, champ `"humanize": false` du
  preset). Panic coupe tout immédiatement, notes en attente comprises.
- **Toggle exclusif** : les notes et accords en mode toggle s'excluent au sein
  d'une même famille de contrôles (boutons, croix directionnelle, chaque doigt
  du touchpad). Les CC en toggle sont toujours indépendants.
- **Touchpad en mode axe** : CC uniquement.
- **Seuil des gâchettes** : en mode bouton, appui à 25 % de la course,
  relâchement à 15 %.
