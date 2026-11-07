# RETROVERSE — Art Bible e specifica dello stile del mondo 3D

**Versione:** 1.0
**Data:** 14 ottobre 2026
**Tipo:** documento di visual design, technical art, asset sourcing e world building
**Piattaforma primaria:** Windows x64
**Tecnologia:** C++20, OpenGL 4.5 (eventuale fallback 3.3), SDL3, glTF/GLB, FFmpeg
**Documento complementare:** `Retroverse_Specifica_Completa_Cpp_OpenGL.md`
**Stato:** specifica progettuale, non descrizione di funzionalità già implementate.

---

## 0. Decisione artistica vincolante

Lo stile di riferimento è quello del concept grafico già selezionato: **un rifugio/museo del retrogaming, caldo, ricco, vissuto e leggermente idealizzato**, che unisce la cameretta del collezionista degli anni Novanta, una boutique di videogiochi, una sala arcade e un museo interattivo.

L'obiettivo **non** è riprodurre l'immagine fotorealisticamente, né creare un megastore impersonale. L'obiettivo è ottenere nel programma un mondo 3D coerente che evochi immediatamente le stesse sensazioni: piacere di esplorare, scoperta, nostalgia, valore degli oggetti da collezione e voglia di fermarsi a guardare un televisore.

**Formula estetica:** `Cozy collector room × curated retro museum × cinematic lighting × interactive arcade`.

### Vincoli non negoziabili

1. Tutto il mondo è **esplorabile in tempo reale**, a scala umana, non uno sfondo prerenderizzato.
2. Le **stanze sono generate proceduralmente** in funzione dei sistemi e dei giochi presenti nella libreria.
3. I modelli di cartucce, custodie, console e mobili derivano da **prefab riutilizzabili**; non si realizza manualmente un ambiente per ogni collezione.
4. Ogni stanza ha **almeno un punto focale leggibile**: console + TV, cabinato arcade, espositore speciale o vetrina.
5. Le **anteprime video** sono visibili su schermi integrati fisicamente nelle stanze.
6. Il mondo **non deve risultare vuoto** se mancano asset o cover: esistono sempre fallback offline inclusi nell'app.
7. La direzione artistica è controllata da **regole di coerenza**: acquistare decine di modelli eterogenei senza uniformarli non è una strategia accettabile.
8. Tutte le funzioni rimangono accessibili anche dal **launcher rapido** e senza animazioni obbligatorie.
9. **Nessuna dipendenza da Godot, Unity, Unreal o servizi cloud** per generare o renderizzare le stanze.
10. Tutti gli asset esterni devono avere provenienza e licenza registrate, prima dell'inclusione nel programma distribuito.

---

# 1. Traduzione dell'immagine di riferimento in regole riproducibili

Il concept approvato mostra sette aree/viste. Le indicazioni seguenti trasformano quelle immagini in elementi costruibili.

| Elemento del concept | Traduzione per il prodotto | Priorità |
|---|---|---|
| Hub centrale con ingressi per le console | Sala comune ampia, landmark centrale, accessi etichettati alle sezioni | P0 |
| Sala SNES, calda e piena di scaffali | Pareti di legno/scaffali scuri, tappeto, CRT, console centrale, luci calde e accenti colorati | P0 |
| Cartuccia/custodia esaminata davanti allo schermo | Modalità ispezione con camera ravvicinata, modello 3D, scheda informazioni leggibile | P0 |
| Sala PlayStation, luci blu, poster, vetrine | Stesso linguaggio architettonico, diverso *theme pack* e un CRT protagonista | P0 |
| Area arcade con neon e cabinati | Stanza con geometria dei cabinet, attract video, illuminazione più contrastata | P1 |
| Stanza delle console portatili con vetrine | Tavoli e display bassi, supporti più piccoli, esposizione museale | P1 |
| Giardino/area relax con menu | Spazio opzionale per raccolte, preferiti, impostazioni e pause | P2 |

**Regola interpretativa:** le scritte e i loghi di marchi presenti nel concept sono riferimenti visivi, non un'autorizzazione a includerli nei binari o nell'asset pack. I pannelli distribuiti con Retroverse devono usare grafica originale/neutra oppure risorse per cui esistono diritti adeguati. Le copertine personali già presenti nella libreria dell'utente sono un caso di contenuto caricato dall'utente, da gestire separatamente. Lo stesso vale per l'identità dei sistemi (nomi reali, loghi, colori e descrizioni letti a runtime dai temi di ES-DE; modelli 3D delle console nella cartella `<user data>/content`): è contenuto dell'utente, mai copiato in `assets/`, nelle fixture o nei golden e mai distribuito (ADR-0019).

## 1.1 Cosa deve comunicare una screenshot di Retroverse

Una schermata valida, anche senza UI, dovrebbe far capire in meno di cinque secondi:

- che il luogo è dedicato ai videogiochi classici;
- che ci sono oggetti fisici collezionabili e riconoscibili;
- che esiste uno schermo acceso o un altro centro d'attenzione;
- quale stanza/piattaforma si sta visitando;
- dove si può camminare e con quali oggetti interagire.

## 1.2 Cosa evitare

- Un magazzino di scaffali perfettamente identici.
- Neon saturi ovunque, senza contrasto con superfici calde.
- Sale bianche da showroom contemporaneo.
- Un 3D *low-poly cartoon* purissimo: può essere una modalità prestazionale, non l'estetica principale.
- Disordine casuale tale da nascondere la collezione.
- Materiali iperrealistici ma incompatibili fra loro per illuminazione, scala o color grading.
- Poligoni e luci insufficienti a rendere leggibile una cartuccia da vicino.
- Stanze identiche alle quali viene cambiato soltanto il colore.
- Uso massiccio di immagini AI come texture di oggetti storici precisi: per la fedeltà hardware usare riferimento, modellazione o asset appropriati.

---

# 2. Direzione artistica: *stylized realism premium*

## 2.1 Livello di realismo

Si adotta un **realismo stilizzato**:

- proporzioni credibili;
- silhouette dei videogiochi e delle console riconoscibili;
- materiali PBR, ma deliberatamente semplificati;
- dettagli concentrati vicino a TV, scaffali, console e punti interattivi;
- ambienti più ordinati e scenografici delle stanze reali;
- uso controllato di luci accentuate, bloom e contrasti cromatici;
- imperfezioni leggere (usura, impronte, bordi consumati), evitando un'estetica sporca.

Un oggetto visto a 2 metri deve apparire plausibile; a 10 metri deve essere facilmente distinguibile dalla sua silhouette, non dal numero di dettagli modellati.

## 2.2 Tre scale di dettaglio visivo

| Livello | Distanza/uso indicativo | Regola |
|---|---|---|
| Hero | 0–2 m; oggetto selezionato, TV, console principale | Mesh e texture curate; leggere animazioni; leggibilità massima |
| Ambient | 2–8 m; mobili, ripiani e apparecchiature | Mesh e texture moderate, ripetibili e istanziabili |
| Background | Oltre 8 m; pareti secondarie, scatole e decorazioni | Geometria semplice, texture atlas, eventuale billboard/LOD |

I valori di distanza sono indicativi: il LOD effettivo dipende dalla proiezione a schermo, dalla risoluzione e dalla velocità della camera.

## 2.3 Strategia di coerenza visiva

Per ogni nuovo asset acquisito si applica almeno questa normalizzazione:

1. scala e pivot;
2. orientamento e convenzioni degli assi;
3. quantità di dettagli compatibile con la categoria;
4. gamma cromatica e materiali;
5. roughness e intensità delle superfici emissive;
6. eventuale usura;
7. ombra di contatto e integrazione nella luce di scena;
8. naming e collision proxy;
9. prestazioni e LOD;
10. registro di origine/licenza.

Gli asset non normalizzabili non entrano nella libreria ufficiale: si usa un altro modello o un fallback procedurale.

---

# 3. Palette, materiali e cromia

I colori seguenti sono **valori iniziali di art direction**, non valori estratti scientificamente dall'immagine. Si calibrano nel rendering reale.

| Ruolo | Colore suggerito | Codice | Impiego |
|---|---|---|---|
| Legno principale | Noce scuro caldo | `#493024` | Scaffali, rivestimenti |
| Legno secondario | Rovere medio | `#76513A` | Cornici, ripiani alternativi |
| Pareti | Grigio-bruno fumoso | `#383735` | Superfici di sfondo |
| Metallo strutturale | Grafite | `#252A2E` | Ringhiere, montanti, cabinet |
| Tessuti | Bordeaux desaturato | `#683D42` | Divani e tappeti |
| Luce tungsteno | Ambra morbida | `#FFC88A` | Applique, lampade, faretti |
| Accento freddo | Blu elettrico moderato | `#4C8BC9` | Alcune sale/segni luminosi |
| Accento arcade | Magenta/viola | `#B34DCE` | Sala arcade, LED mirati |
| Accento segnaletica | Verde desaturato | `#6EAF93` | Indicazioni e display |
| UI neutra | Carbone traslucido | `#171B22` | Pannelli contestuali |
| UI testo | Avorio chiaro | `#F4EFE6` | Titoli e metadati |

## 3.1 Regola 70/20/10

- Circa **70%** dell'ambiente visivo: materiali neutri e caldi, poco saturi.
- Circa **20%**: dettagli, colori legati all'epoca o al tema della stanza.
- Circa **10%**: accenti luminosi, schermi, insegne e oggetti che devono attirare lo sguardo.

