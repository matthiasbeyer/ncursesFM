#include "../inc/mount.h"

#ifdef SYSTEMD_PRESENT
static void mount_fs(const char *str, const char *method, int mount);

static char mount_str[PATH_MAX + 1];

#ifdef LIBUDEV_PRESENT
static void cleanup_f(void *x);
static int init_mbus(void);
static int check_udisks(void);
static void enumerate_block_devices(void);
static int is_mountable(const char *dev_path);
static int is_mounted(const char *dev_path);
static int add_callback(sd_bus_message *m, void *userdata, sd_bus_error *ret_error);
static int remove_callback(sd_bus_message *m, void *userdata, sd_bus_error *ret_error);
static int change_callback(sd_bus_message *m, void *userdata, sd_bus_error *ret_error);
static void *device_monitor(void *x);
static void change_mounted_status(int pos, int mount, const char *name);
static int add_device(struct udev_device *dev, const char *name);
static int remove_device(const char *name);
static int is_present(const char *name);
static void *safe_realloc(const size_t size);
static void free_devices(void);

static struct udev *udev;
static int number_of_devices;
char (*my_devices)[PATH_MAX + 1];
static pthread_mutex_t dev_lock;
static sd_bus *mbus;
#endif
#endif

#ifdef SYSTEMD_PRESENT
/*
 * Open the bus, and call "method" string on UDisks2.Filesystem.
 */
static void mount_fs(const char *str, const char *method, int mount) {
    sd_bus_error error = SD_BUS_ERROR_NULL;
    sd_bus_message *mess = NULL;
    sd_bus *mount_bus = NULL;
    const char *path;
    char obj_path[PATH_MAX + 1] = "/org/freedesktop/UDisks2/block_devices/";
    char tmp[30];
    int r;

    r = sd_bus_open_system(&mount_bus);
    if (r < 0) {
        print_info(strerror(-r), ERR_LINE);
        WARN(strerror(-r));
        return;
    }
    strcat(obj_path, strrchr(str, '/') + 1);
    sprintf(tmp, "calling %s on bus.", method);
    INFO(tmp);
    r = sd_bus_call_method(mount_bus,
                           "org.freedesktop.UDisks2",
                           obj_path,
                           "org.freedesktop.UDisks2.Filesystem",
                           method,
                           &error,
                           &mess,
                           "a{sv}",
                           NULL);
    if (r < 0) {
        print_info(error.message, ERR_LINE);
        WARN(error.message);
    } else {
        if (!mount) {
            sd_bus_message_read(mess, "s", &path);
            sprintf(mount_str, "%s mounted in: %s.", str, path);
        } else {
            sprintf(mount_str, "%s unmounted.", str);
        }
#ifndef LIBUDEV_PRESENT
        print_info(mount_str, INFO_LINE);
#endif
    }
    close_bus(&error, mess, mount_bus);
}

/*
 * Open the bus and setup the loop device,
 * then set "SetAutoclear" option; this way the loop device
 * will be automagically cleared when no more in use.
 */
void isomount(const char *str) {
    sd_bus_error error = SD_BUS_ERROR_NULL;
    sd_bus_message *mess = NULL;
    sd_bus *iso_bus = NULL;
    const char *obj_path;
    int r, fd;
    
    fd = open(str, O_RDONLY);
    r = sd_bus_open_system(&iso_bus);
    if (r < 0) {
        print_info(strerror(-r), ERR_LINE);
        WARN(strerror(-r));
        return;
    }
    INFO("calling LoopSetup method on bus.");
    r = sd_bus_call_method(iso_bus,
                           "org.freedesktop.UDisks2",
                           "/org/freedesktop/UDisks2/Manager",
                           "org.freedesktop.UDisks2.Manager",
                           "LoopSetup",
                           &error,
                           &mess,
                           "ha{sv}",
                           fd,
                           NULL);
    close(fd);
    if (r < 0) {
        print_info(error.message, ERR_LINE);
        WARN(error.message);
    } else {
        sd_bus_message_read(mess, "o", &obj_path);
        sd_bus_flush(iso_bus);
#ifndef LIBUDEV_PRESENT
        mount_fs(obj_path, "Mount", 0);
#endif
        INFO("calling SetAutoClear on bus.");
        r = sd_bus_call_method(iso_bus,
                               "org.freedesktop.UDisks2",
                               obj_path,
                               "org.freedesktop.UDisks2.Loop",
                               "SetAutoclear",
                               &error,
                               NULL,
                               "ba{sv}",
                               TRUE,
                               NULL);
        if (r < 0) {
            print_info(error.message, ERR_LINE);
            WARN(error.message);
        }
    }
    close_bus(&error, mess, iso_bus);
}

