# RETROVERSE
## Specifica completa del prodotto, dell'architettura e dell'implementazione

**Versione:** 1.0 — Specifica iniziale di riferimento
**Data:** 14 ottobre 2026
**Piattaforma target:** Windows 10/11, x86-64
**Tecnologie fondamentali:** C++20, OpenGL, CMake
**Tipologia:** applicazione desktop nativa, frontend alternativo a EmulationStation
**Stato:** specifica progettuale; nessuna implementazione è implicita o dichiarata completata

---

# 1. Visione del prodotto

Retroverse è un **mondo tridimensionale esplorabile che costituisce l'interfaccia per una collezione personale di videogiochi emulati**. Invece di elenchi e caroselli, l'utente attraversa stanze generate proceduralmente, osserva scaffali di cartucce e custodie, prende un oggetto, lo esamina e avvia il relativo videogioco. Console, televisori CRT, schermi LCD, postazioni arcade, arredi e oggetti da collezione conferiscono all'esperienza l'aspetto di un «paradiso del retrogamer».

Il software sostituisce **nell'uso quotidiano** l'interfaccia di EmulationStation su Windows, ma **non** sostituisce né reimplementa gli emulatori. Legge gli stessi file, le stesse ROM e, per quanto consentito dai formati e dalle configurazioni disponibili, gli stessi metadati e media. L'utente non deve duplicare la libreria né eseguire una seconda catalogazione obbligatoria.

L'esperienza comprende due modalità entrambe primarie:

1. **Esplorazione immersiva** — entrare in una sala, osservare gli oggetti, ascoltare l'audio dei televisori, prendere una cartuccia e utilizzarla con la console.
2. **Launcher rapido** — cercare un titolo, filtrare la collezione e avviarlo in pochi passaggi, senza attraversare le stanze o attendere animazioni.

## 1.1 Principi non negoziabili

- **Nessun game engine:** niente Godot, Unity o Unreal. Renderer, scena, simulazione procedurale e interazione sono sviluppati direttamente in C++ e OpenGL, usando librerie di supporto mirate.
- **Unica collezione:** le ROM restano dove si trovano; le immagini, i video e i metadati già presenti vengono utilizzati in-place.
- **Compatibilità non distruttiva:** di default nessuna riscrittura dei file originari di EmulationStation/ES-DE. Le modifiche condivise sono esplicite, verificate e reversibili.
- **Proceduralità reale:** stanze, dimensioni, collegamenti, scaffali, postazioni e disposizione dei giochi sono calcolati dalla composizione della libreria.
- **Persistenza stabile:** nuove ROM non devono rimescolare tutta la collezione o cancellare le modifiche manuali dell'utente.
- **Video integrati nel mondo:** i televisori mostrano anteprime realmente decodificate e renderizzate come texture su superfici 3D.
- **Controller-first:** il software deve essere completamente utilizzabile da un divano con un gamepad, pur supportando mouse e tastiera.
- **Funzionamento offline:** dopo l'import iniziale, le funzionalità principali non dipendono dalla rete.
- **Prestazioni controllate:** anche librerie grandi non implicano migliaia di video in decodifica o di mesh individualmente renderizzate.
- **Architettura estendibile:** l'implementazione Windows precede eventuali adapter Linux, RetroBat e Batocera.

## 1.2 Esempio d'uso canonico

L'utente dispone di una collezione ES-DE con giochi SNES, N64, PlayStation, PlayStation 2, GBA e arcade. Retroverse rileva il percorso dati ES-DE, importa le informazioni, dimensiona automaticamente un hub e alcune stanze, inserisce gli oggetti fisici negli scaffali e colloca televisori vicino alle console. Uno dei CRT riproduce clip di gameplay dei giochi presenti nella sala. Quando l'utente seleziona un titolo, il CRT mostra il video corrispondente, mentre una scheda riporta titolo, anno, sviluppatore e altri dettagli. Se sceglie **Gioca**, Retroverse minimizza/sospende la propria scena, avvia l'emulatore configurato e al suo termine ripristina la stanza e la posizione della camera. Dopo l'aggiunta di ROM, i nuovi giochi compaiono in slot disponibili; i precedenti rimangono al loro posto.

---

# 2. Ambito, obiettivi e confini

## 2.1 Ambito della prima versione completa

- Applicazione nativa **Windows x64**.
- Input da tastiera, mouse e controller XInput/SDL GameController.
- Renderer 3D OpenGL con asset glTF e geometria modulare.
- Import e aggiornamento della libreria **ES-DE**; supporto a configurazioni legacy attraverso un adapter esplicito.
- Generazione automatica di stanze e oggetti a partire dal catalogo reale.
- Launcher per emulatori Windows esterni.
- Anteprime video sulle superfici 3D, con audio opzionale e politiche di risparmio risorse.
- Scraper configurabile per metadati e media, nel rispetto dei provider.
- Ricerca globale, teletrasporto, cronologia, preferiti, statistiche, impostazioni e gestione dei problemi.
- Salvataggio delle personalizzazioni del mondo, senza alterare la libreria di origine.
- Distribuzione con installer e variante portable.

## 2.2 Fuori ambito iniziale

- Emulazione proprietaria di console, BIOS e hardware.
- Inclusione o redistribuzione di ROM, BIOS, manuali e asset di terzi senza diritti.
- Compatibilità completa con tutti i fork EmulationStation come risultato automatico della compatibilità ES-DE.
- Riproduzione dei videogiochi *all'interno* del televisore virtuale (emulazione embedded): possibile funzione futura, non requisito iniziale.
- Multiplayer, cloud obbligatorio, visite remote e social network. (La VR non è più fuori ambito: ADR-0017, iterazione
  I10.)
- Un editor di modellazione 3D general-purpose.
- Un motore di fisica complesso: inizialmente sono sufficienti collisioni e interazioni contestuali.

## 2.3 Compatibilità: terminologia precisa

«EmulationStation» può indicare prodotti distinti. L'implementazione primaria sarà per **ES-DE / EmulationStation Desktop Edition su Windows**. L'EmulationStation originale (legacy), RetroBat e Batocera saranno **profili di origine distinti**, perché possono avere percorsi, naming dei sistemi, struttura dei media e semantica di avvio differenti. Non si deve affermare un'equivalenza automatica fra di essi.

La documentazione ES-DE corrente, usata come riferimento di partenza, è quella della linea 3.x; al momento della specifica il sito ufficiale riporta **ES-DE 3.5.0, pubblicato il 30 settembre 2026**. Compatibilità e comportamento effettivi verranno verificati con fixture e installazioni reali di ciascuna versione supportata.

---

# 3. Stack tecnologico e dipendenze

## 3.1 Decisioni tecniche

| Area | Scelta principale | Ruolo | Motivazione |
|---|---|---|---|
| Linguaggio | **C++20** | Tutta la logica applicativa | Controllo delle risorse e portabilità |
| Rendering | **OpenGL 4.5**, baseline compatibile 3.3 Core | GPU, shader, framebuffer, texture | Gestione diretta della pipeline grafica |
| Windowing/input | **SDL3** | Finestre, eventi, gamepad, finestra OpenGL | Un unico strato per sistemi operativi e input |
| Caricamento OpenGL | **glad** | Entry point delle funzioni GL | Nessuna dipendenza da estensioni manuali |
| Matematica | **GLM** | Vettori, matrici, quaternioni, trasformazioni | Convenzioni familiari OpenGL |
| Asset 3D | **fastgltf** (preferito) | Import glTF 2.0 / GLB | Asset pipeline moderna; alternativa Assimp se serve multi-formato |
| Immagini | **stb_image** + libreria di encoding dedicata | PNG/JPEG e texture | Semplicità; i formati aggiuntivi sono opzionali |
| Video | **FFmpeg libavformat/libavcodec/libswscale** | MP4/H.264, altri contenitori/codecs disponibili | Decodifica indipendente dagli emulatori |
| Audio | **miniaudio** | Output, mixer, audio spaziale semplice | Leggero e integrabile |
| UI runtime | **RmlUi** | Interfaccia utente scalabile e navigabile | Separazione fra UI di prodotto e mondo 3D |
| UI debug/editor | **Dear ImGui** (build developer) | Inspector e diagnostica | Non è la UI finale del frontend |
| Parsing XML | **pugixml** | Gamelist e configurazioni ES-DE | Facile da integrare e preservare dove possibile |
| JSON | **nlohmann/json** | Manifest asset, layout, import/export | Convenienza e leggibilità |
| Configurazioni | **toml++** | Config utente e profili | Schema comprensibile per un utente avanzato |
| Database | **SQLite3** | Indice, identità, job, layout, statistiche | Storage embedded affidabile |
| HTTP/download | **libcurl** | Scraping e scaricamento media | TLS, resume, timeout, redirect controllati |
| ECS opzionale | **EnTT** | Entità/componenti della scena | Evita di reinventare registry ed eventi |
| Logging | **spdlog** | Log e diagnostica | Performance, rotazione log |
| File watching | **ReadDirectoryChangesW** / astrazione | Aggiornamenti incrementali | Supporto Windows senza scansioni continue |
| Build | **CMake + Ninja + vcpkg** | Compilazione, dependency pinning | Build riproducibili |
| Test | **Catch2** o **GoogleTest** | Unit/integration test | Casi di compatibilità automatizzati |
| Profiling | **Tracy** e RenderDoc | CPU/GPU e debug frame | Individuazione colli di bottiglia |

**Vincolo:** l'elenco è una *proposta implementativa*, non un impegno a incorporare tutte le librerie prima dell'uso. In particolare SDL3, RmlUi, FFmpeg, fastgltf e le loro versioni dovranno essere bloccati in manifest e validati insieme in un progetto di integrazione.

## 3.2 Profilo grafico

- **Target raccomandato:** OpenGL 4.5 Core Profile, framebuffer sRGB, instancing, UBO/SSBO dove disponibili, asynchronous texture upload quando utile.
- **Baseline opzionale:** OpenGL 3.3 Core con percorsi alternativi per funzioni non disponibili; non dichiarare supporto finché non testato.
- **Renderer proposto:** forward+ semplificato o forward clustered in fase avanzata; partire da forward renderer modulare con instancing e luci dinamiche limitate.
- **Materiali:** workflow PBR metallico/ruvidità per gli asset glTF, con fallback non-PBR per hardware modesto.
- **Post-process:** tonemapping, bloom leggero e shader CRT opzionali; nessun effetto è indispensabile per usare il frontend.
- **VSync, FPS cap, risoluzione dinamica:** configurabili.

## 3.3 Asset fisici

Le console, gli chassis delle cartucce, le custodie, i CRT, i mobili e gli accessori saranno **modelli 3D riutilizzabili** (preferibilmente glTF/GLB). Il generatore costruisce gli *spazi e le disposizioni*, non inventa automaticamente un modello accurato di ogni singola console a partire dal nulla.

Modelli generici devono essere sempre disponibili come fallback per piattaforme prive di asset dedicati. La provenienza, attribuzione e licenza degli asset inclusi vanno registrate.

---

# 4. Compatibilità con ES-DE e unicità dei dati

## 4.1 Regola della fonte di verità

1. **ROM:** directory della libreria originale, mai copiate automaticamente.
2. **Metadati condivisi:** `gamelist.xml` di ES-DE (più eventuali altre fonti dichiarate dal profilo).
3. **Media condivisi:** `downloaded_media` di ES-DE oppure directory configurata dall'utente.
4. **Definizioni di sistema e lancio:** `es_systems.xml` e override personalizzati *quando accessibili*, più eventuali regole interne o preferenze che richiedono adapter dedicati.
5. **Stato esclusivo Retroverse:** database locale che contiene world seed, posizioni, preferenze visuali, job, cache e sessioni di gioco.