Non utilizzare l'illuminazione colorata come sostituto di geometria interessante o composizione.

## 3.2 Material library comune

Costruire un set condiviso di circa 15–25 materiali base, personalizzabili con parametri:

- `MAT_Wood_Walnut`, `MAT_Wood_Oak`, `MAT_Wood_Varnished`;
- `MAT_Concrete_Warm`, `MAT_Wall_Painted`, `MAT_Plaster_Used`;
- `MAT_Metal_Graphite`, `MAT_Metal_Brushed`;
- `MAT_Plastic_Black`, `MAT_Plastic_Gray`, `MAT_Plastic_AgedIvory`;
- `MAT_Glass_Display`, `MAT_Glass_CRT`;
- `MAT_Fabric_Sofa`, `MAT_Carpet_Ornate`;
- `MAT_Cardboard_Box`, `MAT_Paper_Print`;
- `MAT_Neon_Diffuser`, `MAT_Screen_Emissive`.

Ogni materiale ha preset base di *baseColor*, *metallic*, *roughness*, normal map opzionale, AO e emissive. Non assegnare nuove texture 4K a ogni mobile generato: preferire varianti parametriche e atlanti.

## 3.3 Invecchiamento moderato

Aggiungere macro-imperfezioni leggere: bordi consumati, piccole variazioni di rugosità, polvere minima, scolorimento della plastica, differenze fra ripiani. Le imperfezioni sono meglio gestite in un materiale condiviso o decal atlas piuttosto che richiedere mesh uniche.

---

# 4. Illuminazione e atmosfera

## 4.1 Obiettivo fotografico

L'ambiente deve sembrare illuminato da **lampade calde e schermi realmente accesi**. Le luci fredde servono a differenziare le stanze, non a prevalere sull'illuminazione generale.

Stratificazione:

1. **Ambient/fill:** illuminazione diffusa proveniente da finestre, corridoi o IBL.
2. **Practical lights:** lampade a parete, sospensioni, plafoniere, abat-jour.
3. **Accent lights:** spot sulle vetrine e sui pezzi speciali.
4. **Emissive surfaces:** monitor, insegne, LED, elementi decorativi.
5. **Contact/shadow cues:** AO, ombre dinamiche limitate o lightmap precomputate.

## 4.2 Preset iniziali

| Zona | Tinta dominante | Luci d'accento | Risultato |
|---|---|---|---|
| Hub | Ambra | Blu tenue, rosso e verde piccoli | Grandioso ma accogliente |
| SNES / 16-bit | Tungsteno | Magenta lieve | Cameretta anni '90 |
| N64 | Calda | Verde/blu/rosso tenui | Collezionismo ludico |
| PS1/PS2 | Ambra scura | Blu elettrico tenue | Lounge/museo primi 2000 |
| Portatili | Neutro caldo | Verde e blu soft | Vetrine di oggetti piccoli |
| Arcade | Caldo scuro | Magenta, ciano, rosso più forti | Energia e profondità |
| Area relax | Tramonto caldo | Luci soffuse | Zona di decompressione |

## 4.3 Configurazione tecnica OpenGL

- **Prima implementazione:** forward shading PBR semplificato, una o poche shadow map selezionate, AO leggero, light probes/IBL, emissive e bloom localizzato.
- **Non fare affidamento su GI real-time**: l'apparenza può essere raggiunta con illuminazione statica/baked, probe e piccoli trucchi artistici.
- **sRGB corretto:** decodifica delle albedo sRGB; calcoli luce in spazio lineare; tonemapping e output sRGB verificati.
- **Post-processing:** tonemapping filmic moderato (ad es. una curva ACES-like verificata), bloom leggero, vignette minima opzionale.
- **Luci importanti:** quelle realmente visibili e necessarie alla lettura degli spazi hanno priorità sulle luci puramente decorative.
- **Schermi:** video emissivi su mesh; eventuale point/area-light fittizia vicino alla TV con colore medio aggiornato lentamente, mai una luce costosa per ogni pixel del video.
- **Interni:** considerare occlusione per stanza o settore; nessun bisogno di renderizzare luci e oggetti del museo intero in ogni frame.

### Profili

- `Low`: IBL semplice, luci dinamiche drasticamente limitate, niente ombre complesse, video limitati, AO cheap.
- `Balanced`: shadow map principale, AO, bloom sobrio, qualità base consigliata.
- `High`: ombre aggiuntive mirate, riflessi fittizi, maggior dettaglio materiale e risoluzione media.

**Criterio artistico:** il profilo `Low` mantiene palette e composizione; riduce dettagli tecnici senza trasformare il mondo in un'ambientazione diversa.

---

# 5. Architettura visiva del mondo

## 5.1 Organizzazione globale

Generare un mondo composto da:

- un **Hub** comune e persistente, luogo d'ingresso e orientamento;
- una o più **Theme Zones** raggruppate per produttore, generazione o uso;
- una o più **Rooms** per i sistemi presenti;
- eventuali **Expansion Rooms** per collezioni molto ampie;
- spazi speciali opzionali: arcade, portatili, sala delle rarità, relax.

Il sistema non crea automaticamente una stanza minuscola per ogni piattaforma con tre giochi: può aggregare sistemi vicini in una zona comune, preservando identità e indicazioni.

## 5.2 Hub principale

**Mood:** museo-biblioteca del retrogaming, con legno caldo, travi/montanti scuri e un landmark visibile appena entri.

Elementi obbligatori:

- punto di ingresso chiaro;
- almeno tre uscite tematizzabili quando ci sono abbastanza sistemi;
- mappa/terminal di ricerca accessibile;
- landmark centrale: globo luminoso *originale*, orologio videoludico, vetrina o installazione modulare;
- vista parziale delle aree vicine;
- uno o due schermi informativi non invadenti;
- area libera di movimento.

**Dimensioni iniziali proposte:** diametro equivalente 18–24 m oppure pianta rettangolare circa 18 × 14 m; altezza prevalente 4–6 m. Non sono misure obbligatorie: sono intervalli per il prototipo visivo, da calibrare con camera e collisioni.

**Da costruire con prefab:** pavimento, arco/portale, segmenti di parete, pilastri, lampade, ringhiere, insegne, vetrina e sedute. Evitare un modello monolitico dell'intero hub: sarebbe poco adattabile alla generazione.

## 5.3 Template stanza console

Dimensioni di partenza: stanza media circa **9 × 10 m**, altezza **3–3,6 m**; cresce in moduli di 2–3 m o tramite stanze di espansione.

Zone interne funzionali:

- **Ingresso/wayfinding**: nome della famiglia, visibilità immediata di un punto d'interesse.
- **Hero wall**: televisore, console e seduta o tavolino.
- **Collection wall**: scaffali di giochi, supporti e custodie.
- **Museum accent**: una vetrina, un poster o un espositore.
- **Open circulation**: corridoio principale e vie di accesso libere.
- **Expansion sockets**: posizioni riservate per nuovi scaffali e ampliamenti.

Il generatore deve poter creare sale rettangolari, L-shaped e lounge con alcove. Le varianti si basano su una stessa grammatica geometrica, non su room mesh scollegate.

## 5.4 Area arcade

- Struttura lineare o a due file di cabinet.
- Almeno un cabinato con schermo attivo se sono presenti media video.
- Insegne neon generiche/locali; niente artwork commerciali incorporati senza diritti.
- Materiali più scuri; frequenza dei colori accentati più alta.
- Composizione che consenta di distinguere ogni cabinet, evitando pareti dense di monitor indistinguibili.
- Mobile `CabinetGeneric` sempre disponibile; skin personalizzabili tramite media locale dell'utente dove consentito.

## 5.5 Area console portatili

- Stands e vetrine ad altezza osservabile.
- Supporti piccoli ingranditi in modalità ispezione.
- Illuminazione da museo, meno contrastata dell'arcade.
- Schermi LCD o piccoli CRT destinati alle anteprime, senza simulare obbligatoriamente un gioco all'interno del display della console portatile.

## 5.6 Stanze molto piccole o molto grandi

| Dimensione della collezione per sistema | Regola consigliata |
|---|---|
| 1–10 giochi | Espositore/angolo in una stanza condivisa |
| 11–50 | Stanza piccola o sezione dedicata |
| 51–200 | Stanza media con pareti multiple di collezione |
| 201–600 | Stanza grande o due aree collegate |
| oltre 600 | Più sale di espansione raggruppate, non una parete infinita |

Questi numeri sono **euristiche configurabili**; la vera capienza dipende dal tipo di oggetto, dall'orientamento e dalla densità scelta.

---

# 6. Composizione spaziale e navigazione

## 6.1 Regole di leggibilità

- Da ogni ingresso si deve vedere almeno una *hero object cluster*.
- Il primo televisore principale deve emergere dalla composizione grazie a luce e contrasto, non grazie a un HUD obbligatorio.
- Non porre vetrine, piante o sedute lungo l'asse principale di circolazione.
- Evitare scaffali che sembrano pareti invalicabili.
- Introdurre piccoli *negative spaces* per consentire alla vista di riposare.
- Diversificare ritmo e altezza dei mobili: libreria alta, credenza bassa, TV, vetrina, poster.

## 6.2 Misure di partenza