#ifdef LIBUDEV_PRESENT
void start_monitor(void) {
    int fail = 0;
    
    udev = udev_new();
    if (!udev) {
        WARN("could not create udev.");
        fail = 1;
    }
    if (!fail) {
        fail = init_mbus();
    }
    if (fail) {
        if (udev) {
            udev_unref(udev);
        }
        device_mode = DEVMON_OFF;
    } else {
        INFO("started device monitor th.");
        pthread_create(&monitor_th, NULL, device_monitor, NULL);
    }
}

static int init_mbus(void) {
    int r;
    
    r = sd_bus_open_system(&mbus);
    if (r < 0) {
        WARN(strerror(-r));
        return r;
    }
    r = check_udisks();
    if (r < 0) {
        WARN("UDisks2 not present. Disabling udisks2 monitor.");
    } else {
        r = sd_bus_add_match(mbus, NULL, 
                         "type='signal',"
                         "sender='org.freedesktop.UDisks2',"
                         "interface='org.freedesktop.DBus.ObjectManager',"
                         "member='InterfacesAdded'",
                         add_callback, NULL);
        if (r < 0) {
            WARN(strerror(-r));
        } else {
            r = sd_bus_add_match(mbus, NULL, 
                         "type='signal',"
                         "sender='org.freedesktop.UDisks2',"
                         "interface='org.freedesktop.DBus.ObjectManager',"
                         "member='InterfacesRemoved'",
                         remove_callback, NULL);
            if (r < 0) {
                WARN(strerror(-r));
            } else {
                r = sd_bus_add_match(mbus, NULL, 
                                 "type='signal',"
                                 "sender='org.freedesktop.UDisks2',"
                                 "interface='org.freedesktop.DBus.Properties',"
                                 "member='PropertiesChanged'",
                                 change_callback, NULL);
                if (r < 0) {
                    WARN(strerror(-r));
                }
            }
        }
    }
    if (r < 0) {
        sd_bus_flush_close_unref(mbus);
        return r;
    } 
    return 0;
}

static int check_udisks(void) {
    INFO("Checking for udisks2");
    return sd_bus_call_method(mbus,
                            "org.freedesktop.UDisks2",
                            "/org/freedesktop/UDisks2",
                            "org.freedesktop.DBus.Peer",
                            "Ping",
                            NULL,
                            NULL,
                            NULL);
}

void show_devices_tab(void) {
    if (number_of_devices) {
        pthread_mutex_lock(&fm_lock[active]);
        ps[active].number_of_files = number_of_devices;
        str_ptr[active] = my_devices;
        device_mode = 1 + active;
        sprintf(ps[active].title, device_mode_str);
        reset_win(active);
        pthread_mutex_unlock(&fm_lock[active]);
    } else {
        if (device_mode == DEVMON_STARTING) {
            print_info("Still polling udev for devices.", INFO_LINE);
        } else {
            print_info("No devices found.", INFO_LINE);
        }
    }
}

/*
 * Scan "block" subsystem for devices.
 * For each device check if it is a really mountable external fs,
 * add it to my_devices{} and increment number_of_devices.
 */
static void enumerate_block_devices(void) {
    struct udev_enumerate *enumerate;
    struct udev_list_entry *devices, *dev_list_entry;
    struct udev_device *dev;

    enumerate = udev_enumerate_new(udev);
    udev_enumerate_add_match_subsystem(enumerate, "block");
    udev_enumerate_scan_devices(enumerate);
    devices = udev_enumerate_get_list_entry(enumerate);
    udev_list_entry_foreach(dev_list_entry, devices) {
        const char *path = udev_list_entry_get_name(dev_list_entry);
        dev = udev_device_new_from_syspath(udev, path);
        const char *name = udev_device_get_devnode(dev);
        int r = add_device(dev, name);
        udev_device_unref(dev);
        if (quit) {
            break;
        } else if (r && device_mode) {
            update_devices(number_of_devices, my_devices);
        }
    }
    udev_enumerate_unref(enumerate);
}

static int is_mountable(const char *dev_path) {
    blkid_cache cache = NULL;
    int ret;
    
    if (blkid_get_cache(&cache, NULL) < 0) {
        WARN("No libblkid cache file.");
    }
    blkid_gc_cache(cache);
    /* Only call probe_all_removable for CDs -> otherwise it will slow down everything */
    if (!strncmp(dev_path, "/dev/sr", strlen("/dev/sr"))) {
        blkid_probe_all_removable(cache);
    }
    blkid_dev dev = blkid_get_dev(cache, dev_path, 0);
    dev ? (ret = 0) : (ret = -1);
    blkid_put_cache(cache);
    return ret;
}