SQLite **non** deve diventare una seconda libreria autorevole: è un indice ricostruibile, tranne che per lo stato che appartiene esclusivamente a Retroverse.

## 4.2 Rilevamento ES-DE su Windows

Il wizard cerca e propone, senza assumere che esistano sempre:

- dati utente in `%USERPROFILE%\ES-DE\` per le installazioni standard;
- struttura dati nella cartella portable dell'installazione ES-DE;
- radici personalizzate e l'eventuale parametro `--home`;
- configurazione `settings/es_settings.xml`;
- `gamelists/<system>/gamelist.xml`;
- `downloaded_media/<system>/<tipo>/...` oppure il percorso definito in `MediaDirectory`;
- cartelle ROM definite dalla configurazione attiva (`ROMDirectory` se presente, o impostazione risolta dal profilo).

Il rilevamento è **assistito e confermato**: si mostrano percorsi individuati e numero di sistemi/giochi, senza sovrascriverli.

## 4.3 Formati dei metadati

Esempio illustrativo di record ES-DE:

```xml
<?xml version="1.0" encoding="UTF-8"?>
<gameList>
  <game>
    <path>./Super Mario World.sfc</path>
    <name>Super Mario World</name>
    <desc>Platform bidimensionale.</desc>
    <releasedate>19901121T000000</releasedate>
    <developer>Nintendo EAD</developer>
    <publisher>Nintendo</publisher>
    <genre>Platform</genre>
    <players>1-2</players>
    <favorite>true</favorite>
  </game>
</gameList>
```

Requisiti del parser:

- caricare `<game>` e `<folder>`, senza distruggere dati o nodi ignoti;
- riconoscere i campi standard, mantenendo anche gli elementi non compresi;
- risolvere i percorsi relativi, compresi `./`, sottodirectory e percorsi assoluti validi;
- gestire correttamente XML escaping, Unicode e caratteri speciali nei nomi file;
- accettare metadati incompleti e creare un fallback a partire dal nome ROM;
- normalizzare internamente date, rating, booleani e liste senza cambiare i valori originali;
- usare un parser robusto con limiti ragionevoli alla dimensione dei file e divieto di riferimenti esterni non necessari;
- distinguere **import**, **modifica locale** ed eventuale **sincronizzazione verso l'origine**.

## 4.4 Associazione dei media ES-DE

ES-DE **non richiede** che i percorsi media siano memorizzati nei tag di `gamelist.xml`; li riconosce in directory per sistema e tipo, sulla base del nome/percorso ROM, incluse le sottocartelle. Retroverse dovrà replicare le regole applicabili alla versione di origine, evitando match approssimativi basati esclusivamente sul titolo visualizzato.

Esempio concettuale:

```text
C:\Users\User\ES-DE\
├── gamelists\
│   └── snes\
│       └── gamelist.xml
├── downloaded_media\
│   └── snes\
│       ├── covers\
│       │   └── Super Mario World.jpg
│       ├── physicalmedia\
│       │   └── Super Mario World.png
│       ├── screenshots\
│       │   └── Super Mario World.png
│       └── videos\
│           └── Super Mario World.mp4
└── settings\
    └── es_settings.xml
```

Tipologie riconosciute: `covers`, `backcovers`, `3dboxes`, `physicalmedia`, `screenshots`, `titlescreens`, `marquees`, `miximages`, `fanart`, `manuals`, `videos`, `custom`. Non tutte sono presenti in ogni libreria.

### Regole di risoluzione

- Cercare prima una risorsa esplicitamente mappata e verificata dall'adapter.
- Replicare il naming ES-DE basato sul percorso ROM e su eventuali directory annidate.
- Rispettare la directory media configurata, non solo il default.
- Gestire varianti di estensione effettivamente supportate, case-insensitive quando appropriato su Windows.
- Non confondere titoli omonimi o versioni regionali differenti.
- Mostrare un fallback in caso di assenza, senza lanciare scraping non richiesto.
- Memorizzare nel catalogo il percorso reale della risorsa condivisa, non una copia duplicata.

## 4.5 EmulationStation legacy

L'adapter separato supporta, per gli schemi accertati, `es_systems.cfg`, `gamelist.xml` e riferimenti espliciti come `<image>`, `<video>`, `<thumbnail>`. Non converte silenziosamente i file in formato ES-DE. I nomi dei sistemi devono passare da una tabella di alias, non da sostituzioni hard-coded a livello di renderer.

## 4.6 Sincronizzazione non distruttiva

**Default della release iniziale: modalità di sola lettura dei metadati condivisi.**

Un'eventuale scrittura successiva richiede:

1. Consenso esplicito per il singolo tipo di modifica.
2. Backup e registrazione dell'hash/versione originale del file.
3. Rilettura immediata prima del commit e rilevamento delle modifiche concorrenti.
4. Aggiornamento del nodo specifico preservando campi sconosciuti, ordine dove fattibile e compatibilità dello schema.
5. Scrittura in file temporaneo sullo stesso volume, flush e sostituzione sicura con strategia di recovery Windows.
6. Report e annullamento quando l'origine è stata modificata nel frattempo.
7. Mai riscrivere XML condivisi mentre un'altra applicazione potrebbe salvare lo stesso file, senza controllo di conflitto.

Lo scraping può scaricare media direttamente nella directory ES-DE scelta dall'utente; la modifica dei metadati XML è una scelta indipendente. Retroverse non scrive nelle ROM né nei file BIOS.

---

# 5. Primo avvio e profili libreria

## 5.1 Wizard guidato

1. **Benvenuto e modalità** — `Importa da ES-DE`, `EmulationStation legacy`, `Cartelle ROM manuali`.
2. **Rilevamento dati** — scelta home ES-DE, directory ROM e directory media; test di accessibilità.
3. **Anteprima importazione** — conteggio sistemi, ROM, media presenti, titoli non riconosciuti, possibili errori.
4. **Emulatori** — profili rilevati, abbinamento sistema/emulatore e test di apertura su titolo campione.
5. **Aspetto del mondo** — preset `Cameretta anni '90`, `Museo`, `Mega game room`, `Arcade`; seed e densità.
6. **Media** — riuso delle risorse presenti; proposta non obbligatoria di configurare provider per gli asset mancanti.
7. **Generazione** — calcolo del layout, stima capienza, creazione del primo mondo, tutorial controlli.

Nessuna credenziale esterna è necessaria per giocare ai titoli già presenti. È possibile saltare lo scraping e configurarlo successivamente.

## 5.2 Profili multipli

Supportare profili con collezioni diverse, anche quando i dati condividono gli stessi emulatori. Ogni profilo contiene origine e configurazione di mondo proprie. Passare da un profilo all'altro chiude in modo sicuro decoder, watcher e risorse legate alla libreria precedente.

## 5.3 Modalità di esecuzione Windows

- Finestra, borderless fullscreen e fullscreen se supportato.
- Avvio automatico opzionale con Windows, configurato dal sistema utente e reversibile.
- Modalità `kiosk` con input da controller e uscita protetta da conferma.
- Retroverse non modifica la shell di Windows, non disinstalla EmulationStation e non lo disabilita automaticamente.

---

# 6. Dominio, identità e modello dati

## 6.1 Entità principali

| Entità | Campi rilevanti | Responsabilità |
|---|---|---|
| `LibraryProfile` | id, adapter, home, romRoot, mediaRoot, options | Fonte catalogo |
| `System` | id, sourceName, canonicalId, fullName, aliases, mediaType | Piattaforma |
| `Game` | id, systemId, romPath, relativePath, title, metadata, status | Gioco logico |
| `GameAsset` | gameId, kind, path, source, region, validity | Media condiviso |
| `DiscGroup` | id, gameId, entries, primaryDisc | Multi-disco e playlist |
| `EmulatorProfile` | id, platform, exe, argvTemplate, workingDir, env | Avvio |
| `WorldDefinition` | id, seed, generatorVersion, style, settings | Regole mondo |
| `Room` | id, zone, theme, capacity, graphNode, geometryKey | Locale generato |
| `Fixture` | id, roomId, prefabId, transform, capacity | Arredo/interattivo |
| `ShelfSlot` | id, fixtureId, index, dimensions, assignedGameId | Alloggiamento |
| `PlacementOverride` | entityId, transform, pin, provenance | Modifica dell'utente |
| `DisplayDevice` | id, fixtureId, playlist, policy, audio, quality | Schermo 3D |
| `PlaySession` | id, gameId, start, end, outcome | Cronologia |
| `ScrapeJob` | id, provider, target, state, retries, result | Download |
| `MediaMatchReview` | id, gameId, candidates, decision | Match incerti |

## 6.2 Identità persistenti

Un gioco deve avere un **ID persistente assegnato alla prima importazione**, distinto dal nome o dal percorso assoluto della ROM. Le lookup key includono `sourceProfile + canonicalSystemId + relativeRomPath normalizzato`; l'ID può essere riconciliato quando il file viene spostato tramite hash/seriale, dimensione, directory e conferma manuale. Non fare sempre hash di ISO gigantesche all'avvio: usare scansione leggera e hashing opzionale/progressivo.

Per titoli multi-disco, la rappresentazione principale può essere una sola custodia con il gruppo di dischi collegati. I playlist file `.m3u` possono essere l'entrypoint canonico se l'emulatore e il sistema lo richiedono.

## 6.3 Esempio di database SQLite (schema illustrativo)

```sql
CREATE TABLE libraries (
    id TEXT PRIMARY KEY,
    adapter TEXT NOT NULL,
    home_path TEXT NOT NULL,
    rom_root TEXT NOT NULL,
    media_root TEXT NOT NULL
);

CREATE TABLE systems (
    id TEXT PRIMARY KEY,
    library_id TEXT NOT NULL REFERENCES libraries(id),
    source_name TEXT NOT NULL,
    canonical_platform TEXT NOT NULL,
    title TEXT NOT NULL
);

CREATE TABLE games (
    id TEXT PRIMARY KEY,
    system_id TEXT NOT NULL REFERENCES systems(id),
    source_relative_path TEXT NOT NULL,
    absolute_rom_path TEXT NOT NULL,
    title TEXT NOT NULL,
    metadata_json TEXT NOT NULL DEFAULT '{}',
    fingerprint TEXT,
    is_available INTEGER NOT NULL DEFAULT 1,
    UNIQUE(system_id, source_relative_path)
);

CREATE TABLE media_assets (
    id TEXT PRIMARY KEY,
    game_id TEXT NOT NULL REFERENCES games(id),
    kind TEXT NOT NULL,
    absolute_path TEXT NOT NULL,
    provenance TEXT,
    is_available INTEGER NOT NULL DEFAULT 1
);

CREATE TABLE room_placements (
    id TEXT PRIMARY KEY,
    world_id TEXT NOT NULL,
    room_id TEXT NOT NULL,
    fixture_id TEXT NOT NULL,
    slot_id TEXT NOT NULL,
    game_id TEXT REFERENCES games(id),
    is_pinned INTEGER NOT NULL DEFAULT 0,
    UNIQUE(world_id, slot_id)
);

CREATE TABLE world_overrides (
    world_id TEXT NOT NULL,
    object_id TEXT NOT NULL,
    override_json TEXT NOT NULL,
    PRIMARY KEY(world_id, object_id)
);

CREATE TABLE play_sessions (
    id TEXT PRIMARY KEY,
    game_id TEXT NOT NULL REFERENCES games(id),
    started_at TEXT NOT NULL,
    ended_at TEXT,
    exit_code INTEGER
);

CREATE TABLE scrape_jobs (
    id TEXT PRIMARY KEY,
    provider TEXT NOT NULL,
    target_game_id TEXT NOT NULL REFERENCES games(id),
    task_type TEXT NOT NULL,
    status TEXT NOT NULL,
    attempts INTEGER NOT NULL DEFAULT 0,
    error TEXT
);
```

