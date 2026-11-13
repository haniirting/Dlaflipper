/*
 * Multi RFID Switcher
 * ---------------------------------------------------------
 * Показывает список сохранённых LF RFID карт (/ext/lfrfid/*.rfid)
 * и позволяет переключаться между ними и эмулировать выбранную
 * нажатием кнопки OK. Кнопка "Назад" возвращает к списку.
 *
 * Собирается как обычное внешнее приложение (.fap) через
 * ufbt (micro Flipper Build Tool).
 */

#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <gui/modules/submenu.h>
#include <gui/modules/popup.h>
#include <storage/storage.h>
#include <lfrfid/lfrfid_worker.h>
#include <lfrfid/lfrfid_dict_file.h>
#include <toolbox/path.h>

#define TAG "MultiRFID"
#define RFID_FOLDER "/ext/lfrfid"
#define RFID_EXT ".rfid"
#define MAX_CARDS 32

typedef enum {
    MultiRfidViewSubmenu,
    MultiRfidViewPopup,
} MultiRfidView;

typedef struct {
    FuriString* names[MAX_CARDS]; // отображаемые имена
    FuriString* paths[MAX_CARDS]; // полные пути к .rfid файлам
    size_t count;
} CardList;

typedef struct {
    Gui* gui;
    ViewDispatcher* view_dispatcher;
    Submenu* submenu;
    Popup* popup;
    Storage* storage;
    LFRFIDWorker* worker;
    CardList cards;
    int32_t selected_index;
    bool emulating;
} MultiRfidApp;

/* --- Сканируем папку /ext/lfrfid и собираем список карт --- */
static void card_list_scan(MultiRfidApp* app) {
    CardList* list = &app->cards;
    list->count = 0;

    File* dir = storage_file_alloc(app->storage);
    if(storage_dir_open(dir, RFID_FOLDER)) {
        FileInfo file_info;
        char name_buf[256];
        while(list->count < MAX_CARDS &&
              storage_dir_read(dir, &file_info, name_buf, sizeof(name_buf))) {
            if(file_info.flags & FSF_DIRECTORY) continue;

            FuriString* fname = furi_string_alloc_set(name_buf);
            if(furi_string_end_with(fname, RFID_EXT)) {
                // Имя без расширения — для отображения в меню
                FuriString* display = furi_string_alloc_set(fname);
                size_t ext_pos = furi_string_search_rstr(display, RFID_EXT);
                if(ext_pos != FURI_STRING_FAILURE) {
                    furi_string_left(display, ext_pos);
                }

                FuriString* full_path = furi_string_alloc_printf(
                    "%s/%s", RFID_FOLDER, furi_string_get_cstr(fname));

                list->names[list->count] = display;
                list->paths[list->count] = full_path;
                list->count++;
            }
            furi_string_free(fname);
        }
        storage_dir_close(dir);
    }
    storage_file_free(dir);

    FURI_LOG_I(TAG, "Found %zu saved RFID cards", list->count);
}

static void card_list_free(MultiRfidApp* app) {
    for(size_t i = 0; i < app->cards.count; i++) {
        furi_string_free(app->cards.names[i]);
        furi_string_free(app->cards.paths[i]);
    }
    app->cards.count = 0;
}

/* --- Остановить текущую эмуляцию, если она идёт --- */
static void stop_emulation(MultiRfidApp* app) {
    if(app->emulating) {
        lfrfid_worker_stop(app->worker);
        lfrfid_worker_stop_thread(app->worker);
        app->emulating = false;
    }
}