Assunzione: **1 unità mondo = 1 metro; asse Y verticale**.

| Elemento | Valore iniziale |
|---|---:|
| Camera player in piedi | 1,60–1,70 m |
| Larghezza passaggio principale | 1,40–1,80 m |
| Larghezza minima libera proposta | 1,10 m (da validare con collisioni) |
| Spazio libero davanti allo scaffale interattivo | 1,10–1,50 m |
| Altezza scaffale principale | 1,70–2,30 m |
| Profondità scaffale | 0,30–0,45 m |
| TV su mobile | Centro schermo circa 1,00–1,40 m |
| Altezza vetrina bassa | 0,80–1,10 m |
| Distanza di ispezione contestuale | 0,60–1,20 m |
| FOV orizzontale desktop | 70–85°, configurabile |

Le misure non sono norme edilizie: sono target di navigazione virtuale. Camera, velocità e collision radius vanno testati con gamepad e mouse.

## 6.3 Densità decorativa

Ogni stanza riceve un parametro `decoration_density` da 0 a 1:

- `0.00–0.25`: essenziale, pochi props, collezione in primo piano.
- `0.25–0.60`: riferimento raccomandato, *cozy curated*.
- `0.60–0.85`: ricca, adatta a collezioni grandi.
- `0.85–1.00`: vetrina/esposizione speciale, da usare con cautela.

La densità non deve mai ridurre la larghezza libera sotto i vincoli di navigazione.

---

# 7. Asset sourcing: modello, acquisto, creazione o generazione

## 7.1 Principio centrale: asset ibridi

Non tentare di generare da zero tutte le console realistiche con primitive OpenGL, e non tentare neppure di acquistare un modello 3D per ogni stanza. La strategia corretta è **ibrida**:

1. **Procedurale:** pareti, pavimenti, moduli degli scaffali, ripiani, casse semplici, cornici, pannelli, espositori, cartucce generiche, signage parametrico.
2. **Modellazione proprietaria in Blender:** asset speciali ricorrenti (TV CRT, console neutre, un set di chassis cartuccia, lettore CD, cabinato arcade, landmark, portal set).
3. **Asset di terzi verificati:** piante, sedute, lampade, tappeti, oggetti ambientali, alcune console accurate se utilizzabili.
4. **Texture dalla libreria ES-DE:** cover, screenshot, video, etichette/physicalmedia da applicare a geometrie progettate.
5. **Fallback di categoria:** oggetto 3D plausibile e neutro anche quando un asset specifico non è disponibile.

## 7.2 Fonti consigliate e condizioni d'uso

| Fonte | URL | Utilizzo prioritario | Licenza / cautela |
|---|---|---|---|
| **Poly Haven** | https://polyhaven.com/ | Materiali PBR, HDRI, arredo e piccoli props | Asset CC0 dichiarati dal provider; molto adatti al realismo stilizzato |
| **ambientCG** | https://ambientcg.com/ | Texture per legno, tessuti, muri, metallo, PBR | CC0 dichiarato dal progetto; verificare la pagina del singolo asset |
| **Kenney** | https://kenney.nl/assets | Fallback 3D, kit modulari, props neutri, prototipazione | Asset pubblicati sulle pagine del catalogo CC0, verificare pacchetto |
| **Quaternius** | https://quaternius.com/ | Props/kit stilizzati, eventuali arredi di background | **QAL v1.0 dal 28/08/2026**, NON assumere CC0: consentito incorporare in un prodotto, vietata la ridistribuzione come asset standalone |
| **Fab** | https://www.fab.com/ | Modelli speciali, arredi, computer/TV di qualità | Controllare **licenza del singolo asset** e condizioni Standard/CC-BY/Reference-Only; comprare solo se esportabile/convertibile |
| **Sketchfab** | https://sketchfab.com/ | Console storiche, CRT e accessori difficili | Molte licenze possibili: CC con attribuzione, restrizioni editoriali, altri diritti; non ogni modello è scaricabile/usabile nel prodotto |
| **Modellazione propria** | Blender: https://www.blender.org/ | Prefab essenziali e stili coerenti | Nessun vincolo di terzi, salvo texture/riferimenti incorporati |

**Importante — Quaternius:** la licenza pubblica attuale è `Quaternius Asset License (QAL) v1.0`, aggiornata il 28 agosto 2026, e non va etichettata automaticamente come CC0. La distribuzione di un programma che incorpora asset può essere permessa, ma non una raccolta di file sorgente 3D separata: archiviare versione e testo di licenza acquisiti.

**Importante — Fab:** la licenza Standard permette l'incorporazione in progetti e non limita l'uso a Unreal Engine, ma non permette di redistribuire l'asset come prodotto standalone. *Reference-Only* non equivale a ricevere il file sorgente. Verificare i termini al momento dell'acquisto.

**Importante — Sketchfab:** non creare un downloader non autorizzato di asset 3D; eventuali funzioni integrate devono seguire le API/condizioni del servizio e i permessi specifici del modello.

### Ricerca concreta: parole chiave

Usare ricerche mirate (in inglese, principalmente) e verificare la licenza prima di scaricare:

- `retro CRT television 3d model gltf low poly realistic`
- `1990s game room furniture 3d model modular shelf`
- `vintage entertainment center tv stand glb`
- `wood bookshelf modular 3d game asset`
- `retro video game controller game console 3d model license`
- `arcade cabinet blank 3d game ready`
- `record store interior modular assets`, `museum exhibition display case 3d`
- `desk lamp 1990s low poly pbr`, `rug persian low poly pbr`
- `retro wall panel industrial interior modular`, `neon sign blank 3d`

Per ogni modello candidato aprire preview, controllare poligoni/materiali, disponibilità del download, pivot, animazioni necessarie, qualità delle UV e licenza: **l'aspetto nella thumbnail non basta**.

## 7.3 Gerarchia di qualità/fallback obbligatoria

Ogni tipo di oggetto ha sempre tre livelli di disponibilità:

| Tier | Asset | Esempio: console | Quando usarlo |
|---|---|---|---|
| **A — Signature** | Modello dedicato, verificato e normalizzato | Chassis accurato di una specifica console, se disponibile e autorizzato, oppure importato dall'utente nella propria cartella dei contenuti (ADR-0019 §7) | Hero display, ispezione ravvicinata |
| **B — Family** | Modello originale generico per famiglia | `ConsoleCartridge90s`, `ConsoleOpticalDisc` | Nessun modello dedicato valido, sistema meno comune |
| **C — Procedural** | Mesh generata da primitive e materiale base | Corpo estruso, slot, pulsanti e pannello dati | Asset mancante, corrotto, incompatibile, profilo Low |

Gli oggetti Tier C devono mantenere **identità di categoria**: una console a cartucce non deve diventare una semplice sfera o un cubo anonimo. Aggiungere tipologia di slot, forma generale e targhetta testuale generata.

### Algoritmo di risoluzione asset

```text
resolvePrefab(request):
    key = canonicalAssetKey(request)
    if registry.signature[key] exists and passes validation/license/distribution:
        return registry.signature[key]
    familyKey = resolveFamily(request.mediaType, request.era, request.deviceClass)
    if registry.family[familyKey] exists:
        return registry.family[familyKey]
    return proceduralFactory.make(familyKey, deterministicSeed(key))
```

**A runtime** non serve reinterpretare la licenza di un asset: il controllo delle licenze appartiene a una fase di acquisizione/build e a un registro. A runtime si caricano solo asset già approvati o quelli importati esplicitamente dall'utente.

## 7.4 Priorità per le risorse

**Creare internamente subito:** muro/pavimento/soffitto, archi, scaffali modulari, ripiani, cartucce per famiglia, custodie/dischi, TV CRT generica, supporto console, segnaletica e frame espositivi.

**Cercare o acquistare in seguito:** poltrone particolari, tappeti, piante, lampade, decorazioni iconiche, console/hardware storicamente accurati, cabinati particolari.

**Non cercare un modello per singola ROM:** tutte le ROM SNES, per esempio, usano la stessa famiglia di mesh con texture/metadata diversi.

---

# 8. Catalogo iniziale dei prefab

Il set seguente è la **libreria minima di costruzione**; i prefabs non sono stanze complete.

