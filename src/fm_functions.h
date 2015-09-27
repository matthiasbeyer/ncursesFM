#define _GNU_SOURCE
#include <ftw.h>
#include "ui_functions.h"
#include <sys/wait.h>
#include <sys/file.h>
#include <archive.h>
#include <archive_entry.h>
#ifdef LIBX11_PRESENT
#include <X11/Xlib.h>
#endif
#ifdef LIBCUPS_PRESENT
#include <cups/cups.h>
#endif

#define BUFF_SIZE 8192
#define FAST_BROWSE_THRESHOLD 500

void change_dir(const char *str);
void switch_hidden(void);
void manage_file(const char *str);
int new_file(void);
int remove_file(void);
void manage_space_press(const char *str);
int paste_file(void);
int move_file(void);
int rename_file_folders(void);
void search(void);
void list_found(void);
int search_enter_press(const char *str);
void leave_search_mode(const char *str);
#ifdef LIBCUPS_PRESENT
void print_support(char *str);
#endif
int create_archive(void);
void change_tab(void);
#if defined(LIBUDEV_PRESENT) && (SYSTEMD_PRESENT)
void devices_tab(void);
void leave_device_mode(void);
void manage_enter_device(void);
#endif
void fast_browse(int c);