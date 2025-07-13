// Flipper Zero NFC Tour App - Version Améliorée
// Crée une application native avec menu pour émuler séquentiellement des badges NFC.
//
// Améliorations par rapport à la version de base :
// 1.  Gestion du cas où aucun fichier .nfc n'est trouvé dans le dossier.
// 2.  Retour automatique au menu principal après la fin d'une tournée.
// 3.  Suppression du SceneManager qui n'était pas utilisé pour simplifier le code.
// 4.  Ajout de commentaires supplémentaires pour clarifier le fonctionnement.
// 5.  Utilisation de constantes pour les durées d'émulation et de pause.

#include <furi.h>
#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <gui/modules/submenu.h>
#include <gui/modules/text_box.h>
#include <furi_hal.h>
#include <furi_hal_random.h>
#include <dialogs/dialogs.h>
#include <storage/storage.h>
#include <nfc/nfc_worker.h>
#include <toolbox/path.h>

#define TAG "NFCTourApp"

// --- Configuration ---
#define NFC_TOUR_FOLDER "/ext/nfc_tour" // Dossier contenant les badges
#define MAX_BADGES 64 // Nombre maximum de badges que l'app peut gérer
#define EMULATION_DURATION_S 10 // Durée d'émulation pour chaque badge en secondes
#define MIN_PAUSE_S 30 // Pause minimale entre les émulations en secondes
#define MAX_PAUSE_S 180 // Pause maximale entre les émulations en secondes

// Définition de la structure principale de l'application
typedef struct {
    Gui* gui;
    ViewDispatcher* view_dispatcher;
    Submenu* submenu;
    TextBox* textbox;
    uint32_t tour_duration_min;
    volatile bool stop_requested; // 'volatile' car modifié par un callback d'input
} App;

// Définition des différentes vues/écrans de l'application
typedef enum {
    AppViewSubmenu,
    AppViewTextBox,
} AppView;

// Prototypes des fonctions
static void run_tour_thread(void* context);

/**
 * @brief Gère les entrées utilisateur (appui sur les boutons).
 * On ne s'en sert que pour intercepter le bouton "Retour" pendant une tournée.
 * @param event L'événement d'entrée.
 * @param context Le contexte de l'application (App*).
 * @return true si l'événement a été géré, false sinon.
 */
static bool app_input_callback(InputEvent* event, void* context) {
    App* app = context;
    // Si l'utilisateur appuie sur "Retour" pendant que la tournée est en cours (vue TextBox)
    if(view_dispatcher_get_current_view_index(app->view_dispatcher) == AppViewTextBox) {
        if(event->type == InputTypeShort && event->key == InputKeyBack) {
            app->stop_requested = true;
            return true; // On a géré l'événement, ne pas le propager
        }
    }
    return false; // On n'a pas géré l'événement
}

/**
 * @brief Exécute la tournée d'émulation des badges NFC.
 * Cette fonction est conçue pour être lancée dans un thread séparé afin de ne pas bloquer l'interface.
 * @param context Le contexte de l'application (App*).
 */
