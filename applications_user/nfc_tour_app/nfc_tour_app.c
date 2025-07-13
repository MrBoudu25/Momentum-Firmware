/**
 * Application NFC Tour pour Flipper Zero
 * Basé sur le code original fourni par l'utilisateur.
 *
 * Cette version est modernisée pour utiliser les dernières API du Flipper
 * et inclut une meilleure gestion des erreurs.
 */

#include <furi.h>
#include <furi_hal.h>

// --- Interfaces graphiques ---
#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <gui/modules/submenu.h>
#include <gui/modules/text_box.h>
#include <dialogs/dialogs.h>

// --- API nécessaires ---
#include <storage/storage.h> // Pour lire les fichiers sur la carte SD
#include <nfc/nfc_worker.h>   // Pour émuler les badges NFC

// --- Constantes de l'application ---
#define TAG "NFCTourApp"
#define NFC_TOUR_FOLDER "/ext/nfc_tour" // Le dossier où stocker les badges .nfc
#define MAX_BADGES 64                   // Nombre maximum de badges gérés

// --- Structure principale de l'application ---
// Contient tous les éléments dont notre application a besoin
typedef struct {
    Gui* gui;
    ViewDispatcher* view_dispatcher;
    Submenu* submenu;
    TextBox* textbox;
    DialogsApp* dialogs;

    FuriThread* worker_thread; // Un thread pour que la tournée ne bloque pas l'interface
    volatile bool stop_requested;
} App;

// --- Vues de l'application ---
typedef enum {
    AppViewSubmenu,
    AppViewTextBox,
} AppView;

/**
 * La fonction qui exécute la tournée.
 * Elle tourne dans un thread séparé pour ne pas geler l'interface.
 */
static int32_t nfc_tour_worker(void* context) {
    App* app = context;
    app->stop_requested = false;

    Storage* storage = furi_record_open(RECORD_STORAGE);
    Dir* dir = storage_dir_alloc(storage);

    char* badge_files[MAX_BADGES];
    size_t badge_count = 0;

    // Affiche un message de chargement
    text_box_set_text(app->textbox, "Recherche des badges\ndans " NFC_TOUR_FOLDER "...");
    
    // Vérifie si le dossier existe et lit les fichiers .nfc
    if(storage_dir_open(dir, NFC_TOUR_FOLDER)) {
        const char* file_name;
        while(storage_dir_read(dir, NULL, &file_name, 1024) && badge_count < MAX_BADGES) {
            if(strstr(file_name, ".nfc")) {
                badge_files[badge_count] = strdup(file_name);
                badge_count++;
            }
        }
    }

    storage_dir_close(dir);
    storage_dir_free(dir);

    // Si aucun badge n'est trouvé, affiche un message et s'arrête.
    if(badge_count == 0) {
        text_box_set_text(app->textbox, "Aucun badge .nfc trouvé.\n\nCréez le dossier\n" NFC_TOUR_FOLDER "\net placez-y vos badges.");
        furi_delay_ms(4000);
        furi_record_close(RECORD_STORAGE);
        return 0;
    }

    // Boucle principale de la tournée
    size_t current_badge_index = 0;
    while(!app->stop_requested) {
        // --- Phase 1: Émulation du badge ---
        FuriString* current_path = furi_string_alloc_printf("%s/%s", NFC_TOUR_FOLDER, badge_files[current_badge_index]);
        FuriString* display_text = furi_string_alloc_printf(
            "Badge %zu/%zu\n%s\n\nEmulation (10s)...",
            current_badge_index + 1,
            badge_count,
            badge_files[current_badge_index]);
        text_box_set_text(app->textbox, furi_string_get_cstr(display_text));

        NFCWorker* nfc_worker = nfc_worker_alloc();
        if(nfc_worker_load(nfc_worker, furi_string_get_cstr(current_path))) {
            nfc_worker_start_emulate(nfc_worker);
            // Attend 10 secondes, en vérifiant toutes les 100ms si on doit s'arrêter
            for(int i = 0; i < 100 && !app->stop_requested; i++) {
                furi_delay_ms(100);
            }
            nfc_worker_stop(nfc_worker);
        } else {
            furi_string_cat_printf(display_text, "\nErreur chargement !");
            text_box_set_text(app->textbox, furi_string_get_cstr(display_text));
            furi_delay_ms(2000);
        }
        nfc_worker_free(nfc_worker);
        furi_string_free(current_path);
        furi_string_free(display_text);

        if(app->stop_requested) break;

        // --- Phase 2: Pause aléatoire ---
        uint32_t delay_s = 30 + (furi_hal_random_get() % 151); // Pause entre 30s et 3min
        display_text = furi_string_alloc_printf("Pause de %lu secondes...", delay_s);
        text_box_set_text(app->textbox, furi_string_get_cstr(display_text));
        furi_string_free(display_text);

        for(uint32_t i = 0; i < delay_s * 10 && !app->stop_requested; i++) {
            furi_delay_ms(100);
        }

        if(app->stop_requested) break;

        // Passe au badge suivant (en boucle)
        current_badge_index = (current_badge_index + 1) % badge_count;
    }

    // Nettoyage
    for(size_t i = 0; i < badge_count; i++) {
        free(badge_files[i]);
    }
    furi_record_close(RECORD_STORAGE);
    return 0;
}

