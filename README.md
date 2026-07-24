# NeoTrinkey_custom_driver
Project for the Advanced Operating Systems course



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


-per gestire i permessi eseguire i comandi seguenti:

sudo groupadd -f trinkey
sudo usermod -aG trinkey $USER
sudo chown -R root:trinkey /etc/trinkey
sudo chmod 775 /etc/trinkey
sudo chmod 664 /etc/trinkey/config
sudo chown root:trinkey /usr/local/bin/trinkeyctl
sudo chmod 4755 /usr/local/bin/trinkeyctl