| ID prefabs / famiglia | Asset principali | Origine preferita | Fallback |
|---|---|---|---|
| `architecture/wall_*` | Parete piena, con porta, finestra, nicchia, angolo | Procedurale / mesh proprietaria | Quads e boxes |
| `architecture/floor_*` | Parquet, moquette, mattonelle, soppalco | Procedurale + materiale tileable | Piano texturizzato |
| `architecture/portal_*` | Portale, insegna, architrave | Procedurale / Blender | Portale rettangolare |
| `furniture/shelf_modular` | Montanti, ripiani, retro, cornice, terminali | Blender + composizione | Scaffale a boxes |
| `furniture/tv_cabinet` | Credenza TV, console drawer, tavolino | Blender / terzi | Mobile a tre boxes |
| `furniture/seating` | Poltrona, divano, sgabello | Terzi normalizzati | Seduta neutra |
| `furniture/display_case` | Teche e vetrine | Blender/procedurale | Basamento + vetro semplificato |
| `electronics/crt_4_3` | TV da 14–29 pollici, profili multipli | Blender | Schermo 4:3 in cabinet generico |
| `electronics/lcd_16_9` | Monitor più moderno | Procedurale | Rettangolo con cornice |
| `electronics/stereo_*` | Impianto, casse, radio | Terzi/Blender | Casse + pannelli |
| `hardware/console_*` | Console per famiglia/varianti | Blender/terzi autorizzati | `GenericConsole*` |
| `hardware/controller_*` | Joypad/periferiche | Blender/terzi | `GenericController*` |
| `media/cartridge_*` | Cartridge SNES/N64/NES/GB/GBA/MD ecc. | Blender parametrico | Cartridge box sagomata |
| `media/case_*` | Jewel case, DVD, UMD, longbox, blister | Blender parametrico | Prism + texture front/dorso |
| `media/disc_*` | CD/DVD/miniDVD/UMD | Procedurale | Disco + label |
| `arcade/cabinet_*` | Cabinet upright, bartop, cocktail | Blender | Corpo estruso + schermo |
| `decor/lighting_*` | Spot, lampade e appliques | Terzi/Blender | Emissive light fixture |
| `decor/poster_frame` | Poster e cartelli incorniciati | Procedurale | Piano con cornice |
| `decor/rug_*` | Tappeti con varianti | Piano + texture | Plane texturizzato |
| `decor/plant_*` | Pianta in vaso | Terzi | Vaso + fogliame semplice |
| `decor/neon_sign` | Testo/logo originale emittente | Generazione geometria/text | Pannello emissivo |
| `special/hub_landmark` | Globo/teca/forma simbolica centrale | Mesh proprietaria | Sfera luminosa + piedistallo |

## 8.1 Set minimo per ottenere una prima stanza credibile

Una stanza SNES riconoscibile può essere costruita già con:

1. 4 moduli di parete, pavimento e soffitto;
2. una libreria modulare in 3 varianti dimensionali;
3. un CRT originale;
4. un mobile TV;
5. un prefab generico console a cartucce;
6. un prefab cartuccia adatto;
7. un tappeto e una lampada;
8. 2–3 quadri/poster neutri;
9. due profili di illuminazione;
10. un televisore alimentato da video ES-DE.

Il successo del primo ambiente non deve dipendere dal possesso di una collezione di asset a pagamento.

---

# 9. Convenzioni standard per i modelli 3D

## 9.1 Formato e unità

- **Scena di authoring:** Blender `.blend` (oppure formato sorgente equivalente documentato).
- **Formato runtime:** preferire **glTF 2.0 / GLB**.
- **Unità:** metri.
- **Assi runtime:** `+Y` verso l'alto, `+Z` verso la camera/front di riferimento secondo convenzione stabilita nel loader, `+X` verso destra; adattamento Blender/glTF verificato in test automatico.
- **Origine:** a pavimento per mobili/console esposte; pivot centrato e documentato per cartucce/dischi ispezionabili.
- **Transforms:** scale applicate/normalizzate prima della pubblicazione; evitare scale non uniformi accidentali.
- **UV:** UV0 presente se una texture è necessaria; UV semplificate e ripetibili per props decorativi.
- **Tangenti:** fornite o ricostruite se il materiale usa normal map.
- **Collision:** AABB/OBB o mesh semplificata separata; mai collisione per triangolo su ogni custodia.
- **Materiali:** metallic-roughness PBR standard; estensioni opzionali solo dopo supporto e test nel loader.
- **Animazioni:** clip nominate e limitate al necessario: `lid_open`, `disc_insert`, `cartridge_insert`, `screen_power_on`.

## 9.2 Convenzioni semantiche

Esempio nomi nodi:

```text
CRT_29inch.glb
  Root
    Body
    ScreenSurface       # superficie a cui associare video
    PowerButton
    SpeakerLeft
    SpeakerRight
    ColliderProxy
    Anchor_TV_Stand     # riferimento base
    Anchor_ScreenLook   # target interazione
```

Nodi `Anchor_*` possono essere letti nel tool di import e convertiti in *socket metadata*. Se il tool di ottimizzazione elimina nodi vuoti, la pipeline deve **estrarre e salvare gli anchor prima dell'ottimizzazione**.

## 9.3 Perché GLB

Un singolo `.glb` riduce errori di percorso e rende controllabili mesh/materiali/texture. Il formato glTF 2.0 è specificato da Khronos e progettato per trasferimento efficiente di asset 3D; l'uso di alcune estensioni richiede comunque supporto esplicito nel loader C++.

---

# 10. Procedura concreta per reperire e preparare un modello esterno

## 10.1 Flusso standard obbligatorio

**Passo 1 — Ricerca.** Consultare una fonte affidabile con criterio: serve un CRT 4:3 senza marchi obbligatori, con mesh leggibile, licenza chiara e file scaricabile.

**Passo 2 — Qualifica legale.** Registrare URL del modello, autore, licenza, data di acquisizione, eventuale attribuzione e permesso di incorporazione/distribuzione.

**Passo 3 — Import in Blender.** Importare glTF/FBX/OBJ nel progetto DCC. Se FBX o OBJ, **non** consumarli direttamente al runtime: convertirli alla pipeline ufficiale.

**Passo 4 — Normalizzazione.** Correggere scala, orientamento, materiali, numero di parti, errori topologici, UV e qualità visiva.

**Passo 5 — Stylization pass.** Uniformare palette e rugosità, eliminare decorazioni troppo moderne o inadatte, eventualmente creare varianti colore.

**Passo 6 — Semantica.** Definire `ScreenSurface`, `Anchor_*`, collision proxy, interazioni, classi LOD.

**Passo 7 — Export GLB.** Esportare con impostazioni versionate in un preset Blender.

**Passo 8 — Validazione.** Eseguire glTF Validator, test importer, check texture/materiali, screenshot comparativa e controllo manuale.

**Passo 9 — Ottimizzazione.** Ridurre geometria e texture tramite pipeline, senza perdere socket/animazioni e senza introdurre estensioni non decodificate.

**Passo 10 — Packaging.** Registrare manifest e hash; copiare solo il runtime artifact nel pack di distribuzione (e ogni licenza necessaria).

## 10.2 Strumenti concreti

- Blender — editor/modellatore: https://www.blender.org/
- Exporter glTF integrato Blender — documentazione: https://docs.blender.org/manual/en/4.2/addons/import_export/scene_gltf2.html
- Khronos glTF Validator — https://github.com/KhronosGroup/glTF-Validator
- meshoptimizer / `gltfpack` — https://github.com/zeux/meshoptimizer
- Khronos KTX2 / BasisU — https://github.com/KhronosGroup/glTF/tree/main/extensions/2.0/Khronos/KHR_texture_basisu

### Nota sulle ottimizzazioni

`gltfpack` può modificare gerarchie, mesh e animazioni: i prefabs con `Anchor_*` e parti interattive vanno verificati dopo ogni conversione. Le modalità di compressione che producono estensioni glTF speciali **non vanno attivate** fino a quando `fastgltf` e il renderer Retroverse non le supportano o non esiste una fase di decompressione offline.

---

# 11. Asset manifest e catalogo di prefab

Ogni modello accettato deve avere un manifest machine-readable.

```json
{
  "schemaVersion": 1,
  "id": "electronics.crt.consumer_29.a",
  "assetType": "electronic_screen",
  "category": "crt",
  "role": "hero",
  "source": {
    "kind": "original",
    "url": null,
    "author": "Retroverse team",
    "acquiredAt": "2026-10-14",
    "license": "proprietary-original",
    "licenseSnapshot": null,
    "attributionRequired": false,
    "redistributionApproved": true
  },
  "runtime": {
    "model": "models/electronics/crt_consumer_29.glb",
    "scaleMeters": [0.71, 0.61, 0.56],
    "lods": ["crt_consumer_29.glb", "crt_consumer_29_lod1.glb"],
    "defaultMaterialVariant": "warm_graphite",
    "sockets": {
      "screen": "ScreenSurface",
      "inspect": "Anchor_ScreenLook",
      "power": "PowerButton"
    },
    "collision": "box",
    "videoSurface": {"aspectRatio": "4:3", "meshNode": "ScreenSurface"}
  },
  "tags": ["crt", "1990s", "tv", "console_room"],
  "fallbackPrefab": "electronics.crt.generic",
  "hash": "sha256:TO_BE_GENERATED_BY_BUILD"
}
```

**Importante:** i campi e le misure del manifest sono un esempio di schema, non misure certificate di uno specifico televisore. Il tool di build calcola l'hash reale; non introdurre stringhe fittizie nel catalogo finale.

## 11.1 Registro legal/attribution

Tenere un registro strutturato, indipendente dai manifest runtime, con:

- ID univoco e titolo asset;
- pagina di origine e autore;
- licenza e versione;
- data di download e snapshot della licenza;
- eventuali condizioni di accredito;
- modifiche apportate;
- approvazione alla distribuzione;
- percorso del file sorgente nel repository privato;
- percorso del runtime artifact;
- eventuale data di scadenza/necessità di verifica.

L'app genera la schermata **Crediti & Licenze** leggendo questo registro, senza doverla redigere a mano.

---

# 12. Produzione delle cartucce e delle custodie

La collezione è la componente più visibile e ripetuta. Serve un pipeline speciale per migliaia di istanze.

## 12.1 Famiglie geometriche