Lo schema definitivo deve includere `schema_version`, migrazioni, indici di ricerca full-text (FTS5), timestamps, transazioni e vincoli per eliminazione/archiviazione. Eseguire migrazioni versionate, testate con database delle release precedenti.

---

# 7. Generazione procedurale del mondo

## 7.1 Oggetti generati

- Hub principale e passaggi fra stanze.
- Stanze tematiche, proporzionate alla collezione.
- Pareti, pavimenti, soffitti, porte, finestre decorative e punti luce.
- Librerie, scaffali, teche, espositori, banconi e tavolini.
- Cartucce, custodie, dischi, cartoncini e scatole tridimensionali.
- Console, controller, cavi decorativi, memory card e accessori.
- TV CRT, monitor LCD, vetrine video, cabine arcade e schermi informativi.
- Poster e decorazioni selezionati in base a piattaforma, epoca e disponibilità media.
- Segnaletica, targhette e indicatori discreti.
- Volumi di collisione, spawn point, trigger interazione e percorsi accessibili.

## 7.2 Input del generatore

```text
CatalogSnapshot
  + WorldPreset
  + WorldSeed
  + PlatformVisualManifest
  + AssetLibrary
  + GeneratorVersion
  + PreviousWorldPlan (facoltativo)
  + UserOverrides
  => WorldPlan
```

L'output `WorldPlan` deve essere una **descrizione serializzabile**, distinta dalle entità GPU. Non creare mesh o texture durante il calcolo della pianta. Il renderer materializza soltanto le aree necessarie.

## 7.3 Algoritmo ad alto livello

1. Normalizzare la collezione e ordinare gli input in modo stabile.
2. Raggruppare per piattaforma, produttore, generazione o altro criterio del preset.
3. Calcolare numero e tipologia degli oggetti da esporre.
4. Stimare gli slot necessari, con riserva di crescita configurabile.
5. Determinare quante stanze servono e i loro requisiti minimi.
6. Generare un **grafo connesso** delle stanze e relativi varchi.
7. Derivare le geometrie modulari e i corridoi senza intersezioni.
8. Posizionare prima porte, TV, console e ingombri essenziali.
9. Disporre scaffali e espositori seguendo regole di navigabilità.
10. Riempire gli slot con giochi ordinati e assegnazioni persistenti.
11. Applicare decorazioni e luci guidate dal tema, usando PRNG con seed.
12. Applicare gli override manuali e risolvere conflitti dichiarati.
13. Validare capienza, connettività, raggiungibilità e collisioni.
14. Salvare il piano; costruire la scena visuale a richiesta.

### Pseudocodice

```cpp
WorldPlan WorldGenerator::build(const CatalogSnapshot& catalog,
                                const WorldConfig& config,
                                const std::optional<WorldPlan>& previous,
                                const OverrideSet& overrides) {
    auto groups = planner.groupGames(catalog, config.grouping);
    auto requirements = planner.computeRequirements(groups, config);
    auto layout = topology.allocateConnectedRooms(requirements, config.seed);
    fixtures.placeCriticalStations(layout, requirements);
    fixtures.fillWithShelves(layout, requirements, config.density);
    allocator.reusePinnedAndStableAssignments(layout, previous, overrides);
    allocator.placeUnassignedGames(layout, catalog);
    decorations.generate(layout, config.seed);
    validator.requireConnectedAndNavigable(layout);
    return serializer.toWorldPlan(layout, config.generatorVersion);
}
```

Questo esempio descrive **l'API desiderata**, non codice compilabile già implementato.

## 7.4 Politiche di suddivisione in stanze

- **Default:** una zona per sistema; sistemi con pochi titoli possono condividere una stanza mantenendo scaffali distinti.
- **Collezioni grandi:** se una stanza supera capacità o dimensione massima, suddividerla in due o più locali collegati.
- **Arcade:** cabine e piccoli gruppi di cabinati; rappresentazione fisica senza fingere che ogni ROM sia una cartuccia.
- **Portatili:** postazioni con console e espositori di cartucce/custodie proporzionate.
- **Computer storici:** scrivanie e schermi coerenti, con media appropriati.
- **Titoli senza supporto fisico definito:** scatola generica o card digitale visibile, mai gioco invisibile.
- **Sistemi senza giochi:** nascosti o presentati come sala espositiva soltanto se voluto.

## 7.5 Capienza e modularità

Esempio di calcolo, con parametri configurabili:

```text
requiredSlots = ceil(gameCount * (1 + reservedGrowthRatio))
shelvesNeeded = ceil(requiredSlots / effectiveSlotsPerShelf)
roomArea = circulationArea + criticalFixturesArea + shelvingFootprint
```

La `effectiveSlotsPerShelf` dipende dalla forma fisica e dall'orientamento dei giochi: una custodia PS1 e una cartuccia N64 occupano spazi differenti. Il sistema deve prevedere dimensioni dichiarate dai prefab, non costanti globali valide per tutto.

**Vincolo di navigabilità:** nessun arredo deve rendere irraggiungibile una porta, un televisore interattivo o uno scaffale. La larghezza di passaggio suggerita iniziale è almeno 1,2 metri virtuali, configurabile dal profilo e verificata con collisioni e navgrid. Evitare corridoi infiniti, stanze vuote e scaffali impossibili da raggiungere.

## 7.6 Determinismo, stabilità e modifiche incrementali

Il mondo deve usare un PRNG esplicito, con seed e versione dell'algoritmo salvati; nessuna dipendenza dalla sequenza casuale dell'API globale o dall'ordine non deterministico di `unordered_map`.

Quando cambia il catalogo:

1. riconciliare le identità dei giochi;
2. preservare oggetti e slot già assegnati;
3. applicare immediatamente le modifiche media e metadata, senza rigenerare geometria;
4. usare prima gli slot liberi per nuovi giochi;
5. aggiungere scaffali nel rispetto dell'area disponibile;
6. espandere la stanza o creare una stanza aggiuntiva solo quando necessario;
7. preservare gli override `pinned`;
8. chiedere conferma per modifiche strutturali che coinvolgono oggetti personalizzati.

Azioni esplicite: **Aggiorna libreria**, **Riorganizza scaffale**, **Rigenera stanza**, **Rigenera mondo**. Le ultime due mostrano un riepilogo dell'impatto sugli oggetti prima di procedere.

## 7.7 Stili procedurali

Preset iniziali:

- `Collector Bedroom 1995` — camera compatta, CRT, poster, scaffali domestici.
- `Collector Apartment 2002` — più stanze, ambienti caldi, console per generazioni.
- `Retro Museum` — espositori ordinati, didascalie, percorsi tematici.
- `Arcade Hall` — luci al neon, cabinati, attenzione agli schermi.
- `Minimal Gallery` — meno elementi decorativi, leggibilità massima.

Ogni preset espone un manifesto di regole: palette, set prefab, materiali, numero target di TV, luce, rumore ambientale, densità, esposizione dei titoli e variazioni consentite. Non dipende da un livello costruito manualmente per ciascuna collezione.

---

# 8. Rappresentazione 3D dei videogiochi

## 8.1 Supporti fisici supportati

| Categoria | Esempi | Forma visiva |
|---|---|---|
| Cartucce domestiche | NES, SNES, N64, Mega Drive | Mesh specifica e label dinamica |
| Cartucce portatili | GB, GBC, GBA, DS/3DS | Mesh dimensionata e adesivo |
| CD jewel case | PS1, Saturn, Dreamcast (varianti) | Custodia + disco opzionale |
| Case DVD | PS2, Wii, Xbox | Custodia + fascetta |
| Case proprietarie | GameCube, PSP/UMD | Mesh e artwork appropriati |
| Supporti vintage | Floppy, cassette | Mesh di supporto riutilizzabile |
| Arcade | ROM set arcade | Cabinato o pannello espositivo |
| Sconosciuto | qualsiasi sistema | Confezione/placchetta generica |

La rappresentazione fisica e il launcher sono indipendenti: una ROM `.zip` può rappresentare una cartuccia visiva, una scheda arcade o altro, secondo la piattaforma.

## 8.2 Materiali e texture dinamiche

- Ogni prefab definisce gli slot texture: fronte, dorso, retro, etichetta cartuccia, disco, logo e piccolo display.
- Il sistema legge le immagini ES-DE disponibili e compone **texture derivate** solo in cache, mantenendo immutato l'originale.
- Quando manca l'immagine, genera una label leggibile da titolo, sistema e regione.
- Lo stesso prefab è condiviso da migliaia di giochi; cambiano materiali/atlas e parametri per istanza.
- Prevedere cover in più risoluzioni e sostituzione progressiva della miniatura con texture di qualità quando l'oggetto è vicino.
- Alcuni asset, come `physicalmedia` o `3dboxes`, sono immagini: non possono essere automaticamente scambiati per modelli 3D.

## 8.3 Interazione con oggetti

Stati principali di ogni supporto: `OnShelf`, `Focused`, `HeldForInspection`, `AnimatingToConsole`, `Launching`, `Returning`, `PinnedPlacement`. Animazioni non obbligatorie e interrompibili. Le confezioni possono ruotare ed essere ingrandite; in presenza di un asset dedicato è consentito aprire il case o estrarre il disco.

---

# 9. Televisori, anteprime video e audio 3D

## 9.1 Tipologie di schermo

| Tipo | Posizionamento | Contenuto |
|---|---|---|
| CRT principale | Vicino alla console della stanza | Gioco selezionato / playlist della sala |
| CRT secondario | Scaffale o angolo espositivo | Rotazione di giochi della stanza |
| Monitor informativo | Ingresso stanza | Titoli, classifiche, copertine, informazioni |
| Arcade monitor | Nel cabinato | Gameplay o immagine del titolo associato |
| Monitor moderno | Sala post-2000 | Anteprime con materiali diversi dai CRT |
| Schermo decorativo | Insegna o vetrina | Animazioni leggere, non necessariamente video |

**Regola:** per default, ogni stanza che ospita giochi contiene almeno **un display attivo**, salvo disattivazione da parte dell'utente o profilo grafico ridotto. Il numero massimo di video contemporaneamente decodificati è separato dal numero di televisori fisicamente presenti.

## 9.2 Comportamenti configurabili

- `SelectedGame`: anteprima del supporto sul quale è posto il focus.
- `RoomShuffle`: estrazione pseudo-casuale di clip appartenenti alla stanza.
- `RoomSequential`: riproduzione ciclica secondo ordine catalogo.
- `Favorites`: playlist di preferiti filtrati per sistema o stanza.
- `RecentlyPlayed`: ultimi giochi eseguiti.
- `ExplicitPlaylist`: selezione manuale.
- `IdleAttract`: rotazione quando nessun gioco è selezionato.

Parametri per display: `enabled`, `autoplay`, `audioMode`, `volume`, `maxResolution`, `playlistMode`, `priority`, `proximityThreshold`, `loop`, `previewDuration`, `showOnFocus`, `crtPreset`, `fallbackImageType`.

## 9.3 Eventi e priorità