/**
 * Callback pour gérer les entrées (bouton retour)
 */
static bool app_input_callback(InputEvent* event, void* context) {
    App* app = context;
    if(event->type == InputTypeShort && event->key == InputKeyBack) {
        // Si on est dans la vue de la tournée, on demande l'arrêt du thread
        if(view_dispatcher_get_current_view_index(app->view_dispatcher) == AppViewTextBox) {
            app->stop_requested = true;
            furi_thread_join(app->worker_thread); // On attend que le thread se termine proprement
            view_dispatcher_switch_to_view(app->view_dispatcher, AppViewSubmenu);
            return true;
        }
    }
    return false; // Laisse le système gérer les autres cas
}

/**
 * Callback quand on choisit une option dans le menu
 */
static void submenu_callback(void* context, uint32_t index) {
    App* app = context;
    if(index == 0) { // "Démarrer la tournée"
        view_dispatcher_switch_to_view(app->view_dispatcher, AppViewTextBox);
        // On lance la fonction nfc_tour_worker dans un thread séparé
        furi_thread_set_callback(app->worker_thread, nfc_tour_worker);
        furi_thread_start(app->worker_thread);
    }
}

/**
 * Point d'entrée principal de l'application
 */
int32_t nfc_tour_app_main(void* p) {
    UNUSED(p);
    App* app = malloc(sizeof(App));

    // Initialisation des composants
    app->gui = furi_record_open(RECORD_GUI);
    app->dialogs = furi_record_open(RECORD_DIALOGS);
    app->view_dispatcher = view_dispatcher_alloc();
    app->submenu = submenu_alloc();
    app->textbox = text_box_alloc();
    app->worker_thread = furi_thread_alloc_ex("NFCTourWorker", 1024, app);

    // Configuration du dispatcher de vues
    view_dispatcher_enable_queue(app->view_dispatcher);
    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);
    view_dispatcher_set_input_callback(app->view_dispatcher, app_input_callback);
    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);

    // Ajout des vues
    view_dispatcher_add_view(app->view_dispatcher, AppViewSubmenu, submenu_get_view(app->submenu));
    view_dispatcher_add_view(app->view_dispatcher, AppViewTextBox, text_box_get_view(app->textbox));

    // Configuration du menu principal
    submenu_add_item(app->submenu, "Démarrer la tournée", 0, submenu_callback, app);
    
    // Affiche la première vue (le menu)
    view_dispatcher_switch_to_view(app->view_dispatcher, AppViewSubmenu);

    // Lance la boucle principale de l'application
    view_dispatcher_run(app->view_dispatcher);

    // Nettoyage à la sortie de l'application
    app->stop_requested = true;
    furi_thread_join(app->worker_thread);
    furi_thread_free(app->worker_thread);

    view_dispatcher_remove_view(app->view_dispatcher, AppViewSubmenu);
    view_dispatcher_remove_view(app->view_dispatcher, AppViewTextBox);
    text_box_free(app->textbox);
    submenu_free(app->submenu);
    view_dispatcher_free(app->view_dispatcher);
    furi_record_close(RECORD_DIALOGS);
    furi_record_close(RECORD_GUI);
    free(app);

    return 0;
}
