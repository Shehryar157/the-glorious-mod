#include <windows.h>
#include <cwctype>
#include <regex>
#include <string>
#include "engine.h"
#include "lang.h"

namespace lang {

namespace {

// English, French, German, Spanish (the game's languages: Core/LANG.TXT). Written by Claude
// (2026-11-15), not by native speakers: worth a check by players of each language.
struct Phrase {
    const wchar_t *en, *fr, *de, *es;
};

const Phrase phrases[] = {
    // Counts ("of" itself comes from the game: IDS_TURNS_OF; this is the fallback outside it)
    {L"of", L"sur", L"von", L"de"},
    // Controls and their states
    {L"selected", L"sélectionné", L"ausgewählt", L"seleccionado"},
    {L"unavailable", L"indisponible", L"nicht verfügbar", L"no disponible"},
    {L"not available", L"pas disponible", L"nicht verfügbar", L"no disponible"},
    {L"checked", L"coché", L"aktiviert", L"marcado"},
    {L"not checked", L"non coché", L"nicht aktiviert", L"no marcado"},
    {L"user content", L"contenu utilisateur", L"Benutzerinhalt", L"contenido de usuario"},
    {L"locked", L"verrouillé", L"gesperrt", L"bloqueado"},
    {L"table, {0} rows, {1} columns", L"tableau, {0} lignes, {1} colonnes", L"Tabelle, {0} Zeilen, {1} Spalten",
     L"tabla, {0} filas, {1} columnas"},
    {L"drop-down", L"liste déroulante", L"Aufklappliste", L"lista desplegable"},
    {L"list", L"liste", L"Liste", L"lista"},
    {L"edit", L"zone de saisie", L"Eingabefeld", L"campo de texto"},
    {L"editing", L"saisie", L"Eingabe", L"escribiendo"},
    {L"blank", L"vide", L"leer", L"vacío"},
    {L"{0} items", L"{0} éléments", L"{0} Einträge", L"{0} elementos"},
    {L"edge", L"bord", L"Rand", L"borde"},
    {L"Loading", L"Chargement", L"Wird geladen", L"Cargando"},
    {L"combo box", L"liste déroulante", L"Kombinationsfeld", L"cuadro combinado"},
    {L"canceled", L"annulé", L"abgebrochen", L"cancelado"},
    {L"closed", L"fermé", L"geschlossen", L"cerrado"},
    {L"empty", L"vide", L"leer", L"vacío"},
    {L"none", L"aucun", L"keine", L"ninguno"},
    // Message categories
    {L"All", L"Tout", L"Alle", L"Todo"},
    {L"Combat", L"Combat", L"Kampf", L"Combate"},
    {L"Morale", L"Moral", L"Moral", L"Moral"},
    {L"Movement", L"Mouvement", L"Bewegung", L"Movimiento"},
    {L"Turns", L"Tours", L"Züge", L"Turnos"},
    {L"Deployment", L"Déploiement", L"Aufstellung", L"Despliegue"},
    // Units
    {L"facing {0}", L"orientation {0}", L"Ausrichtung {0}", L"orientado al {0}"},
    {L"south", L"sud", L"Süden", L"sur"},
    {L"southeast", L"sud-est", L"Südosten", L"sureste"},
    {L"east", L"est", L"Osten", L"este"},
    {L"northeast", L"nord-est", L"Nordosten", L"noreste"},
    {L"north", L"nord", L"Norden", L"norte"},
    {L"northwest", L"nord-ouest", L"Nordwesten", L"noroeste"},
    {L"west", L"ouest", L"Westen", L"oeste"},
    {L"southwest", L"sud-ouest", L"Südwesten", L"suroeste"},
    {L"{0} north", L"{0} nord", L"{0} nach Norden", L"{0} al norte"},
    {L"{0} south", L"{0} sud", L"{0} nach Süden", L"{0} al sur"},
    {L"{0} east", L"{0} est", L"{0} nach Osten", L"{0} al este"},
    {L"{0} west", L"{0} ouest", L"{0} nach Westen", L"{0} al oeste"},
    {L"here", L"ici", L"hier", L"aquí"},
    {L"has general", L"avec général", L"mit Feldherr", L"con general"},
    {L"out of command range", L"hors de portée de commandement", L"außerhalb der Befehlsreichweite",
     L"fuera del alcance de mando"},
    {L"out of sight", L"hors de vue", L"außer Sicht", L"fuera de la vista"},
    {L"lost {0} men, {1} left", L"{0} hommes perdus, {1} restants", L"{0} Mann verloren, {1} übrig",
     L"{0} hombres perdidos, quedan {1}"},
    {L"spotted", L"repéré", L"gesichtet", L"avistado"},
    {L"moved to", L"déplacé en", L"bewegt nach", L"movido a"},
    {L"Generals", L"Généraux", L"Feldherren", L"Generales"},
    {L"You", L"Vous", L"Sie", L"Tú"},
    // Mouse pictures in the game's text stand for the mod's key
    {L"Enter", L"Entrée", L"Enter", L"Intro"},
    {L"pressing Enter", L"en appuyant sur Entrée", L"mit Enter", L"pulsando Intro"},
    // F1 in battle: the mod's keys
    {L"Q, W, E, A, D, Z, X, C: move the cursor",
     L"Q, W, E, A, D, Z, X, C : déplacer le curseur",
     L"Q, W, E, A, D, Z, X, C: Cursor bewegen",
     L"Q, W, E, A, D, Z, X, C: mover el cursor"},
    {L"Ctrl plus a direction: move the cursor with the game's detailed tooltips",
     L"Ctrl plus une direction : déplacer le curseur avec les infobulles détaillées du jeu",
     L"Strg plus Richtung: Cursor bewegen, mit den ausführlichen Tooltips des Spiels",
     L"Ctrl más una dirección: mover el cursor con las descripciones detalladas del juego"},
    {L"Alt plus a direction: order the selected unit to the square next to it that way; it stays selected",
     L"Alt plus une direction : envoyer l'unité sélectionnée sur la case voisine dans cette direction ; elle reste sélectionnée",
     L"Alt plus Richtung: die ausgewählte Einheit auf das Nachbarfeld in dieser Richtung schicken; sie bleibt ausgewählt",
     L"Alt más una dirección: ordenar a la unidad seleccionada ir a la casilla vecina en esa dirección; sigue seleccionada"},
    {L"Enter: select the unit, or give the order on the cursor square",
     L"Entrée : sélectionner l'unité, ou donner l'ordre sur la case du curseur",
     L"Enter: Einheit auswählen, oder den Befehl auf dem Cursorfeld geben",
     L"Intro: seleccionar la unidad, o dar la orden en la casilla del cursor"},
    {L"Alt+Enter: undo the move",
     L"Alt+Entrée : annuler le mouvement",
     L"Alt+Enter: Bewegung rückgängig machen",
     L"Alt+Intro: deshacer el movimiento"},
    {L"Ctrl+Enter: unit information",
     L"Ctrl+Entrée : informations sur l'unité",
     L"Strg+Enter: Einheiteninformationen",
     L"Ctrl+Intro: información de la unidad"},
    {L"Escape: deselect",
     L"Échap : désélectionner",
     L"Esc: Auswahl aufheben",
     L"Esc: deseleccionar"},
    {L"S: the unit's AP, men, morale and details",
     L"S : PA, hommes, moral et détails de l'unité",
     L"S: AP, Männer, Moral und Details der Einheit",
     L"S: PA, hombres, moral y detalles de la unidad"},
    {L"T: turn and scores",
     L"T : tour et scores",
     L"T: Runde und Punktestand",
     L"T: turno y puntuaciones"},
    {L"O: battle name, date and briefing",
     L"O : nom de la bataille, date et briefing",
     L"O: Name der Schlacht, Datum und Einsatzbesprechung",
     L"O: nombre de la batalla, fecha y sesión informativa"},
    {L"R: retreat from the battle; Enter or R again to confirm, Escape to cancel",
     L"R : se retirer de la bataille ; Entrée ou R à nouveau pour confirmer, Échap pour annuler",
     L"R: aus der Schlacht zurückziehen; Enter oder noch einmal R zum Bestätigen, Esc zum Abbrechen",
     L"R: retirarse de la batalla; Intro o R otra vez para confirmar, Esc para cancelar"},
    {L"N: end turn; Enter or N again to confirm, Escape to cancel. In the battlefield review: end the review",
     L"N : fin du tour ; Entrée ou N à nouveau pour confirmer, Échap pour annuler. Dans la revue du champ de bataille : terminer la revue",
     L"N: Zug beenden; Enter oder noch einmal N zum Bestätigen, Esc zum Abbrechen. In der Schlachtfeldübersicht: Übersicht beenden",
     L"N: terminar turno; Intro o N otra vez para confirmar, Esc para cancelar. En la revisión del campo de batalla: terminar la revisión"},
    {L"Page Up, Page Down: scanner, previous or next unit type, nearest first",
     L"Page préc., Page suiv. : scanner, type d'unité précédent ou suivant, le plus proche d'abord",
     L"Bild auf, Bild ab: Scanner, vorheriger oder nächster Einheitentyp, nächster zuerst",
     L"Re Pág, Av Pág: escáner, tipo de unidad anterior o siguiente, el más cercano primero"},
    {L"Alt+Page Up, Alt+Page Down: previous or next unit of the same type",
     L"Alt+Page préc., Alt+Page suiv. : unité précédente ou suivante du même type",
     L"Alt+Bild auf, Alt+Bild ab: vorherige oder nächste Einheit desselben Typs",
     L"Alt+Re Pág, Alt+Av Pág: unidad anterior o siguiente del mismo tipo"},
    {L"Ctrl+Page Up, Ctrl+Page Down: scanner category: our units, enemy units, generals",
     L"Ctrl+Page préc., Ctrl+Page suiv. : catégorie du scanner : nos unités, unités ennemies, généraux",
     L"Strg+Bild auf, Strg+Bild ab: Scannerkategorie: eigene Einheiten, feindliche Einheiten, Feldherren",
     L"Ctrl+Re Pág, Ctrl+Av Pág: categoría del escáner: nuestras unidades, unidades enemigas, generales"},
    {L"Home: go to the scanned unit; End: where it is from the cursor",
     L"Origine : aller à l'unité scannée ; Fin : où elle est par rapport au curseur",
     L"Pos1: zur gescannten Einheit gehen; Ende: wo sie vom Cursor aus liegt",
     L"Inicio: ir a la unidad escaneada; Fin: dónde está respecto al cursor"},
    {L"Left and right brackets: message category; comma, period: older, newer message",
     L"Crochets gauche et droit : catégorie de messages ; virgule, point : message plus ancien, plus récent",
     L"Linke und rechte eckige Klammer: Nachrichtenkategorie; Komma, Punkt: ältere, neuere Nachricht",
     L"Corchetes izquierdo y derecho: categoría de mensajes; coma, punto: mensaje anterior, siguiente"},
    {L"Shift with brackets: first, last category; Shift with comma, period: oldest, newest message",
     L"Maj avec les crochets : première, dernière catégorie ; Maj avec virgule, point : message le plus ancien, le plus récent",
     L"Umschalt mit eckigen Klammern: erste, letzte Kategorie; Umschalt mit Komma, Punkt: älteste, neueste Nachricht",
     L"Mayús con corchetes: primera, última categoría; Mayús con coma, punto: mensaje más antiguo, más reciente"},
    {L"L: unit list, Enter selects the unit",
     L"L : liste des unités, Entrée sélectionne l'unité",
     L"L: Einheitenliste, Enter wählt die Einheit aus",
     L"L: lista de unidades, Intro selecciona la unidad"},
    {L"K: casualties",
     L"K : pertes",
     L"K: Verluste",
     L"K: bajas"},
    {L"Tab: next unit that hasn't moved; B: next unit that can shoot",
     L"Tab : prochaine unité qui n'a pas bougé ; B : prochaine unité qui peut tirer",
     L"Tab: nächste Einheit, die sich nicht bewegt hat; B: nächste Einheit, die schießen kann",
     L"Tab: siguiente unidad que no se ha movido; B: siguiente unidad que puede disparar"},
    {L"Space: close a message or proceed",
     L"Espace : fermer un message ou continuer",
     L"Leertaste: Nachricht schließen oder fortfahren",
     L"Espacio: cerrar un mensaje o continuar"},
    {L"F2: screenshot",
     L"F2 : capture d'écran",
     L"F2: Bildschirmfoto",
     L"F2: captura de pantalla"},
};

Lang from_options()
{
    // Documents\My Games\FieldOfGlory2\OPTIONS.TXT, line "Lang 4673093" (= "ENG" read as an int).
    static Lang cached = EN;
    static FILETIME seen = {};
    static wchar_t path[MAX_PATH] = L"";
    if (!path[0]) {
        typedef HRESULT(WINAPI * Folder_t)(HWND, int, HANDLE, DWORD, LPWSTR);
        HMODULE shell = LoadLibraryW(L"shell32.dll");
        Folder_t folder = shell ? (Folder_t)GetProcAddress(shell, "SHGetFolderPathW") : nullptr;
        wchar_t docs[MAX_PATH];
        if (!folder || folder(nullptr, 0x0005 /* CSIDL_PERSONAL */, nullptr, 0, docs) != S_OK)
            return cached;
        lstrcpynW(path, (std::wstring(docs) + L"\\My Games\\FieldOfGlory2\\OPTIONS.TXT").c_str(), MAX_PATH);
    }
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &a) || CompareFileTime(&a.ftLastWriteTime, &seen) == 0)
        return cached;
    seen = a.ftLastWriteTime;
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE)
        return cached;
    char buf[8192];
    DWORD got = 0;
    ReadFile(f, buf, sizeof buf - 1, &got, nullptr);
    CloseHandle(f);
    buf[got] = 0;
    for (char *p = buf; (p = strstr(p, "Lang")) != nullptr; p += 4) {
        if (p != buf && p[-1] != '\n')
            continue;
        unsigned v = strtoul(p + 4, nullptr, 10);
        char code[4] = {(char)(v & 0xff), (char)(v >> 8 & 0xff), (char)(v >> 16 & 0xff), 0};
        cached = !strcmp(code, "FRE") ? FR : !strcmp(code, "GER") ? DE : !strcmp(code, "SPA") ? ES : EN;
        break;
    }
    return cached;
}

} // namespace