- Entrando in stanza: la TV principale attiva una playlist se l'utente è sufficientemente vicino.
- Passando il focus su una cartuccia: la TV principale della stanza tenta il video di quel titolo.
- Video assente: usa screenshot, titolo o cover e mostra un'opzione `Scarica anteprima` se lo scraping è configurato.
- Uscendo dalla stanza: la riproduzione può essere sospesa, mantenendo il frame visibile.
- Avviando un emulatore: stop/sospensione di tutti i decoder e dell'audio ambientale.
- Tornando dall'emulatore: ripresa delle playlist e della scena secondo le preferenze.
- In più schermi contemporanei: l'audio proviene normalmente dal più vicino/rilevante; non generare un muro di suoni incomprensibile.

Priorità proposta: `Schermo direttamente selezionato` > `Schermo con focus su gioco` > `TV principale della stanza corrente` > `TV ambientale lontana`.

## 9.4 Decodifica video: pipeline obbligatoria

```text
Shared media file (.mp4/.mkv/.avi/...)
        |
        v
FFmpeg AVFormatContext (demux)
        |
        v
AVCodecContext (decodifica video; clock e timestamp)
        |
        v
FrameQueue limitata (CPU, worker thread)
        |
        v
Conversione YUV -> RGB(A) o caricamento piani YUV
        |
        v
OpenGL Texture / PBO upload (render thread)
        |
        v
Materiale emissivo della mesh schermo
        |
        v
Shader CRT opzionale + post processing
```

Il decoder deve usare le API `avcodec_send_packet` e `avcodec_receive_frame`, gestire `EAGAIN`, fine stream, seek e drain. Il clock di presentazione deve basarsi sui timestamp, non su «un frame per render frame». Il media manager non deve bloccare il render thread per lettura disco, apertura file o decoding.

Strategia v1: decoding software con FFmpeg, ring buffer a capienza ridotta e upload texture OpenGL controllato. L'accelerazione hardware video su Windows (D3D11VA/DXVA2) è un'ottimizzazione eventuale, perché l'interoperabilità efficiente con OpenGL va misurata e non va presupposta. La prima versione può convertire frame in CPU; la conversione YUV in shader è una successiva ottimizzazione.

## 9.5 Limiti di risorse

Valori iniziali, configurabili e da confermare con benchmark:

- massimo **2 decoder video attivi** nel profilo qualità normale;
- massimo **1 decoder** nel profilo risparmio;
- anteprime ridimensionate a 480p/720p in base al contesto;
- frame statico per TV non visibili o non prioritarie;
- stop decoding dopo un tempo di inattività o distanza massima;
- non mantenere in RAM tutti i video o tutti i fotogrammi;
- playlist con prefetch della prossima clip solo se nel budget memoria;
- fallback a immagine quando il codec non è disponibile, il file è corrotto o il budget è esaurito.

Il numero di televisori non determina il numero di decoder; più TV possono mostrare lo stesso video condividendo un'unica texture, se hanno sorgente e tempi compatibili.

## 9.6 Effetto CRT

Lo shader potrà simulare, ciascun effetto disattivabile: scanline, maschera RGB, curvatura, vignettatura, lieve bloom, risposta dei fosfori, distorsioni contenute, aspect ratio 4:3. Preset `Neutral`, `Consumer CRT`, `Arcade CRT`, `High quality monitor`. Evitare l'effetto «retro» a scapito della leggibilità e prevedere sempre l'opzione video pulito.

## 9.7 Audio

- Audio stereo delle clip gestito da FFmpeg e inoltrato al mixer miniaudio.
- Volume spaziale e attenuazione in base alla distanza, ove abilitati.
- Priorità e ducking per il video in focus.
- Suoni interazione: pulsanti, sportelli, cartucce, passi.
- Ambiente: ventole, ronzii leggeri, sala arcade, musica fornita dall'utente.
- Mute immediato, volume master, volume video/effetti/ambiente separati.
- Nessun codec o contenuto musicale soggetto a diritti viene incluso senza adeguata licenza.

---

# 10. Scraper integrato e gestione dei media

## 10.1 Obiettivo UX

Le opzioni per scaricare cover, retrocopertine, screenshot, loghi, immagini del supporto fisico, anteprime video e manuali devono essere **accessibili e comprensibili dal frontend stesso**, senza modificare XML né eseguire script a mano.

Il sistema mostra in ogni momento quali categorie hanno media disponibili, quali sono mancanti e quali presentano un'associazione ambigua.

## 10.2 Provider e capacità

- **ScreenScraper:** provider primario desiderato per immagini, supporti fisici, anteprime video e manuali, secondo effettiva disponibilità e autorizzazione API.
- **TheGamesDB:** adapter opzionale per le tipologie che realmente fornisce; non promettere video o tipi non disponibili.
- **File locali:** import di copertine e video raccolti dall'utente, con associazione manuale o automatica basata sul percorso.
- **Provider futuri:** adattatori separati, con dichiarazione dinamica delle categorie supportate e condizioni d'uso.

### Vincolo esterno inderogabile

L'API ScreenScraper richiede credenziali identificative per lo sviluppatore. La documentazione pubblica prevede l'integrazione in applicazioni gratuite distribuite, o condizioni concordate per usi differenti, e invita gli sviluppatori a richiedere credenziali tramite i canali del progetto. **Non incorporare credenziali di terzi o aggirare quote/restrizioni**. Se l'accesso legittimo non è disponibile, lo scraping tramite tale provider rimane disabilitato con messaggio chiaro; il resto dell'app funziona normalmente.

## 10.3 Schermata «Gestione media»

Campi e controlli:

- Provider attivo, stato API, autenticazione e test connessione.
- Eventuale account utente opzionale, inserimento sicuro delle credenziali.
- Priorità regione: PAL/EU, USA, Giappone o fallback configurabile.
- Lingua preferita per metadati testuali.
- Tipi da scaricare: `covers`, `backcovers`, `screenshots`, `titlescreens`, `marquees`, `3dboxes`, `physicalmedia`, `fanart`, `manuals`, `videos`.
- Qualità e limiti di dimensione per immagini/video.
- Ambito: tutta la collezione, una piattaforma, una stanza, una selezione, solo titoli senza metadata, solo cover mancanti, solo video mancanti, solo nuovi giochi.
- Gestione sovrascrittura: **mai**, chiedi, solo se file rotto, sempre (con avviso e backup se applicabile).
- Scrittura dei metadati nel `gamelist.xml`: disattivata di default; attivabile esplicitamente.
- Anteprima prima del download e confronto dei candidati.
- Coda con pausa, riprendi, annulla, riavvia falliti ed esporta report.

## 10.4 Matching e affidabilità

1. Identificare piattaforma secondo ID condiviso con l'adapter provider.
2. Cercare con CRC/MD5/SHA1 e seriale quando utili e compatibili, altrimenti nome file pulito e metadati.
3. Non eseguire hashing completo di ISO enormi senza una policy di costo e un indicatore visibile.
4. Elaborare i risultati con una confidence score motivabile.
5. Richiedere scelta utente per match ambigui, traduzioni, hack, fan translation e revisioni multiple.
6. Mantenere `sourceId`, data e provenienza di ogni risorsa scaricata.
7. Se un file è già presente e corretto, non scaricarlo di nuovo per impostazione predefinita.

## 10.5 Download e destinazione

- Scrivere i media nelle directory ES-DE esistenti, preservando naming e sottocartelle previsti dall'adapter.
- Scaricare in `*.part` fuori dal percorso che il frontend tratta come media definitivo.
- Validare risposta HTTP, formato reale, limiti dimensione, hash se disponibile e decodificabilità elementare.
- Effettuare move/rename finale solo a download completo.
- Aggiornare catalogo/texture/schermi quando i nuovi file diventano disponibili.
- Accettare download manuali inseriti dall'utente senza riavvio obbligatorio.
- Salvare in SQLite stato dei job e tentativi, con retry e exponential backoff per errori transitori.
- Rispettare HTTP 429, rate limit, credenziali errate e divieti del provider.

## 10.6 Sicurezza credenziali

Credenziali utente gestite con **Windows Credential Manager o DPAPI** anziché conservate in chiaro in TOML, JSON o log. Segreti di integrazione dell'app non devono essere recuperabili banalmente da un binario distribuito: definire modalità autorizzata/servizio di accesso conforme alle regole del provider prima del rilascio pubblico. Non inviare telemetria della libreria salvo consenso esplicito.

## 10.7 Dati testuali vs media

È possibile aggiornare i file media condivisi senza riscrivere `gamelist.xml`; quando serve salvare nomi, descrizioni, date e altri metadati, Retroverse usa il writer non distruttivo descritto nella sezione 4.6. La modifica di preferiti/playcount/ultimo gioco usato condivisi rimane subordinata alle regole effettive della variante di origine.

---

# 11. Integrazione e avvio degli emulatori Windows

## 11.1 Principio

Retroverse è un **frontend**, non un emulatore. Il launcher traduce un `Game` in un comando verificato, utilizzando emulatori già installati. Non devono essere presenti percorsi di eseguibili o core hard-coded per macchina.

## 11.2 Profili di lancio

Ogni `EmulatorProfile` definisce:

- `id`, `displayName`, `systemIds`, `executablePath`;
- `argumentTokens` oppure un template validato per singoli argomenti;
- `workingDirectory`, ambiente, variabili ammesse;
- opzioni fullscreen, esecuzione con privilegi dell'utente, gestione di processi figli;
- estensioni/entrypoint supportati e preferenza di launcher per sistema;
- test di validazione ed eventuali dipendenze (core RetroArch e simili).

Profili preconfigurabili: RetroArch, DuckStation, PCSX2, Dolphin, PPSSPP, MAME e altri. I comandi esatti sono specifici della versione installata e devono essere verificati; i preset non devono essere presentati come universali.

## 11.3 Compatibilità di lancio con ES-DE

`es_systems.xml` contiene sistema, estensioni e comandi con variabili/special token come `%ROM%`, `%ROMPATH%`, `%EMULATOR_RETROARCH%`, `%CORE_RETROARCH%`, `%STARTDIR%`, ecc. Il software ES-DE può possedere risoluzioni e override propri. La **sola lettura dell'XML non garantisce di ricostruire automaticamente la stessa configurazione di avvio**.

Strategia obbligatoria:

1. Importare definizioni dei sistemi e override dove disponibili.
2. Riconoscere e classificare token ES-DE supportati dall'adapter.
3. Se tutti i token sono risolti e il comando è validabile, costruire un profilo compatibile.
4. Quando non è possibile risolverli, mostrare `Richiede configurazione launcher` e suggerire un profilo emulatore standard.
5. Conservare la selezione fatta dall'utente per quel sistema o singolo titolo.
6. Consentire un **test di lancio** prima di contrassegnare il profilo come pronto.
7. Non eseguire stringhe provenienti da XML non fidati tramite `cmd.exe` per «replicare» automaticamente ES-DE.

La promessa di prodotto è **compatibilità con la collezione dati** e **avvio degli emulatori configurati**, non esecuzione garantita di ogni comando ES-DE senza migrazione delle impostazioni.

## 11.4 Processo Windows

Usare `CreateProcessW` con stringa di comando/quoting Windows costruiti da token validati (o `ShellExecuteExW` per casi speciali intenzionali e verificati). I percorsi Unicode, gli spazi e le virgolette devono essere gestiti senza interpolazioni shell e senza aprire vettori di command injection.

Ciclo di vita:

