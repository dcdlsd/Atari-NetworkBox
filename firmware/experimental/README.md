# Experimental 38,400 baud firmware / Firmware expérimental 38 400 bauds

This Pico 2 W UF2 supports **both Wi-Fi and W5500 Ethernet**. It is based on
the combined firmware, with the Atari-facing UART speed changed from 19,200
to **38,400 baud (8N1)**. This is **not a validated release**: it did not work
in a first test on the author's Atari STF. It is offered only for users with
other Atari serial hardware who wish to experiment. Set the Atari SLIP port
to 38,400 baud as well. No automatic baud-rate detection is included.

Ce fichier UF2 pour Pico 2 W gère **le Wi-Fi et le RJ45/W5500**. Seule la
vitesse de la liaison série avec l'Atari passe de 19 200 à **38 400 bauds
(8N1)**. **Ce n'est pas une version validée** : elle n'a pas fonctionné lors
d'un premier essai sur l'Atari STF de l'auteur. Elle est destinée uniquement
aux essais sur d'autres configurations série Atari. Réglez aussi le port SLIP
de l'Atari à 38 400 bauds. La vitesse n'est pas détectée automatiquement.

The standard, tested combined firmware remains
[`../Atari_NetworkBox_Pico2W_WIFI_and_RJ45_v0.6.5.uf2`](../Atari_NetworkBox_Pico2W_WIFI_and_RJ45_v0.6.5.uf2)
at 19,200 baud. Flash it again and restore the Atari's 19,200-baud setting
to return to the known-working setup. The experimental source and build file
are in [`../../source/firmware_unified_38400_test/`](../../source/firmware_unified_38400_test/).