Lang current()
{
    // Asked often (every phrase): look again at most twice a second.
    static Lang cached = EN;
    static DWORD at = 0;
    DWORD now = GetTickCount();
    if (at && now - at < 500)
        return cached;
    at = now;
    std::wstring yes = eng::game_string("IDS_CONFIRM_YES");
    for (wchar_t &c : yes)
        c = towlower(c);
    if (yes.empty())
        cached = from_options(); // the launcher, or the game before its strings are loaded
    else
        cached = yes == L"oui" ? FR : yes == L"ja" ? DE : yes == L"si" || yes == L"sí" ? ES : EN;
    return cached;
}

const wchar_t *tr(const wchar_t *en)
{
    Lang l = current();
    if (l == EN)
        return en;
    for (const Phrase &p : phrases)
        if (!wcscmp(p.en, en)) {
            const wchar_t *t = l == FR ? p.fr : l == DE ? p.de : p.es;
            return t && *t ? t : en;
        }
    return en;
}

std::wstring trf(const wchar_t *en, std::initializer_list<std::wstring> args)
{
    std::wstring s = tr(en);
    size_t k = 0;
    for (const std::wstring &a : args) {
        std::wstring mark = L"{" + std::to_wstring(k++) + L"}";
        for (size_t at = s.find(mark); at != std::wstring::npos; at = s.find(mark, at + a.size()))
            s.replace(at, mark.size(), a);
    }
    return s;
}

