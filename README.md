# NeoTrinkey_custom_driver
Project for the Advanced Operating Systems course



#TODO: modificare il modulo (?) in modo che romanga inserito anche dopo reboot del pc



-binario dell'applicazione che interagisce con il driver va compilato con -lm e spostato in usr/local/bin/test


-creare regola in /etc/udev/rules.d/99-trinkey.rules

ACTION=="add", SUBSYSTEM=="usb", \
  ATTR{idVendor}=="239a", ATTR{idProduct}=="80ff", \
  RUN+="/usr/bin/systemd-run --no-block /bin/systemctl start custom-trinkey.service"

ACTION=="remove", SUBSYSTEM=="usb", \
  ENV{ID_VENDOR_ID}=="239a", ENV{ID_MODEL_ID}=="80ff", \
  RUN+="/usr/bin/systemd-run --no-block /bin/systemctl stop custom-trinkey.service"



-creare servizio systemd in etc/systemd/system/custom-trinkey.service

[Unit]
Description=NeoTrinkey Monitor

[Service]
Type=simple
ExecStart=/usr/local/bin/trinkey_app
Restart=on-failure
RestartSec=2
# Disabilita il rate limiter così non si blocca dopo N crash rapidi
StartLimitBurst=0


-riavviare le regole e systemd:
  sudo udevadm control --reload-rules
  sudo systemctl daemon-reload
  sudo udevadm trigger

-creare il file di configurazione:
  sudo touch /etc/trinkey/config
  sudo chmod 664 /etc/trinkey/config


-compilare il file trinkeyctl.c e aggiungerlo in usr/local/bin
-eseguire i comandi seguenti:

# 1. Crea un gruppo di sistema chiamato 'trinkey'
sudo groupadd -f trinkey

# 2. Aggiungi il TUO utente corrente a questo gruppo
sudo usermod -aG trinkey $USER

# 3. Cambia il proprietario della cartella: root come utente, trinkey come gruppo
sudo chown -R root:trinkey /etc/trinkey

# 4. Imposta i permessi corretti:
# Cartella: root legge/scrive, gruppo trinkey legge/scrive/entra (775)
sudo chmod 775 /etc/trinkey
# File: root e gruppo trinkey possono leggere e scrivere (664)
sudo chmod 664 /etc/trinkey/config

sudo chown -R "$USER":"$USER" /etc/trinkey

# Imposta root come proprietario del binario
sudo chown root:$USER /usr/local/bin/trinkeyctl
# Attiva il bit SUID (la 's' permette l'elevazione temporanea solo per questo file)
sudo chmod 4755 /usr/local/bin/trinkeyctl