static int is_mounted(const char *dev_path) {
    FILE *mtab = NULL;
    struct mntent *part = NULL;
    int ret = 0;
    
    if ((mtab = setmntent("/etc/mtab", "r"))) {
        while ((part = getmntent(mtab))) {
            if ((part->mnt_fsname) && (!strcmp(part->mnt_fsname, dev_path))) {
                ret = 1;
                break;
            }
        }
        endmntent(mtab);
    }
    return ret;
}

static int add_callback(sd_bus_message *m, void *userdata, sd_bus_error *ret_error) {
    int r;
    const char *path;
    const char *obj = "/org/freedesktop/UDisks2/block_devices/";
    char devname[PATH_MAX + 1];
    char name[NAME_MAX + 1];
    struct udev_device *dev = NULL;
    
    r = sd_bus_message_read(m, "o", &path);
    if (r < 0) {
        WARN(strerror(-r));
    } else {
        if (!strncmp(obj, path, strlen(obj))) {
            INFO("InterfaceAdded signal received!");
            pthread_mutex_lock(&dev_lock);
            strcpy(name, strrchr(path, '/') + 1);
            dev = udev_device_new_from_subsystem_sysname(udev, "block", name);
            if (dev) {
                sprintf(devname, "/dev/%s", name);
                r = add_device(dev, devname);
                udev_device_unref(dev);
                if (!quit && device_mode && r != -1) {
                    print_info("New device connected.", INFO_LINE);
                    update_devices(number_of_devices, my_devices);
                }
            }
            pthread_mutex_unlock(&dev_lock);
        } else {
            INFO("signal discarded.");
        }
    }
    return 0;
}

static int remove_callback(sd_bus_message *m, void *userdata, sd_bus_error *ret_error) {
    int r;
    const char *path;
    const char *obj = "/org/freedesktop/UDisks2/block_devices/";
    char devname[PATH_MAX + 1];
    char name[NAME_MAX + 1];
    
    r = sd_bus_message_read(m, "o", &path);
    if (r < 0) {
        WARN(strerror(-r));
    } else {
        if (!strncmp(obj, path, strlen(obj))) {
            INFO("InterfaceRemoved signal received!");
            pthread_mutex_lock(&dev_lock);
            strcpy(name, strrchr(path, '/') + 1);
            sprintf(devname, "/dev/%s", name);
            r = remove_device(devname);
            if (!quit && device_mode && r != -1) {
                update_devices(number_of_devices, my_devices);
            }
            pthread_mutex_unlock(&dev_lock);
        } else {
            INFO("signal discarded.");
        }
    }
    return 0;
}

static int change_callback(sd_bus_message *m, void *userdata, sd_bus_error *ret_error) {
    int r;
    const char *path;
    const char *obj = "org.freedesktop.UDisks2.Filesystem";
    char devname[PATH_MAX + 1];
    
    r = sd_bus_message_read(m, "s", &path);
    if (r < 0) {
        WARN(strerror(-r));
    } else {
        if (!strncmp(obj, path, strlen(obj))) {
            INFO("PropertiesChanged signal received!");
            pthread_mutex_lock(&dev_lock);
            const char *name = sd_bus_message_get_path(m);
            sprintf(devname, "/dev/%s", strrchr(name, '/') + 1);
            int present = is_present(devname);
            if (present != -1) {
                int mount = is_mounted(devname);
                change_mounted_status(present, mount, devname);
                if (!quit && device_mode) {
                    update_devices(number_of_devices, NULL);
                }
            }
            pthread_mutex_unlock(&dev_lock);
        }
    }
    return 0;
}

void manage_enter_device(void) {
    int mount;
    int pos = ps[active].curr_pos;
    int len = strlen(my_devices[pos]);
    char *ptr = strchr(my_devices[pos], ',');
    char name[PATH_MAX + 1], action[10];
    
    pthread_mutex_lock(&dev_lock);
    mount = my_devices[pos][len - 1] - '0';
    strcpy(name, my_devices[pos]);
    name[len - strlen(ptr)] = '\0';
    if (mount) {
        strcpy(action, "Unmount");
    } else {
        strcpy(action, "Mount");
    }
    mount_fs(name, action, mount);
    pthread_mutex_unlock(&dev_lock);
}

