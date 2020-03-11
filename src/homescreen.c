#define EFL_BETA_API_SUPPORT
#include <Efl_Ui.h>
#include <Eio.h>
#include <Efl_Canvas_Wl.h>
#include <Evas.h>

#define TABLE_COLUMNS 4
#define TABLE_ROWS 5

typedef struct _Build_Data
{
   Eo *over_container;
   Efl_Io_Manager *io_manager;
   Eo *table;
   Eo *compositor;
   int x, y;
   Evas_Object *wl;
} Build_Data;

static Build_Data *current;

static const char *launcher_apps[][3] =
  { { "Call", "", "call-start" },
    { "Contacts", "", "contact-new" },
    { "Home", "xdg-open $HOME", "user-home" },
    { "Mail", "xdg-email 'info@enlightenment.org'", "mail-send-receive" },
    { "Documents", "xdg-open $(xdg-user-dir DOCUMENTS)", "folder-documents" } };

static void
_show_wl(Build_Data *data)
{
   if (efl_pack_index_get(data->compositor, data->wl) >= 0)
     efl_pack_unpack(data->compositor, data->wl);
   efl_ui_spotlight_push(data->compositor, data->wl);
   efl_canvas_object_key_focus_set(data->wl, EINA_TRUE);
   //evas_object_focus_set(data->wl, EINA_TRUE);
}

static void
_icon_clicked_cb(void *data EINA_UNUSED, const Efl_Event *ev EINA_UNUSED)
{
   const char *command = data;
   printf("%s\n", command);
   /*efl_add(EFL_EXE_CLASS, efl_main_loop_get(),
           efl_core_command_line_command_string_set(efl_added, command),
           efl_task_run(efl_added));*/
   efl_canvas_wl_run(current->wl, command);
   //efl_wl_run(current->wl, command);
   //_show_wl(current);
}

static void
_icon_deleted_cb(void *data, const Efl_Event *ev EINA_UNUSED)
{
   free(data);
}

// If "string" begins with the "key" prefix, sets "value" to whatever comes after "key"
// up until the next \n, replacing it with a \0, in a newly allocated string
// (which must be freed).
// Returns 1 if key was found.
static int
_parse_token(const char *string, const char *key, char **value)
{
   if (eina_str_has_prefix(string, key))
     {
        int key_len = strlen(key);
        const char *end = strchr(string + key_len, '\n');
        if (!end)
          end = string + strlen(string); // Point to EOF '\0'
        int len = end - string - key_len;
        if (*value) free(*value);
        *value = eina_strndup(string + key_len, len + 1);
        *((*value) + len) = '\0';
        return 1;
     }
   return 0;
}

// Reads a .desktop file and returns the app name, the command to launch and the icon name.
// Returns 0 if it didn't work.
// Free the strings after usage.
static int
_parse_desktop_file(const char *desktop_file_path, char **app_name, char **app_command, char **app_icon_name)
{
   EINA_RW_SLICE_DECLARE(slice, 1024);
   Efl_Io_File *desktop_file;
   int ret = 0;

   desktop_file = efl_new(EFL_IO_FILE_CLASS,
                          efl_file_set(efl_added, desktop_file_path),
                          efl_io_closer_close_on_invalidate_set(efl_added, EINA_TRUE));

   if (!desktop_file)
     return 0;

   char *name = NULL, *command = NULL, *icon = NULL, *onlyshow = NULL, *nodisplay = NULL;
   while (!efl_io_reader_eos_get(desktop_file) &&
          efl_io_reader_read(desktop_file, &slice) == EINA_ERROR_NO_ERROR)
     {
        char *content = eina_rw_slice_strdup(slice);
        char *ptr = content;
        while ((ptr = strchr(ptr, '\n')))
          {
             ptr++;
             _parse_token(ptr, "Name=", &name);
             _parse_token(ptr, "Exec=", &command);
             _parse_token(ptr, "Icon=", &icon);
             _parse_token(ptr, "OnlyShowIn=", &onlyshow);
             _parse_token(ptr, "NoDisplay=", &nodisplay);
          }
        free(content);
     }
   if (name && command && icon && !onlyshow && (!nodisplay || eina_streq(nodisplay, "false")))
     {
        *app_name = name;
        *app_command = command;
        *app_icon_name = icon;
        ret = 1;
     }
   else
     {
        if (name)
          free(name);
        if (command)
          free(command);
        if (icon)
          free(icon);
     }
   if (onlyshow)
     free(onlyshow);
   if (nodisplay)
     free(nodisplay);

   efl_unref(desktop_file);

   return ret;
}

