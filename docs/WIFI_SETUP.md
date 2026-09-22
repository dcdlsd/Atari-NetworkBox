# Configuration Wi-Fi

## Français

Les identifiants Wi-Fi ne sont pas compilés dans les firmwares distribués.

1. Flashez un firmware contenant le Wi-Fi.
2. S'il n'a pas encore d'identifiants enregistrés, le Pico crée le réseau
   Wi-Fi `NetworkBox-Setup`.
3. Connectez un téléphone ou un PC à ce réseau avec le mot de passe
   `networkbox`.
4. Ouvrez `http://192.168.4.1` dans le navigateur.
5. Sélectionnez votre réseau, entrez son mot de passe puis sauvegardez.
6. La NetworkBox redémarre et rejoint votre Wi-Fi. Les identifiants sont
   mémorisés dans le Pico.

Sur le firmware unifié, le bouton est relié entre GP4 et GND. Un appui long
d'environ trois secondes, lorsque le Wi-Fi est sélectionné, ouvre à nouveau
le portail de configuration.

## English

No Wi-Fi credentials are compiled into release firmware. On first start, join
the `NetworkBox-Setup` access point (password: `networkbox`) and open
`http://192.168.4.1`. Select your network, enter its password and save. The
Pico restarts and stores the credentials.