| Famiglia | Mesh base | Texture/variazioni |
|---|---|---|
| Cartuccia console domestica | Chassis plastico, zona etichetta, bordi e contatti semplificati | Etichetta frontale, colore plastica |
| Cartuccia handheld | Chassis compatto | Label piccola, variazione colore |
| CD / DVD | Disco cilindrico sottile, zona stampata | Artwork/texture, metalness controllata |
| Jewel case CD | Custodia trasparente, booklet e dorso | Front cover, spine, backcover se presente |
| DVD / PS2-style case | Plastic case con copertina incassata | Front, spine e backcover |
| UMD / altri formati | Family mesh specifica o fallback | Artwork o title label |
| Arcade PCB / cabinet | Cabinet espositivo | Nome del gioco, video e marquee se disponibile |

Le geometrie devono essere **originali, compatibili con il renderer e riutilizzabili**. Quando un particolare involucro commerciale non è autorizzato/disponibile, basta una famiglia generica con forma riconoscibile.

## 12.2 Adattamento delle cover art

Ordine di priorità per la texture del fronte:

1. supporto `physicalmedia` originale che corrisponda davvero alla superficie da mostrare;
2. cover frontale (`covers`) con crop/aspect-ratio appropriato;
3. screenshot o title screen, **solo se presentato come fallback**, non come finta cover autentica;
4. cover procedurale con nome/titolo/anno/colore della piattaforma;
5. placeholder leggibile per giochi privi di metadati.

**Non estendere** una copertina in modo arbitrario per riempire completamente il dorso: il sistema deve generare il dorsetto dal titolo oppure usare un'immagine dedicata se disponibile.

## 12.3 Varianti e LOD

- Mesh base condivisa per formato, materiale comune, texture variabile.
- Al livello hero, copertina leggibile (es. 1K o 2K nei casi utili); negli scaffali, texture ridotte in base alla proiezione a schermo.
- Lato di copertina non visibile = texture non caricata in alta risoluzione.
- Per scaffali lontani, usare instancing o un **atlas per cluster di giochi**; durante l'ispezione caricare il modello/texture singolo di qualità migliore.
- Immagini mancanti non devono generare errori né bloccare la creazione della stanza.

## 12.4 Oggetto fisico vs catalogo

Separare rigorosamente:

- `GameRecord`: titolo, piattaforma, ROM, metadati, media;
- `PhysicalRepresentation`: famiglia di supporto, label/cover, prefab e materiali;
- `Placement`: scaffale, slot, rotazione e coordinate nel mondo;
- `InteractionState`: selezionato, in mano, in inserimento, ecc.

Un cambio di shader o di modello della custodia **non modifica mai** la relazione fra la ROM e il gioco.

---

# 13. Generazione procedurale *curated*, non casuale

## 13.1 Input del generatore artistico

```text
WorldStyleInput
  worldSeed
  userStylePreset
  librarySystems[]
  gamesBySystem{}
  artworkCoverageBySystem{}
  platformEra{}
  hardwareFamily{}
  screenAvailability{}
  roomSizePreference
  clutterDensity
  preferredLighting
  graphicQuality
  customPlacementOverrides[]
```

## 13.2 Pipeline proposta

1. **Classification** — aggregazione sistemi e definizione room theme.
2. **Topology** — grafo delle stanze e connessioni, con Hub sempre raggiungibile.
3. **Room shell** — geometria di pavimenti, pareti, aperture, soffitti e alcove.
4. **Functional zoning** — *hero wall*, library wall, museum display, passaggi, espansioni.
5. **Prefab placement** — mobili e console, applicando collisioni e leggibilità.
6. **Collection allocation** — posizionamento dei giochi negli slot assegnati.
7. **Screens & lighting** — scelta TV principale, secondari e preset luminosi.
8. **Decorative dressing** — piante, poster, tappeti e props controllati dalla densità.
9. **Validation** — raggiungibilità, collisioni, distanze, vista del TV, capienza.
10. **Persistence** — conservazione di ID, posizioni e modifiche manuali.

## 13.3 Algoritmo di placement

Niente random generico `rand()` chiamato in sequenza globale. Usare seed deterministici locali:

```text
roomSeed       = hash(worldSeed, stableRoomId)
shelfSeed      = hash(roomSeed, shelfType, stableShelfId)
propSeed       = hash(roomSeed, propCategory, logicalSlotId)
placementSeed  = hash(roomSeed, gameStableId, physicalFamily)
```

In questo modo un nuovo gioco aggiunto al Nintendo 64 non cambia l'orientamento di lampade e tappeti già presenti in una stanza PlayStation.

## 13.4 Regole di posizionamento estetico

- Un'area hero per stanza, prioritariamente su parete con meno interruzioni.
- TV principale posizionata su mobile o staffa coerente con epoca.
- Scaffali occupano pareti secondarie in segmenti modulari.
- Vetrine e poster solo in slot non necessari alla capacità.
- Luci e insegne con ritmo alternato, evitando sequenze uniformi.
- Tappeti in aree lounge, mai sotto un'intera parete di scaffali per caso.
- Piccole asimmetrie curate: una poltrona leggermente ruotata, libri/riviste su un tavolino, pianta vicino al lato di una credenza.
- Nessun prop decorativo dentro il volume di un oggetto interattivo, davanti alla TV o a ostruire l'ingresso.
- Elementi già modificati dall'utente hanno un vincolo `pinned` e non vengono riposizionati automaticamente.

## 13.5 Scoring delle proposte

Generare più candidati economici, valutarli e scegliere il migliore:

```text
score =
  0.24 * navigationClearance
+ 0.19 * heroVisibility
+ 0.17 * collectionCapacity
+ 0.15 * visualBalance
+ 0.10 * screenVisibility
+ 0.08 * themeMatch
+ 0.07 * decorationVariation
- penaltiesForCollisions
- penaltiesForBlockedDoors
- penaltiesForRepeatedPatterns
```

I pesi sono **parametri iniziali da testare** con screenshot automatiche. Le collisioni e la raggiungibilità sono vincoli duri, non semplici penalità negoziabili.

## 13.6 Proceduralità stabile con nuovi giochi

Tre livelli distinti di ricostruzione:

- `NoRebuild`: cambiano media o metadati, si aggiornano solo UI e texture.
- `LocalReflow`: nuovi giochi occupano prima slot liberi o uno scaffale aggiunto localmente.
- `RoomExpansion`: scaffali finiti; si crea un'estensione, preferendo connessioni già previste.

Una rigenerazione generale è disponibile solo come azione utente esplicita con preview, backup e possibilità di annullamento.

---

# 14. Theme pack delle stanze

Un tema non è una scena prefab fissa: è un insieme di **regole, pesi, variazioni e asset ammessi**.

## 14.1 Pacchetto `Classic16BitWarm`

**Per:** SNES, Mega Drive e, con variazioni, console 8/16-bit.

- pavimento parquet e tappeto;
- librerie scure, quadri e riviste;
- CRT 4:3 importante;
- palette calda con piccole luci magenta o rosse;
- uso predominante di cartucce;
- seduta morbida e mobile basso;
- decorazione densa ma ordinata.

## 14.2 Pacchetto `Console90sCollector`

**Per:** N64, Saturn e sistemi coevi.

- mood più allegro e cromatico, sempre illuminato da lampade calde;
- shelving misto con vetrinette e memorabilia;
- cartucce/chassis coerenti con famiglia;
- TV classico in posizione molto evidente;
- elementi grafici geometrici anni Novanta, originali.

## 14.3 Pacchetto `OpticalEraLounge`

**Per:** PS1, PS2, Dreamcast, GameCube, Xbox classico.

- pareti scure, blue lighting discreto, divani e display case;
- custodie CD/DVD, dischi in espositori;
- una TV hero e talvolta un monitor secondario;
- più superfici metalliche e vetro rispetto alle sale cartridge;
- atmosfera primi anni Duemila, ma non eccessivamente futuristica.

## 14.4 Pacchetto `ArcadeAfterDark`

- fondo scuro e corridoi luminosi;
- cabinet modulari, attract video, insegne originali;
- palette neon più varia, bloom più visibile;
- audio direzionale e luci per cabinato;
- layout che lascia uno spazio chiaro davanti a ogni cabinet.

## 14.5 Pacchetto `HandheldArchive`

- illuminazione da esposizione;
- teche e stand bassi;
- console handheld come oggetti hero;
- supporti molto piccoli con ispezione rapida ingrandita;
- schermi televisivi di ambientazione come anteprime, non obbligatoriamente simulazioni degli LCD originali.

## 14.6 Pacchetto `RetroLibraryHub`

- architettura più ampia del resto del mondo;
- legno e metallo, monumento centrale luminoso;
- portali leggibili, segnaletica coerente;
- nessuna saturazione eccessiva;
- atmosfera di museo privato, accessibile e accogliente.

---

# 15. Televisori come punto focale scenografico

## 15.1 Tipologie visive

- `CRT_Consumer_14`, `CRT_Consumer_21`, `CRT_Consumer_29`;
- `CRT_ArcadeCurved`;
- `LCD_16_9_2000s`;
- `ProjectionScreen` opzionale per sala relax;
- `HandheldScreen` principalmente decorativo, salvo implementazione video dedicata.

Ogni schermo è composto da **body mesh** + **screen mesh** + **video source** + **screen shader** + **audio emitter** + **interaction socket**.

## 15.2 Effetto schermo

