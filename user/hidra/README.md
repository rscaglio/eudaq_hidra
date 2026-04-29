# HiDRA2

Prerequisiti e build

- Requisiti esterni principali:
  - **CAENVMELib** >= 4.1.3
  - **CAENComm** >= 1.8.0
  Queste librerie sono fornite da CAEN: installarle seguendo le istruzioni del fornitore
  (mettere le librerie e gli header in percorsi visibili dal linker e dal compilatore).

- Installazione rapida (consigliata — utilizza gli helper locali):

```sh
# dalla root del repository (es: eudaq_hidra)
source user/hidra/misc/setup.sh
# configura cmake (usa i preset HiDRA se disponibili)
cmake_config
# build e install (funzione che chiama la build completa)
build_hidra
```

Se il sistema non supporta i CMake Presets richiesti, lo script fornisce un fallback
esegue internamente i comandi `cmake -S <root> -B build` e `cmake --build build -j 10`.

- VSCode: per creare i preset locali (non committati) usare:

```sh
source user/hidra/misc/setup.sh
setup_vscode_hidra
```

Nota: la comunicazione con strumenti Scope può richiedere VISA:
```sh
sudo apt install ni-visa ni-visa-devel
```

Breve descrizione della struttura `user/hidra` (cartelle principali)

- `dc/`        : codice per `HidraDataCollector` (collector)
- `dry/`       : producer "dry" per test/simulazione (HidraDryFERSProducer, HidraDryXDCProducer)
- `fers/`      : driver e librerie per FERS (hardware specifico)
- `misc/`      : script e preset utili (es. `setup.sh`, `CMakePresets.hidra.json`)
- `rc/`        : RunControl (HidraRunControl)
- `run/`       : script per avviare il sistema (es. `hidra_startrun.sh`, `hidra_startrun_dry.sh`)
- `xdc/`       : produttori/decoder XDC

La parte principale per far funzionare il software dopo la compilazione è la cartella `user/hidra/run`.
Gli script contenuti lanciano i binari appena installati (in `bin/`) e sono il modo più semplice
per avviare una run di prova, per esempio:

```sh
# avvio "dry" (no hardware)
user/hidra/run/hidra_startrun_dry.sh
# o la run completa
user/hidra/run/hidra_startrun.sh
```

Per maggiori dettagli sugli helper disponibili vedere `user/hidra/misc/setup.sh`.

## Git workflow procedure

- Everyone works on its own branch
- When a new feature is ready it needs to be merged to master with a Pull Request:
   - Commit any relevant changes
   - `git switch master` and `git pull` to update local master branch
   - `git switch <your branch>` and `git rebase master` to include changes on master on your own branch
   - Solve any merge conflict and finally `git push` to push your changes to your remote branch
   - In the web browser go to pull request, select master as target branch and your branch as source and create the pull request
   - When request is ready to merge -----> merge it.

## Tricks

- List processes listening to a port: `lsof -i :44000`

## Data format

### XDC 2025 event format

Decoder in [DRCalo/DreamDaqMon](https://github.com/DRCalo/DreamDaqMon)

TB data in [cernbox](https://cernbox.cern.ch/s/H6yDF4TNRez6jsw), or `/eos/user/i/ideadr/TB2025_H8`.

- **Event header**: 14 words, 32 bits each: (in brackets fixed expected values)
 ```
 eventMarker (0xccaaffee) | eventNumber | spillNumber |
 headerSize (0xe) | trailerSize (0x1) |  dataSize | eventSize (=header+trailer+data) |
 eventTimeSec | eventTimeMicrosec |
 triggerMask (0x1 or 0x2) | isPedMask | isPedFromScaler |
 sanityFlag | headerEndMarker (0xaccadead)
 ```
- **VME modules data** CAEN data format (QDCs V792, V792N; TDCs V775, V775N)
  - For each module: 1 header word + $n_{chan}$ data words + 1 trailer. 32 bits each word

- **Event trailer**: 1 word, 32 bits
 ```
 eventTrailer (0xbbeeddaa)
 ```








