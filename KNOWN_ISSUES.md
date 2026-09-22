# Notes et limites connues

- Un Atari STF standard est limité à **19 200 bauds** sur son port modem.
  Les téléchargements FTP volumineux sont donc volontairement lents.
- Les modules W5500 ne sont pas tous identiques. Vérifiez si votre modèle
  accepte 5 V sur `VCC`; les signaux SPI du Pico restent toujours en 3,3 V.
- Le firmware RJ45 surveille le W5500 et tente une récupération automatique
  en cas de transmission bloquée. Si le module ne revient pas après cela,
  coupez puis remettez l'alimentation de la NetworkBox.
- Les fichiers `*.example` contiennent des chemins et une adresse DNS à
  adapter à votre installation Atari et votre routeur.