- mappare il frame video su un materiale emissivo;
- preservare aspect ratio originale (pillarboxing/letterboxing corretto);
- curvatura lieve sul CRT (geometria della mesh o shader UV);
- scanlines sottili e maschera RGB opzionali;
- lieve bloom per suggerire luce del tubo catodico;
- vetro e riflessi controllati, mai tali da rendere il contenuto illeggibile;
- fallback con screenshot, title card o raster noise *originale* se il video manca.

## 15.3 Regole visive per la riproduzione

- TV principale: clip del titolo selezionato; senza focus, shuffle stanza.
- TV secondari: playlist indipendenti a bassa priorità.
- Più schermi visibili possono condividere lo stesso video decoder se mostrano lo stesso filmato.
- La visualizzazione del video deve degradare in immagine statica quando la scena non è in vista o il decoder budget è raggiunto.
- Non aggiungere automaticamente 5 schermi per ogni stanza: **1 hero TV e al massimo 1–2 secondari** sono un target iniziale ragionevole.
- Quando un gioco è in modalità ispezione, l'anteprima principale rimane collegata al suo oggetto fisico.

## 15.4 Audio scenografico

- volume diegetico con attenuazione per distanza;
- una sola sorgente TV dominante in una stanza, salvo scelta utente;
- volume attenuato quando si apre una scheda o una finestra di impostazioni;
- la distanza visiva fra TV non deve essere annullata da audio indistinto;
- mute globale sempre accessibile.

---

# 16. Interazione con supporti fisici: visual design

## 16.1 Tre stati

1. **A scaffale:** supporto illuminato normalmente, nessuna selezione forzata.
2. **Focus:** leggero highlight della silhouette o bordo e, se attiva, piccola etichetta contestuale.
3. **Ispezione:** oggetto vicino alla camera, sfondo con profondità controllata, scheda discreta.

## 16.2 Animazione e tempi

Target UX iniziali, modificabili:

- hover/focus feedback: sotto 100 ms percepiti;
- pickup e rotazione verso camera: 200–400 ms;
- uscita da ispezione: 150–250 ms;
- inserimento nella console: 300–650 ms, **sempre skippabile**;
- selezione con controller: focus stabile e privo di jitter.

Questi tempi sono valori di design da validare, non garanzie sul sistema operativo o sull'emulatore.

## 16.3 UI contestuale

- pannello scuro semiopaco elegante, vicino alla custodia senza coprirla;
- testi grandi a distanza TV (controller-first);
- titolo, anno, piattaforma, sviluppatore e pulsanti essenziali;
- informazioni secondarie nascoste o accessibili con un tasto;
- niente overlay permanente con decine di icone;
- segnale chiarissimo della possibilità di **Avvia**, **Vedi media**, **Informazioni**, **Indietro**.

---

# 17. Segnaletica e identità grafica del mondo

## 17.1 Sistema di signage

Tre livelli:

- **Hub signage:** grandi portali con categoria/era, distanza leggibile.
- **Room signage:** pannelli di sala e piccole informazioni storiche.
- **Object labels:** targhette discrete vicino a pezzi speciali e vetrine.

Le insegne devono essere create da testo e forme vettoriali/mesh originali, non da PNG prerenderizzati per ogni console. Fa eccezione il logo del sistema letto a runtime dai temi di ES-DE dell'utente (ADR-0019 §4): l'insegna lo mostra con il nome reale sotto; senza logo resta il testo generico.

## 17.2 Marchi e artwork

- Le parole che descrivono un sistema e i metadati visualizzati dalla collezione possono essere gestiti come **dati catalografici**.
- Non includere nel pacchetto base loghi ufficiali, pubblicità, poster e mascotte protetti senza una verifica dei diritti/uso. I loghi e i modelli presenti sulla macchina dell'utente (temi di ES-DE, `<user data>/content`) possono essere mostrati a runtime, mai copiati nel pacchetto (ADR-0019).
- Preferire, per gli elementi di ambientazione preinstallati, temi originali come `16-BIT CORNER`, `DISC ERA`, `ARCADE HALL`, `HANDHELDS`.
- Consentire all'utente di assegnare immagini personali ai poster, tenendole locali.
- Nessun sistema di scraping 3D o di artwork commerciale non autorizzato.

## 17.3 Tipografia

- Carattere sans leggibile per UI e pannelli informativi, con licenza di embedding/distribuzione verificata.
- Carattere display retrò originale o opportunamente licenziato per le insegne.
- Mai utilizzare un bitmap pixel font per tutte le informazioni: penalizzerebbe la lettura sui televisori moderni.
- Preferire texture atlas/SDF per testo spaziale aggiornabile dinamicamente.

---

# 18. Preset di stile e personalizzazione

Al lancio esiste **un solo preset principale curato**, `RetroMuseumCozy`, cioè quello del concept. I preset alternativi non devono introdurre costi di sviluppo prima che lo stile principale sia convincente.

## 18.1 Parametri esposti all'utente

- Stile principale: `RetroMuseumCozy`.
- Densità oggetti: essenziale / equilibrata / ricca.
- Luminosità ambiente e intensità neon.
- Tema automatico per epoca o tema neutro globale.
- Dimensione/numero di stanze: compatta / normale / ampia.
- Numero di schermi video attivi.
- Effetto CRT: off / leggero / vintage.
- Mostra/nascondi decorazioni secondarie.
- Permetti modifiche manuali su posizioni e decorazioni.

## 18.2 Preset strutturato (esempio TOML)

```toml
[style]
id = "RetroMuseumCozy"
world_seed = 725183
geometry_units = "meters"
wall_palette = "warm_wood_graphite"
material_profile = "stylized_pbr"
tone_mapping = "filmic_balanced"

[rooms]
layout = "hub_and_themed_rooms"
size = "normal"
clutter_density = 0.52
reserved_expansion_capacity = 0.15
minimum_clear_path_m = 1.40
max_secondary_screens = 2

[lighting]
preset = "warm_cinematic"
accent_light_strength = 0.55
bloom_strength = 0.22
screen_fake_light = true

[visual]
hero_texture_max = 2048
ambient_texture_max = 1024
crt_effect = "subtle"
postprocess_vignette = 0.05
```

**Nota:** parametri, scale e nomi sono indicativi. Il renderer convertirà i valori in unità coerenti, con clamp e default versionati. Il seed di esempio non ha significato particolare.

---

# 19. Pipeline asset di progetto

## 19.1 Organizzazione repository

```text
assets/
  source/                      # opzionale, solo asset originali e redistribuibili
    blender/
      crt_generic.blend
      shelf_modular.blend
      cartridge_families.blend
      hub_landmark.blend
  staging/                     # NON distribuire; modelli scaricati in valutazione
  registry/
    assets.json
    provenance.json
    licenses/
  manifests/
    prefabs/
    themes/
    room_templates/
  materials/
    base/
    tileables/
    atlases/
  models/
    architecture/
    furniture/
    electronics/
    hardware/
    media/
    arcade/
    decor/
  textures/
    decals/
    signage/
    fallback/
  shaders/
    pbr/
    crt/
    postprocess/
  video_fallbacks/
  style_tests/
    reference_scenes/
    screenshots/

tools/
  asset_importer/
  asset_validator/
  asset_preview/
  atlas_builder/
  procedural_prefab_generator/
  credits_generator/

build/assets/                  # pacchetto finale derivato, approvato
```

Non includere in repository pubblico i file `.blend` o i file originali comprati/scaricati se la licenza non ne permette tale distribuzione. La struttura `staging/` deve essere esclusa dal packaging e dal versionamento quando necessario.

## 19.2 Cache applicazione e asset dell'utente

Separare tre domini:

1. **Built-in assets:** distribuiti con Retroverse, auditati e versionati.
2. **User imported assets:** scelti/importati dall'utente, locali al suo profilo e sotto sua responsabilità.
3. **Collection media:** immagini/video ES-DE lette in-place, con cache derivata non distruttiva.

La cache GPU/immagini e i layout restano nelle directory proprie di Retroverse (`%LOCALAPPDATA%` o percorso portable), **non** nella cartella ES-DE, salvo esportazione richiesta dall'utente.

## 19.3 Tool di preview

Creare un'app `retroverse-asset-preview` che mostri:

- modello ruotabile su sfondo standard;
- bounding box, pivot, socket e collision proxy;
- materiali e texture;
- numero di triangoli/mesh/draw-call potenziali;
- LOD e distanza switch;
- shader di fallback e normal visualization;
- check licenza/sorgente del manifest;
- anteprima di un media dinamico applicato al prefab.

**Il preview tool è una priorità di sviluppo reale:** senza di esso, aggiungere asset al progetto C++ puro diventerebbe lento e fragile.

---

# 20. Rendering e prestazioni: budget di art direction

Questi sono **target di progettazione da profilare**, non limiti tecnici universali.

## 20.1 Frame budget

- Target principale: **60 FPS a 1920×1080** su un PC Windows con GPU dedicata di fascia media, previa definizione di una macchina di riferimento.
- Profilo Low: 30 FPS stabili dove 60 non siano realistici.
- Tempo per frame target 60 FPS: circa **16,7 ms** complessivi; budget CPU/GPU ripartito dopo profiling.
- Ridurre la densità visiva soltanto dopo culling, instancing, LOD e gestione dei media.

## 20.2 Budget iniziali per stanza attiva

