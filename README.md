# MidiPad 2

Réécriture en C++20 de Midipad, un convertisseur manette (DualSense) → MIDI.
La version Python de référence se trouve dans
`~/Desktop/Dropbox/Documents/Live coding project/Midipad-dev/`.

## État

| Étape | Contenu | État |
|---|---|---|
| 1 | Moteur (logique pure), accords, format de preset, import des anciens presets, tests | fait |
| 2 | Thread temps réel SDL3 (manette + touchpad) et RtMidi, exécutable sans interface | fait, validé sur DualSense |
| 3 | Éditeur de mapping et gestion des presets (Dear ImGui) | fait, à tester |
| 4 | Écran performance animé | à faire |
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

## Convertir les presets de la version Python

```bash
build/release/tools/midipad-import "<chemin>/Midipad-dev/configs" presets
```

Chaque `<nom>.json` et ses fichiers `<nom>-<canal>.json` deviennent un seul
preset ; les entrées non convertibles sont signalées.

## L'éditeur

```bash
build/release/src/app/midipad
```

Ouvre la fenêtre MidiPad, qui joue en même temps qu'elle édite :

- **Preset** : ouvrir, créer, enregistrer (Ctrl+S), enregistrer sous, annuler
  les modifications, mettre à la corbeille (le fichier est déplacé dans le
  sous-dossier `corbeille`, jamais supprimé), importer les presets de la
  version Python, changer de dossier de presets.
- **Sortie MIDI** : port virtuel « MidiPad » ou un port existant ; canal actif ;
  « Tout couper » pour les notes collées.
- **Tonalité** (facultative) : une fondamentale et une gamme (majeur, mineur
  naturel, harmonique, mélodique, modes, pentatoniques, blues, par tons).
  L'éditeur ne propose alors que les notes de la gamme et, pour les accords,
  un menu « Accord de la gamme » listant par degré tous les accords dont les
  notes appartiennent à la gamme ; la saisie libre reste possible, et une note
  ou un accord hors gamme est signalé. « Aucune » : tout est proposé.
- **Mapping** : la base ou le mapping spécifique d'un canal. « Suivre le canal
  actif » affiche ce qui joue sur le canal en cours, y compris quand il change
  depuis la manette. Quatre onglets : boutons, croix directionnelle, sticks et
  gâchettes, touchpad. Les contrôles touchés sur la manette s'allument en vert.
- **Journal** : connexions, moniteur MIDI, boutons appuyés.

Chaque modification s'entend tout de suite ; elle n'est écrite sur le disque
qu'à l'enregistrement. Les réglages (dossier des presets, dernier preset,
sortie) sont gardés dans `~/.local/share/MidiPad/MidiPad/settings.json`.
Le dossier des presets par défaut est `~/.local/share/MidiPad/MidiPad/presets`
(`midipad chemin/vers/un/preset.json` ouvre plutôt le dossier de ce preset).

### Bitwig sous Linux

Bitwig ne voit que les périphériques MIDI « bruts » du noyau (`hw:`), pas les
ports logiciels ALSA comme le port virtuel « MidiPad » (ni Midi Through,
rtpmidid…). Il faut donc passer par le module `snd-virmidi` :

1. dans MidiPad, choisir la sortie `VirMIDI N-0` ;
2. dans Bitwig, utiliser l'entrée `Virtual Raw MIDI/…` (`VirMIDI N-1`
   correspond à `Virtual Raw MIDI 2/…`, etc.).

Au démarrage, MidiPad se reconnecte au dernier port utilisé, comme la version
Python. S'il est introuvable (ou au tout premier lancement), une fenêtre demande
quel port utiliser, en indiquant pour chaque port VirMIDI son nom dans Bitwig ;
le port virtuel sert en attendant. Le numéro de carte N change selon l'ordre de
branchement ; MidiPad retrouve son port VirMIDI malgré ce changement. Sous macOS, le port virtuel « MidiPad »
apparaîtra directement dans Bitwig ; sous Windows, il faudra loopMIDI.