static void change_mounted_status(int pos, int mount, const char *name) {
    int len = strlen(my_devices[pos]);
    sprintf(my_devices[pos] + len - 1, "%d", mount);
    if (!strlen(mount_str)) {
        if (mount == 1) {
            sprintf(mount_str, "External tool has mounted %s.", name);
        } else {
            sprintf(mount_str, "External tool has unmounted %s.", name);
        }
    }
    if (device_mode != DEVMON_STARTING) {
        print_info(mount_str, INFO_LINE);
    }
    memset(mount_str, 0, strlen(mount_str));
}

void leave_device_mode(void) {
    pthread_mutex_lock(&fm_lock[active]);
    device_mode = 0;
    pthread_mutex_unlock(&fm_lock[active]);
    change_dir(ps[active].my_cwd);
}

static void *device_monitor(void *x) {
    int r;
    
    pthread_cleanup_push(cleanup_f, NULL);
    enumerate_block_devices();
    if (!quit) {
        pthread_mutex_init(&dev_lock, NULL);
        device_mode = DEVMON_READY;
    } else {
        device_mode = DEVMON_OFF;
    }
    while (!quit) {
        r = sd_bus_process(mbus, NULL);
        if (r > 0) {
            continue;
        }
        r = sd_bus_wait(mbus, (uint64_t) -1);
        if (r < 0) {
            break;
        }
    }
    pthread_cleanup_pop(1);
    pthread_detach(pthread_self());
    pthread_exit(NULL);
}

static void cleanup_f(void *x) {
    INFO("canceling thread...");
    sd_bus_flush_close_unref(mbus);
    pthread_mutex_destroy(&dev_lock);
    free_devices();
    udev_unref(udev);
}

static int add_device(struct udev_device *dev, const char *name) {
    long size;
    char s[20];
    struct udev_device *parent;
    char removable[2] = "\0";
    int mount = is_mountable(name);
    
    if (mount != -1) {
        mount = is_mounted(name);
        my_devices = safe_realloc((PATH_MAX + 1) * (number_of_devices + 1));
        if (!quit) {
            /* calculates current device size */
            size = strtol(udev_device_get_sysattr_value(dev, "size"), NULL, 10);
            size = (size / (long) 2) * 1024;
            change_unit((float)size, s);
            if (udev_device_get_property_value(dev, "ID_MODEL")) {
                sprintf(my_devices[number_of_devices], "%s, %s, Size: %s, Mounted: %d", 
                        name, udev_device_get_property_value(dev, "ID_MODEL"), s, mount);
            } else {
                sprintf(my_devices[number_of_devices], "%s, Size: %s, Mounted: %d", 
                        name, s, mount);
            }
            number_of_devices++;
            INFO("added device.");
            if (udev_device_get_sysattr_value(dev, "removable")) {
                strcpy(removable, udev_device_get_sysattr_value(dev, "removable"));
            } else {
                parent = udev_device_get_parent(dev);
                if (parent && udev_device_get_sysattr_value(parent, "removable")) {
                    strcpy(removable, udev_device_get_sysattr_value(parent, "removable"));
                }
            }
            int is_loop_dev = !strncmp(name, "/dev/loop", strlen("/dev/loop"));
            if (!mount && ((config.automount && !strcmp(removable, "1")) || is_loop_dev)) {
                mount_fs(name, "Mount", mount);
            }
        }
    } else {
        INFO("not mountable fs found. Device discarded.");
    }
    return mount;
}

static int remove_device(const char *name) {
    int i = is_present(name);
    
    if (i != -1) {
        while (i < number_of_devices - 1) {
            strcpy(my_devices[i], my_devices[i + 1]);
            i++;
        }
        my_devices = safe_realloc(PATH_MAX * (number_of_devices - 1));
        if (!quit) {
            number_of_devices--;
            print_info("Device removed.", INFO_LINE);
            INFO("removed device.");
        }
    }
    return i;
}

static int is_present(const char *name) {
    for (int i = 0; i < number_of_devices; i++) {
        if (strncmp(my_devices[i], name, strlen(name)) == 0) {
            return i;
        }
    }
    return -1;
}

static void *safe_realloc(const size_t size) {
    void *tmp = realloc(my_devices, size);
    
    if (!tmp) {
        quit = MEM_ERR_QUIT;
        ERROR("could not realloc. Leaving monitor th.");
        return my_devices;
    }
    return tmp;
}

static void free_devices(void) {
    INFO("freeing resources...");
    if (my_devices) {
        free(my_devices);
    }
    INFO("freed.");
}

#endif
#endif