| Risorsa | Target indicativo Balanced | Strategia |
|---|---|---|
| Mesh statiche comuni | Centinaia istanze, poche mesh uniche | Instancing + frustum/room culling |
| Custodie/cartucce | Anche migliaia in catalogo; solo subset rilevante residente in qualità piena | LOD, batching, texture streaming |
| Luci shadow-casting | 1–3 rilevanti nella vista | Shadow maps selettive |
| Video in decodifica attiva | 1–2, espandibili in High dopo misure | Scheduler visibilità/priorità |
| Texture ambientali | Preferenza 1K, hero 2K ove utile | Atlas/KTX2 opzionale e mipmaps |
| Materiali | Libreria condivisa | Material variants, state sorting |
| Props decorativi | Budget per stanza in base a preset | Culling, istanze, scelta casuale vincolata |

Il sistema non deve decodificare automaticamente tutti i filmati della stanza contemporaneamente.

## 20.3 Tecniche consigliate

- Frustum culling e **room/portal culling** per grandi ambienti con stanze separate.
- GPU instancing per scaffali, supporti e props ricorrenti.
- LOD basati su dimensione proiettata; qualità hero solo quando serve.
- Pool e cache per texture di copertine; decode asincrona.
- Riduzione degli shader/material variants inutili.
- Lightmap/probe dove appropriato, non forza bruta in illuminazione dinamica.
- Fallback su immagini statiche per video fuori vista.
- Texture KTX2/Basis Universal **solo se** il runtime supporta decodifica/transcodifica relativa; PNG/JPG come baseline.
- `gltfpack` e meshoptimizer come strumenti di build con test regressione visiva.

## 20.4 Degradazione elegante

Su hardware debole:

1. disattivare effetti CRT avanzati e ombre secondarie;
2. ridurre risoluzione/render scale;
3. abbassare numero di video attivi;
4. ridurre props minori;
5. diminuire LOD/texture;
6. **mantenere sempre** console, TV principale, scaffali e cartucce visibili.

---

# 21. Casistiche di assenza asset: fallback end-to-end

| Mancanza/problema | Comportamento obbligatorio |
|---|---|
| Modello originale di una console assente | `GenericConsole` della stessa famiglia con targa dinamica |
| Modello generico assente | Generazione C++ con primitive sagomate, pulsanti e slot |
| Nessuna cover | Etichetta procedurale con nome e piattaforma |
| `physicalmedia` non corrisponde alla mesh | Usare cover frontale adattata; mai deformare o fingere un layout reale |
| ROM sconosciuta | Custodia generica, titolo dal filename ripulito, icona informativa |
| Video mancante | Screenshot → title screen → cover → idle screen |
| Poster decorativo mancante | Artwork astratto originale e palette della stanza |
| CRT dedicato assente | CRT generico con screen mesh 4:3 |
| Shader custom non compila | Materiale PBR standard o unlit leggibile |
| GLB malformato | Asset family/procedural, warning diagnostico e registrazione errore |
| Licenza non approvata per il packaging | Escludere l'asset dal pacchetto e selezionare fallback |
| Texture troppo grande o memoria esaurita | Streaming/downscale e texture neutra temporanea |
| Nessuna ROM per un sistema | Non creare stanza inutilizzata, salvo stanza utente fissata manualmente |

**Requisito P0:** una libreria composta da una sola ROM senza alcun media deve generare comunque un ambiente gradevole, navigabile, con una postazione utilizzabile.

---

# 22. Catalogo tecnico delle console e dei media: scalabilità

La fedeltà hardware va introdotta progressivamente e con un database dei formati di supporto. Il mapping sistema → prefab non deve essere codificato in dozzine di `if` nel renderer.

```text
MediaFamilyRegistry
  cartridge_home_8bit
  cartridge_home_16bit
  cartridge_home_64bit
  cartridge_handheld_small
  cartridge_handheld_medium
  jewel_case_cd
  dvd_case
  mini_disc_case
  optical_disc
  umd_case
  arcade_cabinet
  generic_media_box
```

Ogni sistema mappa a una famiglia con override per shape, colore, volume, posizione etichetta e animazione. L'identità del sistema viene dal catalogo ES-DE, non dal nome della mesh.

La prima versione coprirà i principali formati presenti nella libreria di test. Formati rari ricadono su `generic_media_box` o su un family prefab vicino, senza interrompere l'importazione.

---

# 23. Asset authoring: quali modelli conviene creare davvero

## 23.1 Modelli a alto ritorno

**A. CRT generico:** si vede in quasi ogni stanza e contiene video; vale la pena modellarlo con buona qualità. Un solo modello può offrire varianti 14/21/29 pollici e plastiche nere/grigie.

**B. Scaffale modulare:** il modello più riutilizzato. Quattro segmenti (terminale, centrale, angolo, basso) e una manciata di altezze bastano per generare centinaia di librerie.

**C. Custodie e cartucce:** dieci famiglie ben realizzate rappresentano migliaia di ROM.

**D. Mobile TV e sedute:** due o tre varianti coerenti fanno la differenza fra magazzino e stanza abitata.

**E. Insegne, poster frame e teche:** facili da parametrizzare e potentissimi visivamente.

**F. Portale della stanza:** dà personalità al mondo con pochi asset.

**G. Cabinet arcade generico:** serve per riempire l'intera sezione arcade senza modelli per gioco.

## 23.2 Modelli da comprare/recuperare per ultimi

- singola console rarissima e accurata;
- controller speciali poco usati;
- lampade e piante molto specifiche;
- oggetti piccoli in sfondo;
- poster commerciali e memorabilia difficili da licenziare.

Un ambiente con scaffali, TV e luce eccellenti ma console generiche **apparirà migliore** di uno con console fedelissime in stanze vuote o prive di identità.

---

# 24. Esempio completo: generare una stanza PlayStation

**Input fittizio di test:** 180 giochi PlayStation, 140 copertine, 80 video, 10 preferiti.

1. `WorldPlanner` individua un sistema con supporto ottico e collezione media.
2. Seleziona il theme pack `OpticalEraLounge`.
3. Riserva una shell di circa 10 × 11 m; layout diverso dal seed SNES.
4. Fissa un'area hero con TV 4:3, mobile basso e una console `Signature` oppure `Family`.
5. Calcola la capienza necessaria delle custodie jewel case (numero di slot e margine di crescita).
6. Posiziona scaffali su due/tre pareti; riserva una sezione ai preferiti.
7. Inserisce una vetrina e un tappeto soltanto se non restringono i passaggi.
8. Popola le custodie con artwork esistente, usando etichette procedurali per i 40 giochi senza cover.
9. Genera poster astratti originali e pannelli informativi; non redistribuisce automaticamente cover non autorizzate.
10. TV principale: `RoomShuffle` con i video disponibili; fallback screenshot per gli altri.
11. Applica illuminazione calda di fondo + un accento blu.
12. Testa raggiungibilità, visibility score della TV e allineamento dei ripiani.
13. Registra gli ID stabili di stanze, scaffali e collocazioni.
14. All'avvio successivo rileva 5 giochi nuovi e li colloca negli slot di espansione, senza toccare le vecchie custodie.

**Risultato minimo accettabile:** nessuno scaffale compenetrato, console visibile, video riproducibile o fallback corretto, stanza leggibile e navigabile da controller.

---

# 25. Progettazione degli shader essenziali

| Shader | Funzione | Priorità |
|---|---|---|
| `PBR_Base` | Materiali metallic-roughness, luci, normal map | P0 |
| `Unlit_Emissive` | Pannelli, insegne, fallback illuminati | P0 |
| `VideoScreen` | Texture video su superficie 3D, aspect ratio | P0 |
| `CRT_Subtle` | Scanline, curvatura, bloom controllati | P0 |
| `HoverOutline` | Focus oggetti senza sfarfallio | P0 |
| `GlassSimple` | Vetrina sobria, alpha corretto | P1 |
| `DecalSurface` | Manifesti/etichette/usura | P1 |
| `CarpetFabric` | Variazioni leggeri di fibre/normal map | P2 |
| `NeonGlow` | Tube emissive e bloom mirato | P1 |
| `DustMotes` | Particelle ambientali quasi impercettibili | P2 |

**Nota di rendering:** usare una scheda di ispezione 2D UI non equivale a caricare ogni informazione come decal 3D. La UI deve rimanere un layer separato, con colore, scala e leggibilità indipendenti dalla luce ambientale.

---

# 26. QA visiva e test automatici

## 26.1 Golden screenshots

Stabilire una camera e un seed fissi per almeno queste scene:

1. Hub con 4 portali.
2. Stanza SNES con 50 giochi.
3. Stanza PS1 con 200 giochi.
4. Arcade con 20 cabinet.
5. Stanza handheld con 10 giochi.
6. Stanza con 1 sola ROM e nessun media.
7. Oggetto ispezionato da vicino.
8. TV con video attivo e TV in fallback.
9. Profilo Low nella stessa scena Balanced.
10. Aggiornamento con aggiunta di 10 giochi.

Automatizzare screenshot comparabili usando stessi seed, camera, risoluzione, versione asset e preset luce; fare diff con tolleranze ragionevoli. Per il video, congelare un frame test a timestamp fisso.

## 26.2 Checklist artistica