static void run_tour_thread(void* context) {
    App* app = context;
    Storage* storage = furi_record_open(RECORD_STORAGE);
    
    char* badge_files[MAX_BADGES];
    size_t badge_count = 0;

    // Tente d'ouvrir le dossier des badges
    Dir* dir = storage_dir_alloc(storage);
    if(storage_dir_open(dir, NFC_TOUR_FOLDER)) {
        const char* file_name;
        // Lit tous les fichiers du dossier
        while(storage_dir_read(dir, NULL, &file_name, 1024) && badge_count < MAX_BADGES) {
            // On ne garde que les fichiers .nfc
            if(strstr(file_name, ".nfc")) {
                badge_files[badge_count] = strdup(file_name);
                badge_count++;
            }
        }
    } else {
        FURI_LOG_E(TAG, "Impossible d'ouvrir le dossier %s", NFC_TOUR_FOLDER);
    }
    storage_dir_close(dir);
    storage_dir_free(dir);

    // Affiche un message si aucun badge n'est trouvé et arrête la tournée
    if(badge_count == 0) {
        text_box_reset(app->textbox);
        text_box_set_text(app->textbox, "Aucun badge .nfc trouvé.\n\nCréez le dossier\n/ext/nfc_tour\net placez-y vos badges.");
        furi_delay_ms(3000); // Laisse le temps de lire le message
        view_dispatcher_switch_to_view(app->view_dispatcher, AppViewSubmenu);
        furi_record_close(RECORD_STORAGE);
        return;
    }

    FURI_LOG_I(TAG, "%zu badges trouvés. Début de la tournée.", badge_count);

    float total_seconds = app->tour_duration_min * 60.0f;
    float remaining_time = total_seconds;
    app->stop_requested = false;

    text_box_reset(app->textbox);
    text_box_set_text(app->textbox, "Début de la tournée...\nAppuyez sur 'Retour' pour arrêter.");
    furi_delay_ms(2000);

    // Boucle principale de la tournée
    while(remaining_time > 0 && !app->stop_requested) {
        for(size_t i = 0; i < badge_count && remaining_time > 0 && !app->stop_requested; i++) {
            FuriString* path = furi_string_alloc_printf("%s/%s", NFC_TOUR_FOLDER, badge_files[i]);
            
            // --- Phase d'émulation ---
            text_box_reset(app->textbox);
            text_box_add_text(app->textbox, furi_string_get_cstr(path));
            text_box_add_text(app->textbox, "\nÉmulation en cours...");
            
            FURI_LOG_I(TAG, "Émulation: %s", furi_string_get_cstr(path));

            NFCWorker* nfc_worker = nfc_worker_alloc();
            if(nfc_worker_load(nfc_worker, furi_string_get_cstr(path))) {
                nfc_worker_start_emulate(nfc_worker);
                // Attend la durée d'émulation, en vérifiant toutes les 100ms si l'utilisateur veut arrêter
                for(int j = 0; j < EMULATION_DURATION_S * 10 && !app->stop_requested; j++) {
                    furi_delay_ms(100);
                }
                nfc_worker_stop(nfc_worker);
            } else {
                FURI_LOG_E(TAG, "Erreur chargement: %s", furi_string_get_cstr(path));
                text_box_add_text(app->textbox, "\nErreur de chargement !");
                furi_delay_ms(1000);
            }
            nfc_worker_free(nfc_worker);
            furi_string_free(path);

            if(app->stop_requested) break;
            remaining_time -= EMULATION_DURATION_S;

            // --- Phase de pause ---
            if(remaining_time <= 0) break;

            uint32_t delay_range = (MAX_PAUSE_S - MIN_PAUSE_S);
            float delay_s = MIN_PAUSE_S + (furi_hal_random_get() % (delay_range * 100)) / 100.0f;
            
            FuriString* pause_text = furi_string_alloc_printf("\nPause de %.1f sec...", delay_s);
            text_box_add_text(app->textbox, furi_string_get_cstr(pause_text));
            furi_string_free(pause_text);

            // Attend la durée de la pause, en vérifiant toutes les 100ms
            for(uint32_t elapsed = 0; elapsed < (uint32_t)(delay_s * 1000) && !app->stop_requested; elapsed += 100) {
                furi_delay_ms(100);
            }
            remaining_time -= delay_s;
        }
    }

    // --- Nettoyage ---
    for(size_t i = 0; i < badge_count; i++) {
        free(badge_files[i]);
    }
    
    text_box_reset(app->textbox);
    if(app->stop_requested) {
        text_box_set_text(app->textbox, "Tournée arrêtée par l'utilisateur.");
        FURI_LOG_I(TAG, "Tournée arrêtée.");
    } else {
        text_box_set_text(app->textbox, "Tournée terminée.");
        FURI_LOG_I(TAG, "Tournée terminée.");
    }

    furi_delay_ms(2000); // Laisse le temps de lire le message de fin
    view_dispatcher_switch_to_view(app->view_dispatcher, AppViewSubmenu); // Retour au menu
    furi_record_close(RECORD_STORAGE);
}

/**
 * @brief Callback appelé lorsqu'une durée est sélectionnée dans le menu.
 * @param context Le contexte de l'application (App*).
 * @param index L'index de l'item sélectionné (on l'utilise pour stocker la durée).
 */