// Creates a widget "button" with the specified name, icon and command
// to execute on click.
// These buttons are actually just an image with a label below.
static Efl_Ui_Widget *
_create_icon(Eo *parent, const char *name, const char *command, const char *icon)
{
   Eo *box = efl_add(EFL_UI_BOX_CLASS, parent);
   // Icon
   efl_add(EFL_UI_IMAGE_CLASS, box,
           efl_ui_image_icon_set(efl_added, icon),
           efl_gfx_hint_weight_set(efl_added, 1.0, 1.0),
           efl_gfx_hint_size_min_set(efl_added, EINA_SIZE2D(64, 64)),
           efl_event_callback_add(efl_added, EFL_INPUT_EVENT_CLICKED, _icon_clicked_cb, command),
           efl_event_callback_add(efl_added, EFL_EVENT_DEL, _icon_deleted_cb, command),
           efl_pack(box, efl_added));

   // Label
   efl_add(EFL_UI_TEXTBOX_CLASS, box,
           efl_text_set(efl_added, name),
           efl_text_multiline_set(efl_added, EINA_TRUE),
           efl_canvas_textblock_style_apply(efl_added,
              "effect_type=soft_shadow shadow_color=#444 wrap=word font_size=10 align=center ellipsis=1"),
           efl_gfx_hint_size_min_set(efl_added, EINA_SIZE2D(0, 40)),
           efl_text_interactive_editable_set(efl_added, EINA_FALSE),
           efl_text_interactive_selection_allowed_set(efl_added, EINA_FALSE),
           efl_pack(box, efl_added));

   return box;
}

// Creates a widget "button" for the specified .desktop file.
// These buttons are actually just an image with a label below.
static Efl_Ui_Widget *
_create_app_icon(Eo *parent, const char *desktop_file_path)
{
   char *name = NULL, *command = NULL, *icon = NULL;
   Eo *widget = NULL;

   if (!_parse_desktop_file(desktop_file_path, &name, &command, &icon))
     return NULL;

   widget = _create_icon(parent, name, command, icon);

   free(name);
   free(icon);
   return widget;
}

// Adds a new empty page to the homescreen
static void
_add_page(Build_Data *bdata)
{
   bdata->table = efl_add(EFL_UI_TABLE_CLASS, bdata->over_container,
                          efl_pack_table_size_set(efl_added, TABLE_COLUMNS, TABLE_ROWS));
   efl_pack_end(bdata->over_container, bdata->table);
   bdata->x = bdata->y = 0;
}

// Adds all files in the array to the homescreen, adding pages as they become full.
static void
_app_found(void *data, Eina_Array *files)
{
   unsigned int i;
   const char *item;
   Eina_Array_Iterator iterator;
   Build_Data *bdata = data;

   EINA_ARRAY_ITER_NEXT(files, i, item, iterator)
     {
        Eo *app = _create_app_icon(bdata->over_container, item);
        if (app)
          {
             if (!bdata->table)
               _add_page(bdata);
             efl_pack_table(bdata->table, app, bdata->x, bdata->y, 1, 1);
             bdata->x++;
             if (bdata->x == TABLE_COLUMNS)
               {
                  bdata->x = 0;
                  bdata->y++;
                  if (bdata->y == TABLE_ROWS)
                    bdata->table = NULL;
               }
          }
     }
}

// Called when directory listing has finished
static Eina_Value
_file_listing_done_cb (void *data, const Eina_Value file, const Eina_Future *dead EINA_UNUSED)
{
   Build_Data *bdata = data;
   // Fill any remaining empty cells with invisible rectangles so the rest of the cells
   // keep the same size as other pages
   while (bdata->y < TABLE_ROWS)
     {
        while (bdata->x < TABLE_COLUMNS)
          {
             efl_add(EFL_CANVAS_RECTANGLE_CLASS, bdata->table,
                     efl_gfx_color_set(efl_added, 0, 0, 0, 0),
                     efl_pack_table(bdata->table, efl_added, bdata->x, bdata->y, 1, 1));
             bdata->x++;
          }
        bdata->x = 0;
        bdata->y++;
     }
   return file;
}

