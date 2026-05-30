# NeoTrinkey_custom_driver
Project for the Advanced Operating Systems course



#TODO: modificare il modulo (?) in modo che romanga inserito anche dopo reboot del pc



-binario dell'applicazione che interagisce con il driver va compilato con -lm e spostato in usr/local/bin/test
-compilare trinkeyctl.c
-il binario trinkeyctl, che modifica il file di configurazione, va spostato in usr/local/bin


-spostare regola in /etc/udev/rules.d/99-trinkey.rules
-spostare servizio systemd in etc/systemd/system/custom-trinkey.service


-riavviare le regole e systemd:
  sudo udevadm control --reload-rules
  sudo systemctl daemon-reload
  sudo udevadm trigger

-creare il file di configurazione:
  sudo touch /etc/trinkey/config
  sudo chmod 664 /etc/trinkey/config


  
-eseguire i comandi seguenti:

Crea un gruppo di sistema chiamato 'trinkey'
  sudo groupadd -f trinkey

Aggiungi il TUO utente corrente a questo gruppo
  sudo usermod -aG trinkey $USER

Cambia il proprietario della cartella: root come utente, trinkey come gruppo
  sudo chown -R root:trinkey /etc/trinkey

Imposta i permessi corretti:
Cartella: root legge/scrive, gruppo trinkey legge/scrive/entra (775)
  sudo chmod 775 /etc/trinkey
  
File: root e gruppo trinkey possono leggere e scrivere (664)
  sudo chmod 664 /etc/trinkey/config

sudo chown -R "$USER":"$USER" /etc/trinkey

Imposta root come proprietario del binario
  sudo chown root:$USER /usr/local/bin/trinkeyctl
Attiva il bit SUID (la 's' permette l'elevazione temporanea solo per questo file)
  sudo chmod 4755 /usr/local/bin/trinkeyctl
