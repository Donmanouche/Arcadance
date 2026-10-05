# MidiPad 2

Réécriture en C++20 de Midipad, un convertisseur manette (DualSense) → MIDI.
La version Python de référence se trouve dans
`~/Desktop/Dropbox/Documents/Live coding project/Midipad-dev/`.

## État

| Étape | Contenu | État |
|---|---|---|
| 1 | Moteur (logique pure), accords, format de preset, import des anciens presets, tests | fait |
| 2 | Thread temps réel SDL3 (manette + touchpad) et RtMidi, exécutable sans interface | à faire |
| 3 | Éditeur de mapping et gestion des presets (Dear ImGui) | à faire |
| 4 | Écran performance animé | à faire |
| 5 | Paquets Windows et macOS | à faire |

## Compiler

Prérequis : CMake ≥ 3.24, Ninja, un compilateur C++20 (GCC 13 testé). Les
dépendances (nlohmann/json, doctest) sont téléchargées par CMake.

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

## Architecture

```
src/core/      bibliothèque sans dépendance système (testée)
  Engine       événements manette → messages MIDI ; aucune allocation à l'exécution
  Preset*      modèle de mapping, lecture/écriture JSON, import des anciens presets
  Chords/Notes théorie musicale
tools/         midipad-import
tests/         tests unitaires (doctest)
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