- [ ] L'immagine è riconoscibile come spazio retrogaming senza il titolo Retroverse?
- [ ] C'è almeno un elemento scenografico forte?
- [ ] Le copertine leggibili sono reali, non deformate?
- [ ] Si capisce dove entrare e dove trovare i giochi?
- [ ] Il legno e la luce calda danno coerenza alle aree?
- [ ] Il neon non brucia i dettagli dei modelli?
- [ ] Il CRT sembra un vero oggetto fisico, non un floating video plane?
- [ ] L'ambientazione resta credibile senza asset commerciali?
- [ ] Il numero di props non compromette la ricerca di una ROM?
- [ ] La vista Low mantiene le stesse priorità estetiche?

## 26.3 Test asset automatici

- Validazione glTF completa per ogni GLB pubblicato.
- Verifica mesh vuote, NaN nelle trasformazioni, normali errate, texture mancanti.
- Verifica `ScreenSurface` obbligatoria per un prefab `electronic_screen`.
- Verifica pivot e bounding box entro limiti per categoria.
- Verifica SHA-256 e provenienza.
- Verifica righe di attribution/licenza per asset di terzi.
- Verifica collider proxy per arredi solidi.
- Verifica family fallback esistente per ogni categoria.
- Test di caricamento con perdita intenzionale di un asset.

---

# 27. Roadmap visuale raccomandata

## Fase A — Look development e tooling

**Obiettivo:** validare la direzione estetica con strumenti minimi, non costruire già il mondo intero.

- Caricatore GLB, camera, shader PBR di base.
- Preview tool, asset manifest, un materiale legno, uno metallo e uno plastica.
- Modular wall/floor + scaffold shelf.
- CRT con screen mesh, illuminazione calda e un'altra luce d'accento.
- Una cover di test applicata a una cartuccia/custodia.
- Golden screenshot della prima stanza.

**Gate A:** stanza vuota ma credibile, con stile e illuminazione già riconoscibili.

## Fase B — Vertical slice SNES

- Stanza generata dai dati reali di una libreria ES-DE.
- 25–100 cartucce riempite proceduralmente.
- TV che riproduce un video locale.
- Pickup/ispezione di una cartuccia.
- Fallback media e oggetti.
- Profilo grafico Balanced e benchmark.

**Gate B:** sembra già il concept, anche se contiene soltanto una stanza.

## Fase C — Multiroom hub

- Hub e connessioni, stanza PS1 e SNES con palette differenziate.
- Theme packs, prop placement e room growth.
- Insegne dinamiche e segnaletica coerente.
- Persistenza modifiche manuali.

**Gate C:** stanze diverse ma inequivocabilmente appartenenti allo stesso mondo.

## Fase D — Arcade e handhelds

- Cabinet generici con video texture.
- Vetrine per portatili, oggetti piccoli e impostazioni di ispezione.
- Miglioramento lighting budget per ambienti più ricchi.

**Gate D:** le variazioni tematiche sono abbastanza forti da giustificare stanze separate.

## Fase E — Art pass e distribuzione

- Normalizzazione del catalogo esterno, audit licenze, crediti.
- LOD, atlas, culling e ottimizzazione degli shader.
- Passeggiata completa sul PC target.
- Collezione numerosa, modalità Low, asset mancanti e recovery.
- Screenshots ufficiali della v1.

**Gate E:** il prodotto è distribuibile, performante e conserva lo stile anche con librerie eterogenee.

---

# 28. Definition of Done del mondo 3D

Il sottosistema visuale si considera pronto per una prima release pubblica quando:

1. È possibile importare una libreria ES-DE e ottenere un hub + le stanze necessarie senza modellazione manuale.
2. Le stanze appartengono a una direzione artistica coerente con il concept: legno scuro, ambienti caldi, display, insegne, luci accentate.
3. Ogni sistema attivo è rappresentato da un supporto fisico identificabile e una postazione o espositore.
4. Ogni oggetto ha un fallback locale e la scomparsa di un modello esterno non impedisce l'avvio.
5. I supporti si aggiornano con cover e metadati già presenti in ES-DE.
6. Almeno un televisore per stanza idonea riproduce il video corretto o mostra un fallback.
7. Le aggiunte di ROM sono incrementali e non distruggono il posizionamento precedente.
8. Le stanze sono percorse facilmente con gamepad e con mouse/tastiera.
9. Uno schema di asset e licenze permette di tracciare ogni modello incluso nel pacchetto.
10. Le golden screenshots passano revisione stilistica e non mostrano intersezioni o asset malformati.
11. Nessuna componente essenziale richiede un download obbligatorio da Internet.
12. Esistono un asset preview tool, un validator e un test di distribuzione con soli assets built-in.

---

# 29. Pianificazione concreta per uno sviluppatore singolo

L'obiettivo non è accumulare centinaia di modelli prima del rendering. Il lavoro più utile è il seguente:

**Prima acquisizione/modellazione:**

- 1 prefab scaffale modulare di qualità;
- 1 CRT hero;
- 1 mobile TV;
- 3 famiglie di supporti per i sistemi principali;
- 1 console generica con cartuccia;
- 1 console generica con disco;
- 2 sedute;
- 1 tappeto;
- 2 illuminazioni;
- 1 portale;
- 2 poster frame;
- 1 teca;
- 1 kit architettonico modulare.

**Primo risultato visivo atteso:** uno spazio di circa 9 × 10 metri con 50 giochi, una console, un televisore attivo, luce calda e scaffali curati. Se questa scena è convincente e fluida, il resto può crescere come combinazione di prefab e regole.

**Non introdurre inizialmente:** cinquecento modelli accurati di console, editor di stanze completo, multiformato di asset al runtime, illuminazione ray-traced, tante varianti cromatiche e generazione automatica di vere texture ufficiali non autorizzate.

---

# 30. Decisioni aperte da validare con prototipi

| Decisione | Default proposto | Come validarla |
|---|---|---|
| Livello di realismo | Stylized realism premium | Screenshot comparate della prima stanza |
| Dimensioni base | 9 × 10 m | Movimento col controller e leggibilità scaffali |
| Numero TV | 1 hero + fino a 2 secondari | Prestazioni FFmpeg/renderer e rumore visivo |
| Famiglie cartucce | 3–5 per primo prototipo | Mappatura della libreria ES-DE reale |
| Texture hero | Fino a 2K dove utile | VRAM e leggibilità in ispezione |
| Ombre | Poche dinamiche + AO/IBL | Benchmark GPU di riferimento |
| Telecamera | Prima persona 70–85° orizzontali | Sensibilità, motion sickness e inquadratura |
| Quota art esterna | Decorazione più che strutture | Audit asset e omogeneità visiva |
| Layout estensibile | Slot liberi + room expansion | Test import incrementale |
| Modelli console realistici | Opzionali Tier A | Costi, disponibilità e licenze |

---

# 31. Risorse ufficiali e verifica delle licenze

Elenco di partenza verificato a ottobre 2026. Licenze, condizioni e disponibilità possono cambiare: ogni asset acquisito deve conservare un proprio snapshot o riferimento alla licenza applicabile.

1. **Poly Haven — asset CC0**: https://polyhaven.com/license
2. **Kenney — FAQ sulle licenze CC0**: https://kenney.nl/support
3. **Quaternius — QAL v1.0 (28/08/2026)**: https://quaternius.com/license.html
4. **Fab — Standard License**: https://www.fab.com/eula
5. **Sketchfab — License Agreement / Editorial restrictions**: https://sketchfab.com/licenses
6. **Sketchfab — Download API, vincoli e autenticazione**: https://sketchfab.com/developers/download-api/downloading-models
7. **Creative Commons BY 4.0 — attribuzione e modifiche**: https://creativecommons.org/licenses/by/4.0/
8. **Khronos — glTF standard**: https://www.khronos.org/gltf/
9. **Khronos — glTF Validator**: https://github.com/KhronosGroup/glTF-Validator
10. **Blender — glTF export**: https://docs.blender.org/manual/en/4.2/addons/import_export/scene_gltf2.html
11. **meshoptimizer e gltfpack**: https://github.com/zeux/meshoptimizer
12. **Khronos — KTX2 / Basis Universal extension**: https://github.com/KhronosGroup/glTF/blob/main/extensions/2.0/Khronos/KHR_texture_basisu/README.md
13. **ambientCG — libreria PBR**: https://ambientcg.com/

Questa sezione è informativa, non consulenza legale. Le licenze di singoli modelli, marchi, confezioni e artwork originali vanno valutate separatamente dai termini del marketplace che li ospita.

---

# 32. Conclusione progettuale

Per ottenere concretamente il mondo mostrato nell'immagine, la priorità è **costruire bene un piccolo insieme di sistemi artistici replicabili**:

- un kit architettonico modulare;
- una libreria di materiali caldi e coerenti;
- una pipeline di acquisizione/normalizzazione/licenza degli asset;
- pochi prefab distintivi (CRT, console, cartucce, scaffali, vetrine);
- un generatore di stanze che lavori per composizione e regole;
- un renderer che valorizzi luci, atmosfera, video e ombre senza dipendere da un engine;
- fallback visivamente gradevoli per ogni categoria di oggetto.

**Principio guida:** *un mondo procedurale deve sembrare progettato da un artista, non riempito da un algoritmo*. Si raggiunge con template, vincoli, peso visivo, gerarchie di dettaglio e piccole variazioni controllate, non aggiungendo casualità a ogni elemento.