```text
Idle (Retroverse in foreground)
  -> Validate ROM + EmulatorProfile
  -> Save camera and selected object
  -> Stop/Suspend video decoders + audio
  -> Minimize/Hide Retroverse
  -> Launch external emulator
  -> Wait for authoritative emulator process exit
  -> Recover foreground/window/display state
  -> Resume input/video/audio
  -> Record PlaySession
  -> Idle
```

Nel caso di wrapper o emulatori che lanciano processi figli, il launcher deve supportare strategie di attesa esplicite (`waitDirectProcess`, `waitProcessTree`, `userConfirmReturn`) e timeout ragionevoli per rilevare l'errore. Non chiudere l'emulatore per errore solo perché il processo wrapper iniziale è terminato.

## 11.5 Comportamenti speciali

- ROM mancanti, media mancanti o emulatore non trovato: errore leggibile, ritorno sicuro alla scena.
- File `.m3u`, `.cue`, directory gioco, collegamenti Windows o launcher speciali: trattare come entrypoint in base al profilo, non come file equivalenti.
- Multi-disco: mantenere il gruppo logico e delegare il cambio disco all'emulatore; UI d'ispezione può mostrare più supporti.
- Alcuni emulatori gestiscono autonomamente salvataggi, save state, BIOS, mapping controller e shader: Retroverse non tenta di scriverli o riprodurli.
- Alla chiusura del frontend durante una sessione, non terminare l'emulatore senza conferma.
- In caso di crash Retroverse o emulatore, conservare sessione e stato mondo recuperabili.

---

# 12. Movimento, interazione e controller

## 12.1 Controlli

| Azione | Mouse/tastiera (default) | Controller (default indicativo) |
|---|---|---|
| Muoversi | WASD | Stick sinistro |
| Guardare | Mouse | Stick destro |
| Interagire | E / click | A / Cross |
| Indietro | Esc | B / Circle |
| Aprire catalogo rapido | Tab | Y / Triangle |
| Ispezionare oggetto | E / pulsante dedicato | A contestuale |
| Gioca subito | Enter dal catalogo | Start/A contestuale |
| Aprire mappa | M | Select/View |
| Menu impostazioni | Esc menu | Start/Menu |
| Spostare/arredare | Modalità editor | Modalità editor |

I mapping devono essere personalizzabili. Il backend SDL3 normalizza assi e pulsanti; prevedere hotplug, riconnessione, più controller e priorità al dispositivo attivo.

## 12.2 Modalità interazione

- **FreeRoam:** navigazione immersiva con capsule collision e camera prima persona.
- **Inspect:** un oggetto è vicino alla camera, ruotabile, zoomabile, con eventuale custodia apribile.
- **FocusScreen:** visualizzazione anteprima, metadati e informazioni del titolo.
- **ConsoleUse:** animazione contestuale di inserimento disco/cartuccia e avvio opzionale.
- **QuickLibrary:** overlay globale con ricerca e filtri.
- **DecorationMode:** spostamento, rotazione e blocco dei prefab consentiti.
- **Settings:** UI full-screen controller-friendly.

Ogni stato ha regole chiare su focus input, movimento camera, uscita e ripristino. Nessuna sequenza fisica deve essere obbligatoria per il comando `Avvia`.

## 12.3 Selezione 3D

Raycast dalla camera, interfaccia con collision shape e `IInteractable`; distanza massima configurabile; indicatore discreto dell'oggetto selezionabile. Gli oggetti sugli scaffali devono avere target interattivi che rimangono usabili anche quando le copertine sono ravvicinate.

Per v1 usare collisioni semplici (AABB/OBB, capsule e ray intersection) e navgrid; un motore fisico dedicato come Jolt può essere introdotto se il gameplay di manipolazione lo richiede veramente.

## 12.4 Accessibilità

- Riduzione/disattivazione head bob, accelerazioni camera ed effetti di movimento.
- FOV e sensibilità personalizzabili.
- Teletrasporto invece di spostamento fisico obbligatorio.
- Text scaling, UI ad alto contrasto e sottotitoli testuali dove pertinenti.
- Possibilità di disabilitare lampeggiamenti CRT, bloom, rumore visivo e audio ambientale.
- Modalità solo catalogo in caso di GPU insufficiente o preferenza utente.

---

# 13. Interfaccia utente e gestione della collezione

## 13.1 Filosofia UI

La UI non è una successione di widget di debug. Deve integrarsi nel mondo 3D tramite overlay coerenti, menu leggibili su TV e piccoli pannelli contestuali. RmlUi gestisce schermate di prodotto; Dear ImGui è riservato a sviluppo, editor avanzato e diagnostica.

## 13.2 Schermate principali

- **Home/world:** ultima stanza visitata, indicatori essenziali, accesso rapido.
- **Catalogo rapido:** ricerca full-text per titolo, piattaforma, anno, sviluppatore, publisher, genere, preferiti e stato.
- **Scheda gioco:** descrizione, metadata, cover, retro, screenshots, video, manuale se presente, regioni, varianti e azioni.
- **Mappa del mondo:** topologia stanze, teletrasporto, conteggio giochi e stato TV.
- **Gestione libreria:** sistemi, ROM mancanti, mapping, conflitti e scansione.
- **Gestione emulatori:** profili, test esecuzione, log e associazioni per sistema.
- **Gestione media:** scraping, impostazioni provider, ricerca asset mancanti, job e risultati.
- **Personalizzazione:** stili, seed, decorazione, generazione incrementale e rigenerazione.
- **Statistiche:** sessioni registrate, ultimi giochi, titoli più usati, giochi segnati come completati, distribuzione per console.
- **Impostazioni:** video, audio, controlli, privacy, performance, percorso dati.
- **Diagnostica:** log, errori di import, asset malformati, problemi shader e video.

## 13.3 Funzioni catalogo

- Ricerca veloce indipendente dalle stanze caricate.
- Preferiti, liste personali, giochi da recuperare e stato `Da iniziare`, `In corso`, `Completato`, `Abbandonato`.
- Ordinamento per nome, uscita, piattaforma, ultimo avvio, tempo giocato.
- Raggruppamento delle versioni regionali e dei dischi quando opportuno, mantenendo identificabili i file esatti.
- Teletrasporto direttamente davanti all'oggetto selezionato.
- Avvio diretto dalla scheda o dai risultati ricerca.
- Collegamento fra scheda gioco e oggetto fisico: `Mostrami sullo scaffale`.
- Espositori automatici di preferiti, ultimi giocati, completati.
- Filtri per assenza cover, video e configurazioni launcher mancanti.

## 13.4 Collezioni e statistiche

La componente personale può registrare informazioni non presenti in ES-DE: note, valutazione, stato di completamento, ultime sessioni e preferenze di esposizione. Queste informazioni restano separate per default. Il tempo di gioco è misurato **dal lancio fino al ritorno rilevato** e quindi è una stima della sessione, non necessariamente del tempo effettivo in-game. Eventuali integrazioni RetroAchievements o aggiornamenti del campo `playcount` vanno progettati e autorizzati separatamente.

---

# 14. Renderer OpenGL e scena 3D

## 14.1 Moduli GPU

- `GLDevice`: gestione contesto SDL, capability check, device lost/context recreation, debug callback.
- `ResourceManager`: shader, programmi, buffer, texture, mesh, sampler e cache.
- `RenderGraph`: pass geometria, shadow pass opzionale, emissive displays, trasparenze, compositing UI e post-processing.
- `SceneRenderer`: camera culling, raccolta draw commands, instancing e batching.
- `MaterialSystem`: materiali glTF, parametri PBR, texture bindings e fallback.
- `TextureAtlas`: composizione etichette, miniatura, contenuto derivato.
- `VideoSurfaceRenderer`: aggiornamento texture video e relativi shader.
- `LightingSystem`: luci statiche/dinamiche, ombre con budget e illuminazione ambientale.
- `AssetStreamer`: gestione asincrona I/O e upload GPU serializzati sul render thread.

## 14.2 Rendering pipeline semplificata

```text
SDL3 window + OpenGL context
          |
          v
Input/Event Pump ---> Application State
          |
          v
World Snapshot ---> Visibility/Culling
          |
          +--> Shadow/Light preparation
          +--> Opaque static geometry (instanced)
          +--> Game media / shelves / consoles
          +--> Animated video screens (emissive textures)
          +--> Transparent elements
          +--> Postprocess (tone map / CRT / optional bloom)
          +--> RmlUi overlay
          |
          v
SDL_GL_SwapWindow
```

## 14.3 Camera, collisioni e navigazione

La camera FreeRoam usa movimento a velocità limitata e collisione semplice. I mobili e le stanze dichiarano bounding volumes; la validazione procedurale dispone i mobili rispettando i volumi e la navgrid. Per interazione con console e cartucce è preferibile un sistema contestuale con animazioni di posizionamento, evitando una simulazione rigid-body generalista.

## 14.4 Ottimizzazione essenziale

- Frustum culling a livello di stanza/oggetto.
- Streaming delle stanze adiacenti e unload progressivo delle stanze distanti.
- Texture atlases/array per label e copertine quando compatibili; gestione LRU della cache GPU.
- Draw call batching e hardware instancing per scaffali, custodie, cartucce e elementi ripetuti.
- LOD per geometria e texture; mesh decorative disattivabili per profilo qualità.
- Budget di ombre e luci dinamiche.
- Render texture video solo se necessaria e nel budget.
- Evitare allocazioni nel loop caldo e parsing XML sul render thread.
- Gli oggetti invisibili non devono comportare caricamento obbligatorio di asset ad alta risoluzione.

---

# 15. Architettura applicativa C++

## 15.1 Suddivisione in layer

```text
+---------------------------------------------------------------+
| retroverse-app                                                |
| Main loop, CLI, bootstrap, user sessions, error recovery      |
+---------------------------------------------------------------+
| ui / interaction / world-view / input / media presentation    |
+---------------------------------------------------------------+
| domain                                                        |
| Catalog, Game, System, WorldPlan, Placement, LaunchRequest     |
+---------------------------------------------------------------+
| application services                                          |
| Importer, Generator, ScrapeCoordinator, EmulatorController    |
+---------------------------------------------------------------+
| ports (pure C++ interfaces)                                   |
| LibrarySource, MediaResolver, Scraper, Launcher, Storage      |
+---------------------------------------------------------------+
| adapters / infrastructure                                     |
| ES-DE, legacy ES, SQLite, Windows processes, HTTP, FFmpeg     |
+---------------------------------------------------------------+
| platform / renderer                                           |
| SDL3, OpenGL, file watching, audio, Win32                     |
+---------------------------------------------------------------+
```

Il dominio **non dipende** da OpenGL, SDL3, FFmpeg, RmlUi o Win32. Il generatore è testabile senza finestra né GPU, idealmente come libreria C++ pura che produce JSON serializzabile.

## 15.2 Interfacce di base (contratti illustrativi)

