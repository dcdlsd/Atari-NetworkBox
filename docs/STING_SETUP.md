# Configuration STinG / SLIP

## Français

L'Atari STF d'origine doit utiliser **19 200 bauds, 8N1**. Le MFP d'origine
ne génère pas correctement 38 400 bauds.

1. Installez STinG 1.26 et HSMODEM sur le disque Atari.
2. Copiez `atari_config_examples/DEFAULT.CFG.example` dans le dossier de
   STinG sous le nom `DEFAULT.CFG`.
3. Dans STinG Port Setup, configurez le port série avec :
   - pilote SLIP / SERIAL.STX ;
   - 19 200 bauds, 8N1 ;
   - Atari : `192.168.7.2` ;
   - passerelle Pico : `192.168.7.1` ;
   - masque : `255.255.255.0` ;
   - DNS : l'adresse de votre routeur (par exemple `192.168.1.254`).
4. Redémarrez l'Atari après toute modification STinG.
5. Testez d'abord un ping vers `192.168.7.1`, puis vers votre routeur.

Le fichier exemple utilise `MSS = 536`, `RCV_WND = 2144` et `DEF_RTT = 1500` :
des valeurs adaptées à une liaison série à 19 200 bauds.

### Mise à jour TCP.STX 1.40 (facultative)

TCP.STX 1.40 corrige notamment des problèmes de fermeture TCP et de dernier
paquet FTP/HTTP. Sauvegardez l'ancien `TCP.STX` avant de le remplacer, puis
redémarrez l'Atari.

## English

Use SLIP at **19,200 baud, 8N1**. Configure the Atari as `192.168.7.2`, Pico
gateway as `192.168.7.1`, netmask `255.255.255.0`, and DNS as your router IP.
Reboot the Atari after changes. Test the Pico gateway first, then the router.