// Create Spotlight widget and start populating it with user apps.
static void
_build_homescreen(Efl_Ui_Win *win, Build_Data *bdata)
{
   Efl_Ui_Spotlight_Indicator *indicator = efl_new(EFL_UI_SPOTLIGHT_ICON_INDICATOR_CLASS);
   Efl_Ui_Spotlight_Manager *scroll = efl_new(EFL_UI_SPOTLIGHT_SCROLL_MANAGER_CLASS);
   bdata->over_container = efl_add(EFL_UI_SPOTLIGHT_CONTAINER_CLASS, win,
                                   efl_ui_spotlight_manager_set(efl_added, scroll),
                                   efl_ui_spotlight_indicator_set(efl_added, indicator)
   );
   bdata->table = NULL;

   Eina_Future *future = efl_io_manager_ls(bdata->io_manager, "/usr/local/share/applications", bdata, _app_found, NULL);
   eina_future_then(future, _file_listing_done_cb, bdata, NULL);
}

// The main box, with an upper space for the apps list and a lower space
// for the quick-action buttons.
static Efl_Ui_Widget*
_build_overall_structure(Efl_Ui_Win *win, Efl_Ui_Widget *homescreen)
{
   Efl_Ui_Widget *box, *start_line;

   box = efl_add(EFL_UI_BOX_CLASS, win);

   // Set box background
   // Objects retrieved with efl_part() only survive one function call, so we ref it
   Eo *background = efl_ref(efl_part(box, "background"));
   efl_file_key_set(background, "e/desktop/background");
   efl_file_set(background, "../src/Hills.edj");
   efl_file_load(background);
   efl_unref(background);

   efl_pack_end(box, homescreen);

   // Start line
   start_line = efl_add(EFL_UI_BOX_CLASS, win,
                        efl_gfx_color_set(efl_part(efl_added, "background"), 128, 128, 128, 128));
   efl_gfx_hint_weight_set(start_line, 1.0, 0.0);
   efl_ui_layout_orientation_set(start_line, EFL_UI_LAYOUT_ORIENTATION_HORIZONTAL);
   efl_pack_end(box, start_line);

   for (unsigned int i = 0; i < sizeof(launcher_apps)/sizeof(launcher_apps[0]); ++i)
     {
        efl_pack_end(start_line, _create_icon(start_line,
                                              launcher_apps[i][0],
                                              strdup(launcher_apps[i][1]),
                                              launcher_apps[i][2]));
     }

   return box;
}

static void
_child_removed_cb(void *data, const Efl_Event *ev EINA_UNUSED)
{
   Build_Data *bdata = data;
   printf("REMOVE REMOVE REMOVE\n");
}

static void
_child_added_cb(void *data, const Efl_Event *ev EINA_UNUSED)
{
   Build_Data *bdata = data;
   _show_wl(bdata);
   efl_event_callback_add(ev->info, EFL_EVENT_DEL, _child_removed_cb, data);
}


static void
_build_compositor(Efl_Ui_Win *win, Build_Data *bdata)
{
   bdata->compositor = efl_add(EFL_UI_STACK_CLASS, win);
   bdata->wl = efl_add(EFL_CANVAS_WL_CLASS, evas_object_evas_get(win));
   efl_event_callback_add(bdata->wl, EFL_CANVAS_WL_EVENT_TOPLEVEL_ADDED, _child_added_cb, bdata);
}

// Called when the app is closed
static void
_gui_quit_cb(void *data, const Efl_Event *event EINA_UNUSED)
{
   Build_Data *bdata = data;
   if (bdata->io_manager)
      efl_del(bdata->io_manager);
   free(bdata);
   efl_exit(0);
}

EAPI_MAIN void
efl_main(void *data EINA_UNUSED, const Efl_Event *ev EINA_UNUSED)
{
   Eo *win, *desktop;
   Build_Data *bdata = calloc (1, sizeof(Build_Data));

   current = bdata;

   bdata->io_manager = efl_add(EFL_IO_MANAGER_CLASS, efl_main_loop_get());

   win = efl_add(EFL_UI_WIN_CLASS, efl_main_loop_get(),
                 efl_ui_win_autodel_set(efl_added, EINA_TRUE));
   // when the user clicks "close" on a window there is a request to delete
   efl_event_callback_add(win, EFL_UI_WIN_EVENT_DELETE_REQUEST, _gui_quit_cb, bdata);

   _build_homescreen(win, bdata);
   desktop = _build_overall_structure(win, bdata->over_container);
   _build_compositor(win, bdata);
   efl_pack_end(bdata->compositor, desktop);
   efl_content_set(win, bdata->compositor);
}
EFL_MAIN()
