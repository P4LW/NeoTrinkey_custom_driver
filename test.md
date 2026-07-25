# Piano e Guida di Test - NeoTrinkey Custom Driver & Multi-Device Suite 🧪

Questo documento contiene la batteria di test consigliata per verificare la correttezza, la reattività e la stabilità dell'intera pila software del progetto **NeoTrinkey Custom Driver**.

---

## 📋 Prerequisiti per l'Esecuzione dei Test

Prima di avviare i test, assicurati che il sistema sia configurato ed aggiornato:

```bash
# 1. Compila il modulo kernel e gli applicativi utenti
make all

# 2. Carica il modulo del kernel
sudo insmod NeoTrinkey_custom_driver.ko

# 3. Installa l'applicazione, il servizio Systemd e le regole Udev
sudo cp trinkey_app /usr/local/bin/ && sudo chmod +x /usr/local/bin/trinkey_app
sudo cp trinkeyctl /usr/local/bin/ && sudo chmod +x /usr/local/bin/trinkeyctl
sudo cp custom-trinkey@.service /etc/systemd/system/ && sudo systemctl daemon-reload
sudo cp 99-trinkey.rules /etc/udev/rules.d/ && sudo udevadm control --reload-rules && sudo udevadm trigger
```

---

## 🧪 Test 1: Verifica Diretta dell'Interfaccia Sysfs del Kernel

**Obiettivo**: Verificare che il driver `NeoTrinkey_custom_driver` riconosca la periferica HID ed esponga correttamente i nodi `sysfs`.

```bash
# 1. Trova la directory sysfs del dispositivo
ls -d /sys/bus/hid/devices/*239A*80FF*

# 2. Test di Scrittura LED (es. imposta colore Verde: "0 255 0")
echo "0 255 0" | sudo tee /sys/bus/hid/devices/*239A*80FF*/trinkey_led

# 3. Test di Lettura Touch (deve restituire 0 a riposo, 1 se premuto)
cat /sys/bus/hid/devices/*239A*80FF*/trinkey_touch
```

- **Esito Atteso**: Il LED si accende in verde; la lettura di `trinkey_touch` restituisce `0` (o `1` toccando il sensore).

---

## 🖥️ Test 2: Esecuzione Manuale di `trinkey_app` in Foreground

**Obiettivo**: Verificare il comportamento del daemon di controllo fuori dall'automazione Systemd.

```bash
# Avvia l'applicazione specificando il dispositivo sysfs
sudo /usr/local/bin/trinkey_app /sys/bus/hid/devices/*239A*80FF*
```

- **Esito Atteso**: 
  - Log di avvio: `Target device bound: /sys/bus/hid/devices/...`
  - Il LED mostra il colore di default (Rosso).
  - Toccando il pulsante capacitivo, il LED passa istantaneamente al Verde.
  - Premendo `Ctrl+C` l'applicazione spegne il LED ed esce in modo pulito.

---

## ⚙️ Test 3: Automazione Udev & Systemd Template Unit

**Obiettivo**: Verificare che il collegamento della chiavetta scateni l'avvio dell'istanza Systemd dedicata `custom-trinkey@<device>.service`.

1. Scollega e ricollega la chiavetta USB **Adafruit NeoKey Trinkey**.
2. Esegui i seguenti comandi:

```bash
# 1. Elenca le unità attive gestite dal servizio template
systemctl list-units "custom-trinkey@*"

# 2. Ispeziona i log in tempo reale del servizio
journalctl -u "custom-trinkey@*" -n 20 --no-pager
```

- **Esito Atteso**: L'unità `custom-trinkey@0003:239A:80FF.XXXX.service` risulta in stato `active (running)`. Il LED si accende automaticamente.

---

## 🎛️ Test 4: Controllo di Configurazione Dinamica via `trinkeyctl` e Segnali `SIGHUP`

**Obiettivo**: Verificare l'aggiornamento al volo dei colori ed effetti LED senza interrompere il daemon.

Mentre il servizio Systemd è attivo in background, esegui i comandi CLI:

```bash
# 1. Imposta modalità "breath" (dissolvenza Blu, tocco Bianco)
trinkeyctl -m breath -i "0 0 255" -t "255 255 255"

# 2. Imposta modalità "blink" (lampeggio Giallo)
trinkeyctl -m blink -i "255 255 0" -t "0 255 0"

# 3. Ripristina modalità "static" (Rosso fisso idle, Verde touch)
trinkeyctl -m static -i "255 0 0" -t "0 255 0"
```

- **Esito Atteso**: `trinkeyctl` stampa il messaggio `Notified X running daemon instance(s)`. L'effetto visivo sulla chiavetta cambia immediatamente.

---

## 🔌 Test 5: Gestione Disconnessione e Pulizia (Hot-Unplug)

**Obiettivo**: Verificare che lo scollegamento fisico della chiavetta arresti in modo pulito il servizio ed elimini i file temporanei.

1. Scollega la chiavetta USB durante l'esecuzione dell'animazione.
2. Esegui i controlli:

```bash
# 1. Controlla lo stato dei servizi
systemctl list-units "custom-trinkey@*"

# 2. Verifica la rimozione del file PID
ls -la /run/trinkey/
```

- **Esito Atteso**: Il servizio Systemd associato si arresta automaticamente. Non vi sono processi zombie né file PID residui in `/run/trinkey/`.

---

## 🔌🔌 Test 6: Concorrenza Multi-Dispositivo (Multi-Key)

**Obiettivo**: Verificare l'isolamento e la gestione indipendente di più schede NeoTrinkey collegate contemporaneamente.

1. Collega **due o più** schede NeoKey Trinkey a porte USB differenti.
2. Controlla le istanze attive:

```bash
systemctl list-units "custom-trinkey@*"
```

- **Esito Atteso**: Saranno presenti due istanze indipendenti (es. `custom-trinkey@...0001.service` e `custom-trinkey@...0002.service`). Entrambe rispondono indipendentemente al tocco e ai comandi `trinkeyctl`.

---

## 📊 Matrice di Riferimento degli Esiti

| ID Test | Ambito | Comando Chiave | Risultato Atteso |
| :--- | :--- | :--- | :--- |
| **Test 1** | Kernel Sysfs | `echo "0 255 0" > trinkey_led` | Cambio colore manuale via sysfs |
| **Test 2** | App Userspace | `sudo trinkey_app <sysfs_path>` | Avvio regolare, gestione tocco & segnali |
| **Test 3** | Udev/Systemd | `systemctl list-units "custom-trinkey@*"` | Auto-start istanza dinamica `%k` |
| **Test 4** | CLI & SIGHUP | `trinkeyctl -m breath -i "0 0 255"` | Aggiornamento al volo dell'effetto LED |
| **Test 5** | Hot-Unplug | Scollegamento fisico USB | Spegnimento pulito servizio e rimozione PID |
| **Test 6** | Multi-Device | Collegamento di 2+ dispositivi | Istanze in parallelo ed esecuzione isolata |