const wchar_t *key_word() { return tr(L"Enter"); }
const wchar_t *pressing_key() { return tr(L"pressing Enter"); }

bool click_instruction(const std::wstring &text, std::wstring &name, size_t &from, size_t &to)
{
    // One pattern per language for the phrase naming the icon; group 1 (or 2) is the order's name.
    // English: "on the Shoot icon", "on the~Turn and Shoot icon" (line breaks are spaces here).
    // French: "sur l'icone Tirer pour...", "l'icône Transférer Général.", "l'icon Ordre".
    // German: "auf das Symbol 'Schießen'", "auf das 'Angreifen'-Symbol".
    // Spanish: "en el icono Mover.", "en el icono de Carga para...", "sobre el icono de Transferir General";
    // "su quieres" is the game's typo for "si quieres" (IDS_TT_CAN_TURN_AND_SHOOT0).
    static const std::wregex patterns[] = {
        std::wregex(L"\\bthe\\s+((?:(?!\\bthe\\b)[^.\\n])+?)\\s+icon\\b"),
        std::wregex(L"\\bl'ic[oô]ne?\\s+(.+?)(?=\\s+(?:icon|si|pour|ou)\\b|\\s*\\(|[.,;]|$)"),
        std::wregex(L"Symbol\\s+'([^']+)'|'([^']+)'-Symbol"),
        std::wregex(L"\\bicono\\s+(?:de\\s+(?:la\\s+)?)?(.+?)(?=\\s+(?:para|si|su|o)\\b|\\s*\\(|[.,;]|$)"),
    };
    std::wstring flat = text;
    for (wchar_t &c : flat)
        if (c == L'\n')
            c = L' ';
    std::wsmatch m;
    if (!std::regex_search(flat, m, patterns[current()]))
        return false;
    int g = m[1].matched ? 1 : 2;
    name = m[g].str();
    while (!name.empty() && iswspace(name.back()))
        name.pop_back();
    from = (size_t)m.position(0);
    to = from + (size_t)m.length(0);
    return !name.empty();
}

} // namespace lang