La manette reste lue quand la fenêtre MidiPad est en arrière-plan.

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
| `-n NOM` | nom du port virtuel (ex. `-n "Bitwig Control"` comme la version Python) |
| `--channel N` | canal de départ (1-16) |
| `-m` | moniteur : affiche chaque message envoyé |
| `-i` | affiche aussi les boutons et les changements de freeze |
| `--stats` | temps de traitement mesurés, affichés à la sortie |
| `--list-ports` | liste les ports MIDI de sortie |

Le touchpad de la DualSense passe par `/dev/hidraw*` : la règle udev
`/etc/udev/rules.d/70-dualsense.rules` de la version Python est nécessaire
(elle est déjà installée sur cette machine). Sans elle, la manette fonctionne
mais sans touchpad, et midipad le signale à la connexion.

### Latence

Un thread dédié lit l'état de la manette 1000 fois par seconde (`--poll-hz`),
en priorité temps réel quand le système l'autorise, et envoie le MIDI
directement. Mesuré sur cette machine avec une DualSense, sur une session de
18 minutes : 21 µs de traitement par lecture en moyenne (0,5 ms au pire),
réveil en retard de 1,6 ms au pire, aucune erreur d'envoi. La version Python attendait
jusqu'à ~15 ms (sommeil de 5 ms + attente de 10 ms) avant de traiter un
événement.

Les événements manette de SDL sont volontairement désactivés : la boucle
d'événements du thread principal dessine l'interface, et l'affichage ne doit
pas retarder le MIDI. Pour la même raison, la manette ne pilote pas
l'interface (clavier et souris uniquement).

## Architecture

```
src/app/       midipad : lecture SDL3 (GamepadInput), sortie RtMidi (MidiOutput),
               thread temps réel (Runtime), mode console (Headless)
  gui/         fenêtre ImGui (GuiApp) et éditeur de mapping (MappingEditor)
src/core/      bibliothèque sans dépendance système (testée)
  Engine       événements manette → messages MIDI ; aucune allocation à l'exécution
  Preset*      modèle de mapping, lecture/écriture JSON, import des anciens presets
  Chords/Notes théorie musicale
  PresetLibrary, AppSettings, Labels   dossier de presets, réglages, libellés
tools/         midipad-import
tests/         tests unitaires (doctest), dont GamepadInput face à une
               manette virtuelle SDL
```

Les contrôles portent les noms de l'API gamepad de SDL3 (`south`, `left_x`,
`up_right`…), et non plus des numéros dépendant du pilote.

## Différences de comportement avec la version Python

Volontaires. Ce sont des corrections :

- **Accords** : les racines E et B ne sont plus jouées un demi-ton trop bas ;
  `M` majuscule signifie majeur (`CM7` = Cmaj7, et non Cm7) ; `C6/9` est
  reconnu. Aucun preset existant n'utilise ces accords.
- **Freeze** : gèle l'axe à sa valeur courante, et non au centre.
- **Inversion d'axe** (`axis_invert`) : elle est appliquée.
- **Gâchettes L2/R2** : utilisables à la fois comme axe (CC) et comme bouton.
- **Transport** : `stop` et `continue` fonctionnent aussi sur les boutons.
- **Notes collées** : une note est toujours éteinte sur le canal où elle a
  démarré, même après un changement de canal ou de preset en cours de jeu.
- **Mapping par canal** : un changement de canal en jeu (bouton `channel`)
  bascule aussi sur le mapping de ce canal.
- **Vélocité** : le champ `velocity` est respecté partout.
- **Axes** : un CC n'est renvoyé que si sa valeur change.

Simplifications à valider :

- **Toggle exclusif** : les notes et accords en mode toggle s'excluent au sein
  d'une même famille de contrôles (boutons, croix directionnelle, chaque doigt
  du touchpad). Les CC en toggle sont toujours indépendants ; en Python, ceux
  de la croix s'excluaient entre eux.
- **Touchpad en mode axe** : CC uniquement.
- **Seuil des gâchettes** : en mode bouton, appui à 25 % de la course,
  relâchement à 15 %.
