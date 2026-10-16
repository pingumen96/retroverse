#include "mock_catalog.hpp"

namespace rv::spike_ui {
namespace {

ui::CatalogEntry game(const char* id, const char* title, const char* platform, int year, const char* developer,
                      const char* publisher, const char* genre, const char* players, const char* description) {
    return ui::CatalogEntry{id, title, platform, year, developer, publisher, genre, players, description};
}

} // namespace

std::vector<ui::CatalogEntry> mockCatalog() {
    return {
        game("g01", "Lanterna Cosmica", "16 bit", 1992, "Studio Aurora", "Aurora Giochi", "Sparatutto", "1-2",
             "Una nave di vetro attraversa nebulose dipinte a mano. In ogni settore c'è una lanterna da "
             "riaccendere prima che il buio inghiotta la rotta. Il secondo giocatore pilota un drone di scorta che "
             "raccoglie i frammenti di luce."),
        game("g02", "Corsa sul Lungomare", "8 bit", 1988, "Officina Pixel", "Officina Pixel", "Corse", "1-2",
             "Biciclette, gelati e curve a strapiombo sul mare: una corsa estiva lungo la costa, con scorciatoie tra "
             "i vicoli e un faro che segna il traguardo. In due si gioca a schermo diviso."),
        game("g03", "Il Faro di Vetro", "16 bit", 1994, "Officina Lunare", "Lunare Edizioni", "Avventura", "1",
             "Un guardiano senza memoria si sveglia in un faro che riflette altri mondi. Specchi da orientare, "
             "enigmi di luce e un diario da ricomporre pagina dopo pagina, una stanza alla volta."),
        game("g04", "Pixel Garden", "Arcade", 1991, "Serra Digitale", "Serra Digitale", "Rompicapo", "1-2",
             "Semi da piantare in una griglia che cresce a ogni mossa: fiori dello stesso colore sbocciano insieme e "
             "liberano spazio. Partite brevi, colori caldi e una modalità a due giocatori in contemporanea."),
        game("g05", "Cavalieri del Vapore", "16 bit", 1993, "Ferro & Fumo", "Aurora Giochi", "Picchiaduro", "1-2",
             "Armature a vapore si sfidano su piattaforme di una città industriale sospesa. Ogni cavaliere "
             "regola la pressione delle sue caldaie: troppa e salta tutto, troppo poca e il colpo non parte."),
        game("g06", "Notte al Museo Meccanico", "32 bit", 1997, "Studio Ingranaggio", "Ingranaggio", "Avventura",
             "1",
             "Gli automi del museo prendono vita dopo la chiusura. Un custode notturno deve riportarli nelle teche "
             "prima dell'alba, riparando molle e carillon con gli attrezzi trovati nelle sale."),
        game("g07", "Rally delle Dolomiti", "32 bit", 1998, "Curva Stretta", "Curva Stretta", "Corse", "1-4",
             "Tornanti innevati, rifugi e gallerie scavate nella roccia: un rally a tappe tra le montagne, con il "
             "meteo che cambia durante la gara e un copilota che detta le note a voce."),
        game("g08", "Piccolo Astronomo", "Portatile", 1995, "Lente Blu", "Lente Blu", "Rompicapo", "1",
             "Costellazioni da ricostruire collegando stelle sparse in un cielo che ruota con le stagioni. Ogni "
             "notte svela un mito nuovo e un telescopio più potente da costruire."),
        game("g09", "Mare di Bit", "8 bit", 1987, "Officina Pixel", "Officina Pixel", "Piattaforme", "1",
             "Un pesce volante attraversa un oceano fatto di blocchi luminosi, saltando tra le onde e schivando le "
             "meduse. Le correnti cambiano direzione a ogni livello."),
        game("g10", "Tempesta sul Ring", "Arcade", 1990, "Pugno Dorato", "Pugno Dorato", "Sport", "1-2",
             "Pugilato da sala giochi con un pubblico che reagisce a ogni colpo. Il tempismo conta più della "
             "forza: le parate perfette caricano un montante che chiude l'incontro."),
        game("g11", "La Torre degli Orologi", "16 bit", 1995, "Studio Aurora", "Aurora Giochi", "Piattaforme", "1",
             "Una torre in cui ogni piano scorre a una velocità diversa del tempo. Fermare gli ingranaggi "
             "apre passaggi, accelerarli fa crescere edere e ponti. In cima, l'orologio che governa la città."),
        game("g12", "Volo Notturno", "32 bit", 1996, "Ala Blu", "Ala Blu", "Simulazione", "1",
             "Un idrovolante postale consegna lettere tra isole illuminate solo dai fari. Strumenti da leggere, "
             "venti da interpretare e atterraggi sull'acqua al chiaro di luna."),
        game("g13", "Giardino Zen", "Portatile", 1999, "Serra Digitale", "Serra Digitale", "Rompicapo", "1",
             "Sassi da spostare e sabbia da rastrellare finché i disegni non tornano in equilibrio. Nessun "
             "tempo limite: solo cento giardini da comporre con calma."),
        game("g14", "Bottega dell'Alchimista", "16 bit", 1993, "Officina Lunare", "Lunare Edizioni",
             "Gioco di ruolo", "1",
             "Un apprendista eredita una bottega piena di ampolle e debiti. Ingredienti da raccogliere nei boschi, "
             "ricette da scoprire e clienti esigenti da accontentare prima della festa del paese."),
        game("g15", "Lupi di Ghiaccio", "8 bit", 1989, "Nord Software", "Nord Software", "Azione", "1-2",
             "Due esploratori attraversano un ghiacciaio inseguiti da un branco affamato. Crepacci, slitte e "
             "aurore boreali in una fuga a scorrimento verticale."),
        game("g16", "Città di Neon", "Arcade", 1992, "Pugno Dorato", "Pugno Dorato", "Picchiaduro", "1-2",
             "Strade bagnate dalla pioggia e insegne al neon: due amici ripuliscono il quartiere a colpi di mosse "
             "speciali, tra salagiochi, mercati notturni e tetti spazzati dal vento."),
        game("g17", "Il Postino delle Stelle", "Portatile", 1998, "Lente Blu", "Lente Blu", "Avventura", "1",
             "Un postino in bicicletta consegna messaggi tra pianeti grandi quanto un quartiere. Ogni lettera "
             "racconta una storia e apre una strada nuova nel sistema solare."),
        game("g18", "Regata del Vento", "32 bit", 1997, "Curva Stretta", "Curva Stretta", "Sport", "1-2",
             "Vele da regolare, boe da girare e raffiche da leggere sull'acqua: una regata tra isole, con barche "
             "d'epoca e una modalità a due in cui si sfida un amico sulla stessa rotta."),
        game("g19", "Biblioteca Sommersa", "16 bit", 1996, "Studio Ingranaggio", "Ingranaggio", "Avventura", "1",
             "Una biblioteca affondata con la sua città. Un palombaro esplora scaffali coperti di coralli, "
             "recupera volumi e ricostruisce la storia di chi li scrisse."),
        game("g20", "Galassia a Mosaico", "Arcade", 1994, "Serra Digitale", "Serra Digitale", "Sparatutto", "1-2",
             "Astronavi di tessere colorate: ogni nemico abbattuto lascia un frammento che completa il mosaico "
             "dello sfondo. Il quadro finito sblocca il livello segreto."),
    };
}

} // namespace rv::spike_ui