/* --- Запустить эмуляцию карты по индексу в списке --- */
static void start_emulation(MultiRfidApp* app, size_t index) {
    stop_emulation(app);
    if(index >= app->cards.count) return;

    LFRFIDProtocol proto;
    ProtocolDict* dict = protocol_dict_alloc(lfrfid_protocols, LFRFIDProtocolMax);

    if(!lfrfid_dict_file_load(dict, furi_string_get_cstr(app->cards.paths[index]))) {
        FURI_LOG_E(TAG, "Failed to load %s", furi_string_get_cstr(app->cards.paths[index]));
        protocol_dict_free(dict);
        popup_set_header(app->popup, "Ошибка", 64, 10, AlignCenter, AlignTop);
        popup_set_text(app->popup, "Не удалось прочитать файл карты", 64, 30, AlignCenter, AlignTop);
        view_dispatcher_switch_to_view(app->view_dispatcher, MultiRfidViewPopup);
        return;
    }

    // Определяем протокол, записанный в файле, и запускаем эмуляцию им
    proto = protocol_dict_get_protocol_by_file(dict, furi_string_get_cstr(app->cards.paths[index]));
    (void)proto;

    lfrfid_worker_start_thread(app->worker);
    lfrfid_worker_emulate_start(app->worker, proto);
    app->emulating = true;
    app->selected_index = index;

    popup_set_header(app->popup, "Эмуляция", 64, 10, AlignCenter, AlignTop);
    popup_set_text(
        app->popup,
        furi_string_get_cstr(app->cards.names[index]),
        64, 30, AlignCenter, AlignTop);
    view_dispatcher_switch_to_view(app->view_dispatcher, MultiRfidViewPopup);

    protocol_dict_free(dict);
}

/* --- Колбэк выбора пункта в меню --- */
static void submenu_callback(void* context, uint32_t index) {
    MultiRfidApp* app = context;
    start_emulation(app, index);
}

/* --- Колбэк кнопки "Назад" из попапа эмуляции --- */
static bool popup_back_callback(void* context) {
    MultiRfidApp* app = context;
    stop_emulation(app);
    view_dispatcher_switch_to_view(app->view_dispatcher, MultiRfidViewSubmenu);
    return true;
}

static bool submenu_back_callback(void* context) {
    UNUSED(context);
    // Выход из приложения
    return false;
}

/* --- Инициализация приложения --- */
static MultiRfidApp* multi_rfid_app_alloc(void) {
    MultiRfidApp* app = malloc(sizeof(MultiRfidApp));
    memset(app, 0, sizeof(MultiRfidApp));

    app->gui = furi_record_open(RECORD_GUI);
    app->storage = furi_record_open(RECORD_STORAGE);
    app->worker = lfrfid_worker_alloc();

    app->view_dispatcher = view_dispatcher_alloc();
    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);

    app->submenu = submenu_alloc();
    submenu_set_header(app->submenu, "Мои карты");
    view_set_previous_callback(submenu_get_view(app->submenu), submenu_back_callback);
    view_dispatcher_add_view(
        app->view_dispatcher, MultiRfidViewSubmenu, submenu_get_view(app->submenu));

    app->popup = popup_alloc();
    view_set_previous_callback(popup_get_view(app->popup), (void*)popup_back_callback);
    view_dispatcher_add_view(app->view_dispatcher, MultiRfidViewPopup, popup_get_view(app->popup));

    card_list_scan(app);
    for(size_t i = 0; i < app->cards.count; i++) {
        submenu_add_item(
            app->submenu,
            furi_string_get_cstr(app->cards.names[i]),
            i,
            submenu_callback,
            app);
    }

    app->selected_index = -1;
    app->emulating = false;

    return app;
}

static void multi_rfid_app_free(MultiRfidApp* app) {
    stop_emulation(app);
    lfrfid_worker_free(app->worker);

    view_dispatcher_remove_view(app->view_dispatcher, MultiRfidViewSubmenu);
    view_dispatcher_remove_view(app->view_dispatcher, MultiRfidViewPopup);
    submenu_free(app->submenu);
    popup_free(app->popup);
    view_dispatcher_free(app->view_dispatcher);

    card_list_free(app);

    furi_record_close(RECORD_GUI);
    furi_record_close(RECORD_STORAGE);
    free(app);
}

/* --- Точка входа --- */
int32_t multi_rfid_app(void* p) {
    UNUSED(p);
    MultiRfidApp* app = multi_rfid_app_alloc();

    if(app->cards.count == 0) {
        popup_set_header(app->popup, "Нет карт", 64, 10, AlignCenter, AlignTop);
        popup_set_text(
            app->popup,
            "Сначала сохраните карты\nчерез 125kHz RFID -> Read -> Save",
            64, 30, AlignCenter, AlignTop);
        view_dispatcher_switch_to_view(app->view_dispatcher, MultiRfidViewPopup);
    } else {
        view_dispatcher_switch_to_view(app->view_dispatcher, MultiRfidViewSubmenu);
    }

    view_dispatcher_run(app->view_dispatcher);

    multi_rfid_app_free(app);
    return 0;
}