```cpp
struct GameId { std::string value; };
struct SystemId { std::string value; };
struct AssetPath { std::filesystem::path value; };

struct LibrarySnapshot {
    std::vector<System> systems;
    std::vector<Game> games;
    std::vector<GameAsset> media;
};

class ILibrarySource {
public:
    virtual ~ILibrarySource() = default;
    virtual DiscoveryResult discover(const DiscoveryOptions&) = 0;
    virtual LibrarySnapshot load(const LibraryProfile&) = 0;
    virtual ChangeSet diff(const LibrarySnapshot&) = 0;
};

class IMediaResolver {
public:
    virtual ~IMediaResolver() = default;
    virtual std::optional<AssetPath> resolve(
        const GameId&, MediaKind) const = 0;
};

class IWorldGenerator {
public:
    virtual ~IWorldGenerator() = default;
    virtual WorldPlan generate(const LibrarySnapshot&,
                               const WorldConfig&,
                               const std::optional<WorldPlan>&,
                               const OverrideSet&) = 0;
};

class IScraperProvider {
public:
    virtual ~IScraperProvider() = default;
    virtual ProviderCapabilities capabilities() const = 0;
    virtual ScrapeSearchResult search(const ScrapeQuery&) = 0;
    virtual DownloadDescriptor requestMedia(const MediaRequest&) = 0;
};

class IEmulatorLauncher {
public:
    virtual ~IEmulatorLauncher() = default;
    virtual LaunchValidation validate(const LaunchRequest&) const = 0;
    virtual LaunchSession launch(const LaunchRequest&) = 0;
};

class IVideoPlaybackService {
public:
    virtual ~IVideoPlaybackService() = default;
    virtual VideoHandle request(const VideoRequest&) = 0;
    virtual void release(VideoHandle) = 0;
};
```

Contratti reali devono esplicitare errori, cancellazione, ownership, sincronizzazione e lifetime; le firme qui rappresentano le responsabilità.

## 15.3 Ownership e thread model

Thread/logiche previsti:

- **Main/render thread:** SDL event loop, input, aggiornamento scena, comandi OpenGL, UI, frame presentation.
- **Catalog worker:** scansione filesystem, XML, lookup catalogo e diff.
- **World planner worker:** topologia, assegnazione slot e validazione geometria descrittiva.
- **Media workers:** lettura/resize immagini e preparazione texture CPU.
- **Video decoding worker(s):** demux/decode, clock e frame queues.
- **HTTP/scraping workers:** richieste remoto, download e retry nel rispetto limiti API.
- **Launcher monitoring:** attesa processo emulatore senza bloccare i task UI essenziali.

Nessun worker deve chiamare direttamente funzioni OpenGL associate al contesto principale. Il render thread riceve messaggi di upload e cleanup tramite command queue con ownership esplicita. I dati esposti alla scena sono snapshot immutabili o versionati; evitare accessi simultanei incontrollati ai container condivisi.

## 15.4 Event bus applicativo

Eventi utili: `LibraryScanStarted`, `LibraryScanCompleted`, `GameAdded`, `GameRemoved`, `GameMetadataChanged`, `MediaAvailable`, `RoomExpanded`, `PlacementChanged`, `ScreenFocused`, `VideoPlaybackFailed`, `LaunchStarted`, `LaunchFinished`, `ScrapeJobUpdated`, `SettingsChanged`, `SourceUnavailable`. Gli eventi devono avere payload identificabili, versioni e consegna deterministica dove richiesto.

## 15.5 Stato applicativo

```text
Boot
  -> SourceDiscovery / SetupWizard
  -> CatalogReady
  -> WorldGenerating
  -> WorldActive
       +-> Inspect
       +-> QuickLibrary
       +-> WorldEditor
       +-> Settings
       +-> ExternalEmulatorRunning
  -> Shutdown
```

`ExternalEmulatorRunning` deve sospendere/preservare risorse in modo esplicito. Una transizione fallita deve riportare l'app in uno stato utilizzabile, mai lasciarla invisibile senza possibilità di recupero.

---

# 16. Configurazioni, filesystem e cache

## 16.1 Percorsi Windows

Per un'installazione normale:

```text
%LOCALAPPDATA%\Retroverse\
├── config\
│   ├── settings.toml
│   └── profiles\<profile-id>.toml
├── database\retroverse.sqlite
├── worlds\<world-id>\
│   ├── world-plan.json
│   ├── overrides.json
│   └── backup\
├── cache\
│   ├── thumbnails\
│   ├── texture-atlases\
│   └── generated-labels\
├── jobs\
└── logs\
```

Modalità portable: stato sotto una cartella controllata dall'utente, ad esempio `RetroverseData\`, con percorsi relativi configurabili; avvisare quando la directory di destinazione non è scrivibile. I media ES-DE continuano a restare nella loro posizione esistente.

## 16.2 Esempio di configurazione TOML

```toml
[app]
profile = "default"
start_fullscreen = true
language = "it"

[library]
adapter = "esde"
home = "C:/Users/User/ES-DE"
rom_root = "D:/ROMs"
media_root = "C:/Users/User/ES-DE/downloaded_media"
read_only_metadata = true
watch_changes = true

[world]
seed = 102938
preset = "collector-apartment-2002"
grouping = "platform"
reserved_capacity = 0.20
incremental_update = true
preserve_pinned = true

[graphics]
opengl_target = "4.5"
resolution_scale = 1.0
vsync = true
max_fps = 60
shadows = "medium"
crt_effects = true

[video]
enabled = true
max_simultaneous_decoders = 2
max_height = 720
audio_mode = "nearest-focused"
proximity_meters = 12.0

[scraping]
provider = "screenscraper"
preferred_region = "eu"
preferred_language = "it"
media_types = ["covers", "physicalmedia", "videos"]
overwrite = "never"
write_shared_gamelist = false

[launcher]
restore_frontend_after_exit = true
suspend_preview_on_launch = true
```

Il file è **solo un esempio di formato progettato per Retroverse**, non una configurazione già supportata da un'app esistente. I valori devono essere validati allo startup e non includere segreti in chiaro.

## 16.3 Integrità e recovery

- Scritture atomiche per config e world plan; backup a rotazione.
- Database SQLite con transazioni e migrazioni; backup preventivo in caso di versione nuova.
- Ricostruibilità dell'indice, senza perdere gli override.
- Modalità safe graphics dopo crash di shader/rendering.
- Backup della configurazione prima di importazioni massive, rigenerazioni distruttive o reset.
- Log diagnostici senza password, token o dati personali non necessari.

---

# 17. Struttura repository e build

## 17.1 Layout di progetto suggerito

```text
retroverse/
├── CMakeLists.txt
├── CMakePresets.json
├── vcpkg.json
├── README.md
├── LICENSE
├── cmake/
├── docs/
│   ├── architecture/
│   ├── esde-compatibility/
│   ├── world-generation/
│   ├── asset-pipeline/
│   ├── video-decoding/
│   └── testing/
├── assets/
│   ├── models/
│   │   ├── consoles/
│   │   ├── physical-media/
│   │   ├── fixtures/
│   │   └── screens/
│   ├── materials/
│   ├── textures/
│   ├── shaders/
│   ├── ui/
│   ├── audio/
│   └── manifests/
├── src/
│   ├── main.cpp
│   ├── app/                   # Bootstrap, stato globale, sessione
│   ├── core/                  # Types, error handling, utilities
│   ├── domain/                # Game, System, Room, WorldPlan, IDs
│   ├── catalog/               # Import, indexing, queries, FTS
│   ├── adapters/
│   │   ├── esde/
│   │   ├── emulationstation_legacy/
│   │   ├── manual_library/
│   │   └── launchers/
│   ├── world/
│   │   ├── planner/
│   │   ├── topology/
│   │   ├── procedural_geometry/
│   │   ├── placement/
│   │   ├── validation/
│   │   └── persistence/
│   ├── scene/                 # Entity/component registry e cameras
│   ├── rendering/
│   │   ├── gl/
│   │   ├── shaders/
│   │   ├── materials/
│   │   ├── geometry/
│   │   ├── textures/
│   │   └── video_surfaces/
│   ├── media/
│   │   ├── images/
│   │   ├── video_ffmpeg/
│   │   ├── audio/
│   │   ├── scraper/
│   │   └── cache/
│   ├── platform/
│   │   ├── windows/
│   │   ├── sdl/
│   │   └── filesystem/
│   ├── input/
│   ├── interaction/
│   ├── ui/
│   ├── database/
│   └── diagnostics/
├── tests/
│   ├── unit/
│   ├── integration/
│   ├── snapshot/
│   ├── fixtures/
│   │   ├── esde-standard/
│   │   ├── esde-portable/
│   │   ├── es-legacy/
│   │   ├── nested-roms/
│   │   ├── multi-disc/
│   │   └── malformed-data/
│   └── benchmarks/
├── tools/
│   ├── asset-validator/
│   ├── fixture-generator/
│   └── worldplan-inspector/
├── packaging/
│   ├── windows-installer/
│   └── portable/
└── third_party/             # Solo dipendenze non gestibili via manager
```

## 17.2 Regole di build

- CMake target per ciascuna libreria interna, evitando un unico eseguibile monolitico con coupling incontrollato.
- vcpkg manifest mode con versioni pin e lock/config riproducibili; generator Ninja.
- MSVC x64 come toolchain Windows primaria; Clang-cl come verifica opzionale.
- Build Debug (validation GL, sanitizzazioni possibili), RelWithDebInfo e Release.
- Shader GLSL e asset installati con percorsi ricostruibili per installer/portable.
- Versione delle dipendenze e licensing generati nel pacchetto di distribuzione.
- CI: configure/build/test, analisi statica, generazione artifact, smoke test su Windows.
- Evitare API di Windows diffuse nel dominio; chiuderle negli adapter `platform/windows`.

## 17.3 Asset pipeline

- Modello sorgente DCC -> export glTF 2.0/GLB -> validazione automatica -> manifest JSON -> asset pack.
- Convenzioni standard per scala (metri), assi, rotazioni, pivot e origine degli oggetti.
- Ogni prefab dichiara `boundingBox`, `interactionAnchor`, `displaySlots`, `textureSlots`, `collisionShape`, `variants`, `lods`.
- Texture in dimensioni e formati documentati, mipmap quando utili.
- Il world generator non deve contenere stringhe di path a file `.glb`; risolve prefab e categorie tramite il registry dei manifest.
- Fallback asset sempre disponibile se un modello dedicato manca o è incompatibile.

### Esempio di manifest prefab

```json
{
  "id": "snes_cartridge_pal",
  "category": "physical_media",
  "platforms": ["snes"],
  "geometry": "models/physical-media/snes-pal.glb",
  "dimensions_m": [0.125, 0.020, 0.085],
  "anchors": {
    "inspect": [0.0, 0.045, 0.0],
    "shelf": [0.0, 0.0, 0.0]
  },
  "texture_slots": {
    "front_label": "FrontLabelMaterial",
    "back_label": "BackLabelMaterial"
  },
  "fallback_label": true
}
```

Il manifest è esemplificativo: le dimensioni e i materiali definitivi richiedono misure e asset reali.

---

# 18. Requisiti non funzionali e budget prestazionali

## 18.1 Target iniziali di sviluppo

I seguenti numeri sono **obiettivi progettuali da misurare**, non prestazioni già dimostrate:

| ID | Metrica | Target iniziale | Metodo |
|---|---|---|---|
| NFR-01 | Avvio a catalogo già indicizzato | <= 5 s su PC di riferimento | Telemetria locale di test |
| NFR-02 | Ricerca catalogo 10.000 giochi | <= 100 ms per query indicizzata | Benchmark SQLite FTS5 |
| NFR-03 | Visualizzazione stanza standard | 60 FPS a 1080p su GPU dedicata di riferimento | Profiling rappresentativo |
| NFR-04 | Frame time 99° percentile | <= 33,3 ms in esplorazione standard | Tracy/RenderDoc |
| NFR-05 | Nuovo file ROM -> comparsa in catalogo | <= 10 s in condizione locale normale | Test watcher |
| NFR-06 | Focus game -> primo frame preview | <= 2 s, video locale pronto e decodificabile | Test UX |
| NFR-07 | Max decoder preview nel preset normale | 2 contemporanei | Assertion runtime |
| NFR-08 | Nuova ROM con slot disponibile | Nessun oggetto `pinned` spostato | Snapshot diff test |
| NFR-09 | Crash/chiusura durante scraping | Nessun media parziale visibile come definitivo | Fault injection |
| NFR-10 | Integrità metadata origine | Byte identici in modalità read-only | Hash before/after |