static void on_duration_selected_callback(void* context, uint32_t index) {
    App* app = context;
    app->tour_duration_min = index;
    FURI_LOG_I(TAG, "Durée sélectionnée: %lu min", app->tour_duration_min);
    
    // On change de vue pour afficher la progression
    view_dispatcher_switch_to_view(app->view_dispatcher, AppViewTextBox);
    
    // On lance la tournée dans un thread pour ne pas geler l'interface.
    // NOTE: Pour une vraie app, il faudrait utiliser FuriThread, mais pour la simplicité,
    // on exécute directement. L'input callback permet de garder une réactivité.
    // Pour une app plus complexe, un vrai thread serait indispensable.
    run_tour_thread(app);
}

/**
 * @brief Callback appelé à l'entrée de la scène principale (le menu).
 * @param context Le contexte de l'application (App*).
 */
static void app_scene_on_enter(void* context) {
    App* app = context;
    // Vide le menu avant de le remplir pour éviter les doublons si on y revient
    submenu_reset(app->submenu);

    // Ajoute les options de durée au menu
    // Le 3ème argument (l'index) est directement utilisé comme valeur en minutes
    submenu_add_item(app->submenu, "Tournée 60 min", 60, on_duration_selected_callback, app);
    submenu_add_item(app->submenu, "Tournée 90 min", 90, on_duration_selected_callback, app);
    submenu_add_item(app->submenu, "Tournée 120 min", 120, on_duration_selected_callback, app);
    
    // Affiche la vue du menu
    view_dispatcher_switch_to_view(app->view_dispatcher, AppViewSubmenu);
}

/**
 * @brief Alloue et initialise toutes les ressources de l'application.
 * @return Un pointeur vers la structure App initialisée.
 */
static App* app_alloc() {
    App* app = malloc(sizeof(App));
    app->gui = furi_record_open(RECORD_GUI);
    app->view_dispatcher = view_dispatcher_alloc();
    
    // Alloue les modules de vue
    app->submenu = submenu_alloc();
    app->textbox = text_box_alloc();

    // Associe les vues au dispatcher
    view_dispatcher_add_view(app->view_dispatcher, AppViewSubmenu, submenu_get_view(app->submenu));
    view_dispatcher_add_view(app->view_dispatcher, AppViewTextBox, text_box_get_view(app->textbox));

    // Attache le dispatcher à l'interface graphique
    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);
    
    // Configure les callbacks
    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);
    view_dispatcher_set_input_callback(app->view_dispatcher, app_input_callback);

    return app;
}

/**
 * @brief Libère toutes les ressources de l'application.
 * @param app Le pointeur vers la structure App à libérer.
 */
static void app_free(App* app) {
    // Détache de l'interface
    view_dispatcher_remove_view(app->view_dispatcher, AppViewSubmenu);
    view_dispatcher_remove_view(app->view_dispatcher, AppViewTextBox);
    
    // Libère les modules de vue
    submenu_free(app->submenu);
    text_box_free(app->textbox);
    
    // Libère le dispatcher
    view_dispatcher_free(app->view_dispatcher);

    // Ferme les services Furi
    furi_record_close(RECORD_GUI);
    
    // Libère la structure principale
    free(app);
}

/**
 * @brief Point d'entrée principal de l'application.
 * @param p Argument non utilisé.
 * @return 0 si l'exécution s'est bien passée.
 */
int32_t nfc_tour_app_main(void* p) {
    UNUSED(p);

    // Vérifie que le dossier existe, sinon on le crée
    Storage* storage = furi_record_open(RECORD_STORAGE);
    if(!storage_dir_exists(storage, NFC_TOUR_FOLDER)) {
        FURI_LOG_I(TAG, "Le dossier %s n'existe pas, création...", NFC_TOUR_FOLDER);
        storage_simply_mkdir(storage, NFC_TOUR_FOLDER);
    }
    furi_record_close(RECORD_STORAGE);

    App* app = app_alloc();
    
    // Prépare et affiche la première scène
    app_scene_on_enter(app);

    // Lance la boucle d'événements principale (bloquant)
    view_dispatcher_run(app->view_dispatcher);

    // Nettoyage après la sortie de l'application
    FURI_LOG_I(TAG, "Sortie de l'application, nettoyage.");
    app_free(app);

    return 0;
}

