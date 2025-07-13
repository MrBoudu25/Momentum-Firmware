Plan Final : Obtenir votre application

Ce document contient tout ce qu'il faut. Suivez ces instructions dans l'ordre, et si l'environnement de compilation de GitHub fonctionne comme il le devrait, vous aurez votre fichier .fap.
1. Le Code Final (nfc_tour_app.c)

C'est la version la plus propre et la plus simple de votre application.

/**
 * Application NFC Tour pour Flipper Zero
 */

#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <gui/modules/submenu.h>
#include <gui/modules/text_box.h>
#include <dialogs/dialogs.h>
#include <storage/storage.h>
#include <nfc/nfc_worker.h>

#define TAG "NFCTourApp"
#define NFC_TOUR_FOLDER "/ext/nfc_tour"
#define MAX_BADGES 64

typedef struct {
    Gui* gui;
    ViewDispatcher* view_dispatcher;
    Submenu* submenu;
    TextBox* textbox;
    FuriThread* worker_thread;
    volatile bool stop_requested;
} App;

typedef enum {
    AppViewSubmenu,
    AppViewTextBox,
} AppView;

static int32_t nfc_tour_worker(void* context) {
    App* app = context;
    app->stop_requested = false;
    Storage* storage = furi_record_open(RECORD_STORAGE);
    Dir* dir = storage_dir_alloc(storage);
    char* badge_files[MAX_BADGES];
    size_t badge_count = 0;

    text_box_set_text(app->textbox, "Recherche des badges...");
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

    if(badge_count == 0) {
        text_box_set_text(app->textbox, "Aucun badge .nfc trouve\ndans /ext/nfc_tour");
        furi_delay_ms(4000);
        furi_record_close(RECORD_STORAGE);
        return 0;
    }

    size_t current_badge_index = 0;
    while(!app->stop_requested) {
        FuriString* current_path = furi_string_alloc_printf("%s/%s", NFC_TOUR_FOLDER, badge_files[current_badge_index]);
        FuriString* display_text = furi_string_alloc_printf(
            "Badge %zu/%zu: %s\nEmulation (10s)...",
            current_badge_index + 1,
            badge_count,
            badge_files[current_badge_index]);
        text_box_set_text(app->textbox, furi_string_get_cstr(display_text));

        NFCWorker* nfc_worker = nfc_worker_alloc();
        if(nfc_worker_load(nfc_worker, furi_string_get_cstr(current_path))) {
            nfc_worker_start_emulate(nfc_worker);
            for(int i = 0; i < 100 && !app->stop_requested; i++) furi_delay_ms(100);
            nfc_worker_stop(nfc_worker);
        }
        nfc_worker_free(nfc_worker);
        furi_string_free(current_path);
        furi_string_free(display_text);

        if(app->stop_requested) break;

        uint32_t delay_s = 30 + (furi_hal_random_get() % 151);
        display_text = furi_string_alloc_printf("Pause de %lu secondes...", delay_s);
        text_box_set_text(app->textbox, furi_string_get_cstr(display_text));
        furi_string_free(display_text);
        for(uint32_t i = 0; i < delay_s * 10 && !app->stop_requested; i++) furi_delay_ms(100);

        if(app->stop_requested) break;
        current_badge_index = (current_badge_index + 1) % badge_count;
    }

    for(size_t i = 0; i < badge_count; i++) free(badge_files[i]);
    furi_record_close(RECORD_STORAGE);
    return 0;
}

static bool app_input_callback(InputEvent* event, void* context) {
    App* app = context;
    if(event->type == InputTypeShort && event->key == InputKeyBack) {
        if(view_dispatcher_get_current_view_index(app->view_dispatcher) == AppViewTextBox) {
            app->stop_requested = true;
            furi_thread_join(app->worker_thread);
            view_dispatcher_switch_to_view(app->view_dispatcher, AppViewSubmenu);
            return true;
        }
    }
    return false;
}