Hardware di riferimento definitivo, misure FPS e limiti VRAM saranno fissati dopo una prima scena con almeno una stanza popolata. I tempi su dischi USB, rete SMB e hard disk lenti potranno essere diversi e vanno documentati.

## 18.2 Concorrenza e reattività

- La UI deve restare utilizzabile durante scan, hashing e download.
- Nessuna chiamata HTTP o lettura di file video interi nel main loop.
- Task cancellabili con cleanup; nessun worker scollegato può accedere a risorse distrutte.
- `WorldPlan` trasferito come risultato completo/immutabile; la costruzione scena usa commit controllato.
- Gestione esplicita di out-of-memory, device/context failure e perdita temporanea dei volumi ROM.

## 18.3 Gestione grandi collezioni

Dimensionare e testare 100, 1.000, 10.000 e 30.000 entry, incluse sottocartelle e sistemi numerosi. `30.000` è una fixture di stress test, non un requisito di caricamento simultaneo di 30.000 oggetti in una stanza. Il mondo deve essere suddiviso in zone e caricato per prossimità.

## 18.4 Scalabilità dei media

I video non vengono precaricati integralmente. Le immagini ad alta risoluzione vengono richieste in funzione della visibilità e della distanza. Prevedere memoria cache limitata con politiche LRU/priority e una dashboard developer per osservare CPU, RAM, VRAM, draw call, decoder attivi, cache hit ratio e tempi import.

---

# 19. Robustezza, sicurezza e conformità

## 19.1 Input non fidati

Considerare non fidati XML, nomi file, metadata, media scaricati, path speciali e manifest esterni. Validare sempre lunghezza, formato, directory traversal e schemi di riferimento prima di usarli. Le chiamate a processi esterni non devono interpretare comandi shell arbitrari ricavati da metadati della libreria.

## 19.2 File e directory

- Supportare Unicode e percorsi Windows lunghi ove configurati.
- Gestire drive esterni rimossi, volume offline, permessi negati, junction e symlink senza cicli infiniti.
- Evitare cancellazioni automatiche di media quando una ROM scompare temporaneamente.
- Limitare la scansione a radici e regole di esclusione configurate.
- Backup prima delle scritture condivise e ripristino documentato.
- Se ES-DE è in esecuzione, ridurre o bloccare operazioni di scrittura concorrenti.

## 19.3 Dipendenze e DLL

SDL3, FFmpeg e altre DLL devono essere distribuite in versioni coerenti e controllate, con manifest di licenze e origini. Definire la scelta di linking e la relativa conformità alle licenze applicabili; la disponibilità tecnica di una libreria non elimina gli obblighi legali di distribuzione. Verificare policy aggiornamento sicurezza per parser e decoder.

## 19.4 Contenuti protetti

Retroverse non fornisce ROM, BIOS, cover, gameplay e manuali di terzi. Le risorse possono essere recuperate solo tramite fonti e account utilizzati nel rispetto delle relative condizioni e della legge. L'utente resta responsabile del possesso/uso dei contenuti. Nessun asset Nintendo, Sony o di altri produttori è incluso nel pacchetto senza titolo autorizzativo idoneo; per console protette si possono utilizzare modelli generici o asset concessi in licenza. Nomi reali, loghi e modelli delle console già presenti sulla macchina dell'utente (temi di ES-DE, cartella dei contenuti) sono mostrati a runtime come contenuto dell'utente e non entrano nel pacchetto (ADR-0019).

## 19.5 Privacy

L'app funziona offline e non raccoglie telemetria per impostazione predefinita. Le credenziali dello scraper vengono protette con API Windows, i log non includono password. Qualsiasi segnalazione automatica di crash o analytics deve essere opt-in, chiara e disattivabile.

---

# 20. Scenari end-to-end obbligatori

## SC-01 — Prima importazione ES-DE

**Dato** un ES-DE Windows con più sistemi, ROM e media già scaricati; **quando** l'utente sceglie il profilo ES-DE nel wizard; **allora** Retroverse rileva percorsi, importa catalogo e media, genera stanze e scaffali, visualizza i supporti e non copia né riscrive automaticamente i dati condivisi.

## SC-02 — Avvio di una cartuccia SNES

L'utente entra nella sala SNES, seleziona una cartuccia, ne vede fronte e metadata, sceglie `Gioca`; Retroverse avvia l'emulatore configurato con il percorso ROM corretto, cede il focus e, alla chiusura, torna nella stanza con camera e selezione preservate.

## SC-03 — TV con anteprima selezionata

Un CRT sta trasmettendo una playlist di giochi della stanza. L'utente focalizza una cartuccia. Il display interrompe o sospende la rotazione, mostra il video locale del titolo oppure un fallback, senza mandare in crash il renderer se il file non è supportato.

## SC-04 — Scaricamento di video mancanti

L'utente filtra una stanza PlayStation e sceglie `Solo video mancanti`, provider abilitato, regione e qualità. Vengono mostrate richieste previste e quota disponibile, la coda parte, i file sono salvati nella cartella `downloaded_media` corretta e la TV può utilizzare i video senza rigenerare la stanza.

## SC-05 — Aggiunta di ROM

Si copia una ROM in una sottocartella ES-DE. Il watcher rileva il cambiamento, il catalogo si aggiorna, l'oggetto appare in un nuovo slot. Nessun supporto precedentemente pinnato si sposta; nessuna configurazione dell'emulatore viene cambiata.

## SC-06 — Rimozione temporanea disco esterno

La directory ROM è su un'unità rimovibile che viene scollegata. Il programma segnala `Origine non disponibile`, non cancella definitivamente giochi, dati o stanze; al ritorno dell'unità e alla nuova scansione la collezione riappare con gli stessi ID.

## SC-07 — Doppio frontend

Un gioco viene rinominato e metadata aggiornati da ES-DE mentre Retroverse è chiuso. Alla successiva importazione, il gioco viene riconciliato per quanto possibile; i metadata vengono aggiornati. Qualsiasi ambiguità di identità viene mostrata e non causa la perdita silenziosa di pin e note personali.

## SC-08 — Video mancante o corrotto

Un video MP4 è assente, vuoto o corrotto. Il servizio video segna l'asset come non riproducibile, mostra uno screenshot/cover e continua l'aggiornamento 3D. L'utente può chiedere una nuova scansione o scaricare una sostituzione autorizzata.

## SC-09 — Modifica manuale mondo

Un utente sposta uno scaffale, mette la sua cartuccia preferita su un espositore e salva. Aggiunge altri giochi e aggiorna la libreria. Il generatore non sovrascrive i cambiamenti manuali, salvo consenso durante una rigenerazione esplicita.

## SC-10 — Launcher incompleto

Il gioco viene importato correttamente ma il comando ES-DE contiene un token non risolvibile. Retroverse presenta `Launcher non configurato`, non esegue comandi arbitrari, offre una procedura di scelta emulatore e registra il profilo quando il test ha successo.

## SC-11 — Sistema sconosciuto

La collezione contiene un sistema senza prefab. Retroverse genera comunque scaffale/area e supporto generico con etichetta, mantiene metadata e gioco avviabile se il profilo launcher è valido.

## SC-12 — Libreria di grandi dimensioni

Il catalogo contiene migliaia di giochi. Il primo caricamento avviene a zone; la ricerca globale è operativa senza materializzare tutto il mondo, e l'esplorazione non avvia automaticamente migliaia di decoder o upload texture contemporanei.

---

# 21. Strategia di test

## 21.1 Unit test

- Parsing `gamelist.xml`: Unicode, date, escape, field ignoti, nodi mancanti, `<folder>`, path relativi.
- Resolver media ES-DE: sottocartelle, omonimi, estensioni, directory configurata, formato fallback.
- Mapping sistemi: alias, platform manifest sconosciuto, formato fisico.
- Identità giochi: stable IDs, rinomina, spostamento, più dischi, file mancanti.
- Procedural planner: capienza, collisioni, connettività, determinismo con seed.
- Incremental placement: nuovi giochi, cancellazioni, pin, slot occupati.
- Profili emulatori: token supportati, quoting Windows, path con spazi e apostrofi, argomenti pericolosi.
- Job scraper: retry, pause/resume, limiti, match incerti e deduplica.
- File sync: conflitti, atomicità, campi sconosciuti, read-only.

## 21.2 Integration test

- Fixture ES-DE standard e portable.
- Fixture con `MediaDirectory` e ROM root custom.
- ES-DE con override sistemi e comando launcher non completamente risolvibile.
- Import di cartelle vuote, profonde, con nomi Unicode, `*.m3u`, `*.cue`, immagini e video mancanti.
- FFmpeg con MP4 H.264, clip breve, video senza audio, file troncato, aspect ratio diversi.
- Lanciare un executable di test che termina con successo, crash o spawn di processo figlio; verificare ritorno UI.
- Integrazione scraper tramite mock HTTP e fixture riproducibili; test live separati con credenziali lecite.

## 21.3 Snapshot / property-based test generatore

- Stesso input ordinato + stesso seed + stessa versione -> **stesso WorldPlan canonico**.
- Ogni `Game` attivo ha un placement valido o un elemento accessibile nel catalogo globale (se overflow esplicito).
- Ogni porta è connessa all'hub; ogni stanza generata è raggiungibile.
- Nessun prefab oltrepassa un volume escluso o si sovrappone a un altro arredo critico.
- Nessun pin cambia a seguito di una sola aggiunta ROM con capacità disponibile.
- Aggiungendo un gioco, il numero di game placements aumenta di uno salvo deduplica/gruppi previsti.
- Una rigenerazione non deve cancellare gli override senza un'azione esplicita.

## 21.4 Test manuali ed ergonomia

- Sessione di 30 minuti da controller senza toccare mouse o tastiera.
- Ricerca e lancio rapido dopo l'avvio a freddo.
- TV con due video simultanei in 1080p/60 FPS.
- Collezione con migliaia di cartucce in più stanze.
- Disconnessione USB, cambio risoluzione, Alt-Tab, emulatore fullscreen, monitor multipli.
- Accessibilità: disattivazione motion effects, FOV, navigazione con teletrasporto.

---

# 22. Fasi implementative e milestone

> L'ordine seguente è una roadmap di dipendenze tecniche, **non** una stima temporale. Ogni fase deve produrre software realmente verificabile, senza introdurre un'altra fonte autorevole per i dati.

## M0 — Fondamenta native C++

- CMake, vcpkg, SDL3, contesto OpenGL, glad, GLM, logging, profiler.
- Window loop, controller, basic camera, shader, mesh loading glTF.
- Una scena di prova con scaffale, supporto fisico e TV non ancora video.
- Test di build su Windows Debug/Release.

**Done:** app Windows avviabile, camera controllabile, asset rendering stabile, nessuna dipendenza da game engine.

## M1 — Catalogo ES-DE reale

- ES-DE discovery, parsing gamelist, directory ROM/media, SQLite index e refresh.
- Sistemi e gioco logico, resolver metadata/media, ricerca testuale.
- Launcher iniziale per almeno un emulatore configurabile.

**Done:** Retroverse legge una collezione ES-DE esistente senza spostare ROM; selezione titolo e avvio funzionano con processo esterno.

## M2 — Vertical slice procedurale

