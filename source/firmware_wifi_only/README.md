# Atari NetworkBox — firmware Wi-Fi seul

Firmware pour Raspberry Pi Pico 2 W. Il relie un Atari ST configure avec
STinG/SLIP a un reseau Wi-Fi via le montage RS-232 et MAX3232.

Cette edition est independante du W5500 : aucun module Ethernet, bouton ou
LED n'est requis.

## Premier demarrage

1. Flasher le fichier `.uf2` sur le Pico 2 W.
2. Depuis un telephone ou un PC, rejoindre le Wi-Fi `NetworkBox-Setup` avec le
   mot de passe `networkbox`.
3. Ouvrir `http://192.168.4.1`.
4. Choisir le Wi-Fi dans la liste (ou saisir un SSID masque), entrer le mot de
   passe puis enregistrer.
5. Le Pico redemarre et memorise les identifiants dans sa propre memoire flash.

Les identifiants ne sont pas inclus dans les sources ni dans le fichier UF2.

## Changer de reseau plus tard

Le bouton est facultatif. S'il est installe entre **GP4** et **GND**, un appui
maintenu trois secondes pendant que le Wi-Fi est connecte reouvre le portail
de configuration. Sans bouton, le portail s'ouvre automatiquement si le
reseau memorise ne peut plus etre rejoint.

## Atari ST

Configurer STinG en SLIP sur le port modem, a **19 200 bauds, 8N1** :

- Atari : `192.168.7.2`
- passerelle : `192.168.7.1`
- masque : `255.255.255.0`

La configuration a ete validee avec ping, DNS, HTTP/HighWire et FTP/Litchi.