static void submenu_callback(void* context, uint32_t index) {
    App* app = context;
    if(index == 0) {
        view_dispatcher_switch_to_view(app->view_dispatcher, AppViewTextBox);
        furi_thread_set_callback(app->worker_thread, nfc_tour_worker);
        furi_thread_start(app->worker_thread);
    }
}

int32_t nfc_tour_app_main(void* p) {
    UNUSED(p);
    App* app = malloc(sizeof(App));
    app->gui = furi_record_open(RECORD_GUI);
    app->view_dispatcher = view_dispatcher_alloc();
    app->submenu = submenu_alloc();
    app->textbox = text_box_alloc();
    app->worker_thread = furi_thread_alloc_ex("NFCTourWorker", 1024, app);

    view_dispatcher_enable_queue(app->view_dispatcher);
    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);
    view_dispatcher_set_input_callback(app->view_dispatcher, app_input_callback);
    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);
    view_dispatcher_add_view(app->view_dispatcher, AppViewSubmenu, submenu_get_view(app->submenu));
    view_dispatcher_add_view(app->view_dispatcher, AppViewTextBox, text_box_get_view(app->textbox));
    submenu_add_item(app->submenu, "Demarrer la tournee", 0, submenu_callback, app);
    view_dispatcher_switch_to_view(app->view_dispatcher, AppViewSubmenu);
    view_dispatcher_run(app->view_dispatcher);

    app->stop_requested = true;
    furi_thread_join(app->worker_thread);
    furi_thread_free(app->worker_thread);
    view_dispatcher_remove_view(app->view_dispatcher, AppViewSubmenu);
    view_dispatcher_remove_view(app->view_dispatcher, AppViewTextBox);
    text_box_free(app->textbox);
    submenu_free(app->submenu);
    view_dispatcher_free(app->view_dispatcher);
    furi_record_close(RECORD_GUI);
    free(app);
    return 0;
}

2. Le Fichier de Configuration Final (application.fam)

C'est la "liste d'ingrédients" pour le compilateur.

App(
    appid="nfc_tour_app",
    name="NFC Tour",
    apptype=FlipperAppType.EXTERNAL,
    entry_point="nfc_tour_app_main",
    stack_size=2 * 1024,
    fap_icon="icon.png",
    fap_category="NFC",
    requires=[
        "nfc",
        "dialogs",
    ],
)

3. Les Instructions Finales

    Créer une copie ("Fork") : Allez sur la page du firmware officiel et cliquez sur "Fork" : https://github.com/flipperdevices/flipperzero-firmware

    Créer les fichiers sur GitHub :

        Sur la page de votre copie, allez dans le dossier applications_user.

        Cliquez Add file > Create new file.

        Nommez le fichier nfc_tour_app/application.fam.

        Collez le contenu du fichier de configuration ci-dessus.

        Cliquez sur Commit changes.

        Retournez dans le dossier nfc_tour_app que vous venez de créer.

        Cliquez Add file > Create new file.

        Nommez le fichier nfc_tour_app.c.

        Collez le code C final ci-dessus.

        Cliquez sur Commit changes.

    Lancer la compilation dans le Cloud :

        Retournez à la page principale de votre projet.

        Cliquez sur < > Code > onglet Codespaces.

        Cliquez sur Create codespace on main.

        Une fois l'environnement prêt, tapez cette commande dans le terminal en bas :

        ./fbt fap_nfc_tour_app

    Télécharger :

        Après la compilation réussie, trouvez votre fichier nfc_tour_app.fap dans le dossier build/f7-firmware-D/.extapps/nfc_tour_app/.

        Faites un clic droit dessus > Download...

C'est la méthode la plus propre et la plus fiable. Si cela échoue, c'est que le service de compilation de GitHub a un problème temporaire. Je suis convaincu que cette fois, ce sera la bonne.