- Manifest SNES, uno stile stanza, topology minima, scaffali con slot.
- Posizionamento procedurale, texturing cartucce, picking, inspect, search/teleport.
- WorldPlan persistente e update incrementale su aggiunta ROM.

**Done:** una sala reale generata dai dati mostra tutte le cartucce del sistema, con stabilità dopo import incrementale.

## M3 — Mondo completo multi-piattaforma

- Room graph connesso, più sistemi, più modelli di media, postazioni e console.
- Hub, porte, collisioni, streaming stanza, override e editor leggero.
- Preset tematici e fallback per sistemi non mappati.

**Done:** collezione eterogenea generata automaticamente, navigabile e personalizzabile senza interventi manuali per singolo gioco.

## M4 — Schermi e media dinamici

- FFmpeg pipeline video, texture GL, playlist TV, shader CRT, audio.
- Scheduler dei decoder, focus screen, fallback e rilevamento media mancanti.
- Media manager con download locali e ScreenScraper **solo se accesso/API autorizzati**.

**Done:** ogni sala prevista contiene TV funzionanti; import e video esistenti si riproducono correttamente, con budget decoder rispettato.

## M5 — Prodotto completo desktop

- Wizard rifinito, configurazione emulatore, scraper UI, filtri, stato giochi, statistiche.
- Packaging installer/portable, update DB, diagnostic tools, error recovery.
- Profiling, benchmark, controller UX, test di regressione e compatibilità reale ES-DE.

**Done:** il frontend può essere utilizzato come alternativa quotidiana a ES-DE su Windows per una libreria e un insieme di emulatori validati.

## M6 — Espansioni successive

- Adapter ES legacy migliorato; RetroBat/Batocera specifici.
- Altri preset e oggetti interattivi, libreria di asset ampliata, mod.
- Multi-display avanzato, decoder accelerato. (La VR è anticipata all'iterazione I10: ADR-0017.)
- Possibile frontend su Linux dopo astrarre tutti i servizi Windows.

---

# 23. Tracciabilità requisiti e criteri di accettazione

I requisiti critici sono identificati con ID per poterli collegare a issue, test e milestone. `P0` = indispensabile, `P1` = importante, `P2` = estensione.

| ID | Priorità | Requisito | Test/criterio di accettazione |
|---|---|---|---|
| FR-001 | P0 | App Windows nativa C++20 + OpenGL | Avvia su Windows senza Godot/Unity/Unreal |
| FR-002 | P0 | Import dati ES-DE esistenti | Legge gamelist e ROM root install/portable |
| FR-003 | P0 | Nessuna duplicazione ROM | Percorsi condivisi, zero copia automatica |
| FR-004 | P0 | Utilizzo media ES-DE esistenti | Risolve cover/video dalla media directory configurata |
| FR-005 | P0 | Generazione automatica delle stanze | Collezione -> WorldPlan valido e navigabile |
| FR-006 | P0 | Cartucce/custodie contestuali | Ogni gioco ha modello per tipo o fallback |
| FR-007 | P0 | Scaffali dimensionati dalla collezione | Capienza e accessibilità validate |
| FR-008 | P0 | Layout deterministico | Stesso seed/config/catalogo -> stesso output |
| FR-009 | P0 | Aggiunte incrementali stabili | Pin e placements esistenti preservati |
| FR-010 | P0 | Ispezione supporto 3D | Focus/rotazione/interazione funzionano |
| FR-011 | P0 | Avvio emulatore esterno | Processo Windows avviato con ROM corretta |
| FR-012 | P0 | Ritorno da emulatore | Ripristino scena/input/sessione |
| FR-013 | P0 | Ricerca globale | Indice interrogabile senza caricare tutte le stanze |
| FR-014 | P0 | Controller completo | Wizard, catalogo, gioco, settings navigabili da pad |
| FR-015 | P0 | TV 3D con video locale | MP4 decodificato e mostrato come texture |
| FR-016 | P0 | Video mirato sul focus | Preview gioco selezionato o fallback |
| FR-017 | P0 | Limitazione decoder | Budget rispettato con schermi fuori vista |
| FR-018 | P0 | Gestione sicura XML | Nessuna modifica condivisa in read-only |
| FR-019 | P0 | Casi di file mancanti | Nessun crash; messaggio/fallback affidabile |
| FR-020 | P0 | Emulatori configurabili | Test e recupero per comandi ES-DE non risolti |
| FR-021 | P1 | Scraper con scelta tipi media | Utente controlla provider/ambito/tipi |
| FR-022 | P1 | Download in cartelle ES-DE | ES-DE può riutilizzare i media scaricati |
| FR-023 | P1 | Coda scraping persistente | Pausa/ripresa/errori/quote presenti |
| FR-024 | P1 | Preset di stanze | Cambiano stile senza cambiare catalogo |
| FR-025 | P1 | Editor arredo | Spostamenti salvati e preservati |
| FR-026 | P1 | Cronologia giochi | Sessioni e ultimi avvii accessibili |
| FR-027 | P1 | Statistiche personali | Query per sistema/stato/ore stimate |
| FR-028 | P1 | Fallback asset console ignota | Gioco navigabile senza prefab speciale |
| FR-029 | P1 | Hotplug controller | Reconnect senza restart |
| FR-030 | P1 | Video e volume spaziale | Regolazioni indipendenti e mute |
| FR-031 | P1 | Sync metadata opt-in | Backup, conflitti, preserve unknown fields |
| FR-032 | P1 | Modalità portable | Impostazioni e cache in posizione scelta |
| FR-033 | P1 | Gestione dischi scollegati | Nessuna cancellazione permanente in scansione |
| FR-034 | P2 | Multi-screen playlist avanzate | Playlist indipendenti e policy di priorità |
| FR-035 | P2 | Temi/community mod | Manifest asset di terzi validati |
| FR-036 | P2 | Adapter Batocera/RetroBat | Suite di compatibilità separata |
| FR-037 | P2 | VR | Specifica separata: ADR-0017 (I10), non cambia il desktop |

## 23.1 Definition of Done generale

Una funzionalità è «completa» quando: soddisfa l'acceptance test; gestisce input mancanti ed errori; funziona almeno con mouse/tastiera e controller se user-facing; ha logging pertinente; non introduce scritture non autorizzate sulla libreria; rispetta budget di memoria/thread; è coperta da test automatici dove la logica è deterministica; è documentata in italiano o inglese coerentemente al progetto.

---

# 24. Rischi progettuali e decisioni aperte

| Rischio | Impatto | Mitigazione |
|---|---|---|
| Incompatibilità tra ES-DE e varianti legacy | Import o launch errato | Adapter separati e fixture per versione |
| Interpretazione incompleta comandi ES-DE | Giochi non avviabili | Profile launcher verificato, wizard, test singolo |
| Video decode + OpenGL costoso | Stutter e memoria | Budget decoder, upload asincrono, fallback statico |
| World generation troppo casuale | Stanze illeggibili | Regole spaziali, validation, preset, seed |
| Aggiunte ROM distruggono il layout | Perdita personalizzazione | ID stabili, slots persistenti, pin/overrides |
| Librerie enorme con texture differenti | Saturazione VRAM | Streaming e LRU, atlanti, mipmap, LOD |
| API scraper non autorizzata | Funzione indisponibile | Autorizzazione ufficiale, adapter opzionale, locale |
| Licenze di modelli/cover non adatte | Distribuzione problematica | Manifest licenze e contenuti utente |
| XML modificati da più frontend | Perdita metadata | Read-only default, conflict detection, atomic write |
| Dipendenze C++ eterogenee | Costi integrazione | Pin versions, smoke tests e uso mirato delle librerie |
| File system USB/SMB | Import lento e offline temporaneo | Cache indice, timeout, rinvio hashing, status origine |
| Gameplay in TV 3D interpretato come emulazione | Scope creep | Preview video sì; emulazione embedded esclusa da v1 |

### Decisioni da chiudere prima della produzione

1. **Versioni ES-DE da certificare:** es. 3.5.x più fixture 3.4.x; evitare diciture generiche «tutte le versioni».
2. **Politica OpenGL baseline:** 4.5 raccomandato; decidere se il 3.3 è requisito reale o fallback best-effort.
3. **Renderer asset style:** realistic low-poly, stylized oppure realismo dettagliato; il generatore deve conoscere i set prefab.
4. **Piattaforme iniziali da supportare fisicamente:** partire da SNES/PS1/N64/PS2/GBA/arcade, espandendo tramite manifest.
5. **Licenza e distribuzione del software** e permessi asset.
6. **Accesso allo scraper ScreenScraper:** credenziali sviluppatore/autorizzazione prima dell'integrazione pubblica.
7. **UI framework:** validare RmlUi con SDL3/OpenGL, controller focus e packaging su Windows.
8. **Profilo video:** software decode iniziale; valutare hardware decode solo dopo benchmark.
9. **Compatibilità ES-DE write-back:** lasciare disattivata finché la suite di round-trip non è solida.

---

# 25. Riferimenti tecnici

Fonti di comportamento ES-DE e librerie (consultate per la specifica; i dettagli possono cambiare con gli aggiornamenti):

1. **ES-DE ufficiale:** https://es-de.org/ — versioni, piattaforme, funzionalità.
2. **ES-DE User Guide:** https://gitlab.com/es-de/emulationstation-de/-/blob/master/USERGUIDE.md — media, scraper, struttura della collezione, impostazioni.
3. **ES-DE Install Guide:** https://gitlab.com/es-de/emulationstation-de/-/blob/master/INSTALL.md — sistemi, XML, percorsi, configurazioni.
4. **ES-DE es_systems.xml (esempio Windows, mirror):** https://github.com/RetroDECK/ES-DE/blob/retrodeck-main/resources/systems/windows/es_systems.xml — struttura comandi, token, estensioni.
5. **EmulationStation legacy:** https://github.com/Aloshi/EmulationStation — backend separato.
6. **ScreenScraper API:** https://www.screenscraper.fr/webapi2.php — accesso sviluppatori, termini e tipologie media.
7. **SDL3 SDL_GL_CreateContext:** https://wiki.libsdl.org/SDL3/SDL_GL_CreateContext — contesto OpenGL e thread safety.
8. **FFmpeg codec send/receive:** https://ffmpeg.org/doxygen/trunk/group__lavc__encdec.html — modello di decoding.
9. **OpenGL reference:** https://registry.khronos.org/OpenGL/ — API e specifiche.
10. **Khronos glTF:** https://www.khronos.org/gltf/ — formato asset 3D.
11. **SQLite:** https://www.sqlite.org/docs.html — persistenza embedded.
12. **CMake:** https://cmake.org/documentation/ — build e generatori.

---

# 26. Sintesi architetturale conclusiva

**Retroverse è un frontend Windows nativo C++20/OpenGL, non un motore di emulazione e non un insieme di scene 3D precostruite.** La collezione ES-DE resta il riferimento per ROM, metadati e media. Un adapter la normalizza in un catalogo; un world generator puro e deterministico traduce il catalogo in una topologia persistente di stanze, arredi e oggetti; il renderer OpenGL materializza le zone visibili; FFmpeg fornisce frame ai televisori virtuali; lo scraper recupera in modo opzionale media mancanti nella directory condivisa; il launcher controlla gli emulatori esterni e riporta l'utente nella scena alla loro chiusura.

Il progetto raggiunge il suo obiettivo quando è piacevole passeggiare fra gli scaffali e osservare i televisori, ma selezionare un gioco rimane veloce quanto in un launcher tradizionale. La rappresentazione tridimensionale deve valorizzare la libreria esistente, **mai diventare una condizione per potervi accedere**.
