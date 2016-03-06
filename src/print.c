#ifdef LIBCUPS_PRESENT

#include "../inc/print.h"

static void print_file(const char *filename);

void print_support(const char *str) {
    char c;

    ask_user(print_question, &c, 1, 'y');
    if (!quit && c == 'y') {
        print_file(str);
    }
}

/*
 * If there are cups printers available, take the default printer and print the file.
 */
static void print_file(const char *filename) {
    cups_dest_t *dests, *default_dest;
    int num_dests = cupsGetDests(&dests);
    int r;

    if (num_dests > 0) {
        default_dest = cupsGetDest(NULL, NULL, num_dests, dests);
        r = cupsPrintFile(default_dest->name, filename, "ncursesFM job",
                      default_dest->num_options, default_dest->options);
        if (r) {
            print_info(print_ok, INFO_LINE);
        } else {
            print_info(ippErrorString(cupsLastError()), ERR_LINE);
        }
    } else {
        print_info(print_fail, ERR_LINE);
    }
}

#endif
