/* USB teleport, client side — see usbexport.h. Pure sysfs + fork/exec
 * (modprobe, usbipd), no library. Runs as root like the rest of the
 * client (framebuffer + console ioctls). */
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "usbexport.h"

#define USB_DEVICES   "/sys/bus/usb/devices"
#define USBIP_DRIVER  "/sys/bus/usb/drivers/usbip-host"
#define USB_DRIVER    "/sys/bus/usb/drivers/usb"
#define USBIPD_PORT   3240
#define MAX_IDS       32
#define MAX_BOUND     16
#define G_N(a)        ((int)(sizeof(a) / sizeof((a)[0])))

struct usb_id {
    unsigned vendor;
    int product;               /* -1 = any product of this vendor */
};

static struct {
    int active;
    struct usb_id allow[MAX_IDS], deny[MAX_IDS];
    int n_allow, n_deny;
    char bound[MAX_BOUND][64]; /* busids we bound (for cleanup) */
    int n_bound;
} U;

static void ulog(const char *fmt, ...)
{
    va_list ap;
    fprintf(stderr, "sremfb-client: usb: ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

/* ------------------------------------------------------ tiny helpers */

static int read_sysfs(const char *path, char *out, size_t len)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    ssize_t n;

    if (fd < 0)
        return -1;
    n = read(fd, out, len - 1);
    close(fd);
    if (n < 0)
        return -1;
    while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == ' '))
        n--;
    out[n] = '\0';
    return 0;
}

static int write_sysfs(const char *path, const char *val)
{
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    ssize_t n;

    if (fd < 0)
        return -1;
    n = write(fd, val, strlen(val));
    close(fd);
    return n == (ssize_t)strlen(val) ? 0 : -1;
}

static int run_cmd(const char *arg0, ...)
{
    const char *argv[8];
    int argc = 0, status = 0;
    va_list ap;

    argv[argc++] = arg0;
    va_start(ap, arg0);
    while (argc < 7 && (argv[argc] = va_arg(ap, const char *)))
        argc++;
    va_end(ap);
    argv[argc] = NULL;

    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        int null = open("/dev/null", O_RDWR);
        if (null >= 0) {
            dup2(null, 0);
            dup2(null, 1);
        }
        execvp(argv[0], (char *const *)argv);
        _exit(127);
    }
    if (waitpid(pid, &status, 0) < 0)
        return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/* "1-1.2" style device directory (an actual device: no ':', no "usbN") */
static int is_busid(const char *name)
{
    int dash = 0;
    for (const char *p = name; *p; p++) {
        if (*p == '-')
            dash++;
        else if (*p != '.' && !isdigit((unsigned char)*p))
            return 0;
    }
    return dash == 1;
}

/* --------------------------------------------------------- id lists */

static void parse_ids(const char *env, struct usb_id *out, int *n)
{
    const char *s = getenv(env);
    char buf[512], *tok, *save = NULL;

    *n = 0;
    if (!s || !*s)
        return;
    snprintf(buf, sizeof(buf), "%s", s);
    for (tok = strtok_r(buf, ", \t", &save); tok && *n < MAX_IDS;
         tok = strtok_r(NULL, ", \t", &save)) {
        unsigned v, p;
        if (sscanf(tok, "%x:%x", &v, &p) == 2) {
            out[*n].vendor = v;
            out[*n].product = (int)p;
        } else if (sscanf(tok, "%x", &v) == 1) {
            out[*n].vendor = v;
            out[*n].product = -1;
        } else {
            ulog("%s: bad id \"%s\" (want vendor[:product] hex)", env, tok);
            continue;
        }
        (*n)++;
    }
}

static int id_match(const struct usb_id *ids, int n, unsigned v, unsigned p)
{
    for (int i = 0; i < n; i++)
        if (ids[i].vendor == v &&
            (ids[i].product < 0 || (unsigned)ids[i].product == p))
            return 1;
    return 0;
}

/* Hard-wired blacklist, stronger than SREMFB_USB_ALLOW: the USB Ethernet
 * chips soldered on Raspberry Pi boards. Exporting one kills the link
 * mid-flight — the dynamic NIC guard already refuses them while the
 * interface is up, this catches them even when it isn't. */
static const struct usb_id builtin_deny[] = {
    { 0x0424, 0xec00 },        /* SMSC LAN9512/9514 (Pi B/2/3B) */
    { 0x0424, 0x7800 },        /* Microchip LAN7515/7800 (Pi 3B+) */
};

static int builtin_denied(unsigned v, unsigned p)
{
    return id_match(builtin_deny, G_N(builtin_deny), v, p);
}

/* -------------------------------------------------------- the guards */

/* True if the real path of `link` lives under the USB device directory
 * of `busid` — i.e. the thing behind `link` hangs off that device. */
static int path_under_device(const char *link, const char *busid)
{
    char real[PATH_MAX], needle[64];

    if (!realpath(link, real))
        return 0;
    snprintf(needle, sizeof(needle), "/%s/", busid);
    if (strstr(real, needle))
        return 1;
    snprintf(needle, sizeof(needle), "/%s", busid);
    size_t rl = strlen(real), nl = strlen(needle);
    return rl >= nl && strcmp(real + rl - nl, needle) == 0;
}

/* An *active* network interface hangs off this device (a Pi's Ethernet
 * is USB): never touch it, whatever the lists say. */
static int guards_nic(const char *busid)
{
    DIR *dir = opendir("/sys/class/net");
    struct dirent *de;
    int hit = 0;

    if (!dir)
        return 0;
    while (!hit && (de = readdir(dir))) {
        char path[288], state[16] = "";
        if (de->d_name[0] == '.' || strcmp(de->d_name, "lo") == 0)
            continue;
        snprintf(path, sizeof(path), "/sys/class/net/%s/operstate",
                 de->d_name);
        if (read_sysfs(path, state, sizeof(state)) < 0 ||
            strcmp(state, "down") == 0)
            continue;
        snprintf(path, sizeof(path), "/sys/class/net/%s/device",
                 de->d_name);
        hit = path_under_device(path, busid);
    }
    closedir(dir);
    return hit;
}

/* A mounted filesystem or active swap sits on a block device that hangs
 * off this USB device (USB-boot SBC, auto-mounted stick): never touch. */
static int guards_mounted(const char *busid)
{
    DIR *dir = opendir("/sys/block");
    struct dirent *de;
    int hit = 0;

    if (!dir)
        return 0;
    while (!hit && (de = readdir(dir))) {
        char path[288];
        if (de->d_name[0] == '.')
            continue;
        snprintf(path, sizeof(path), "/sys/block/%s/device", de->d_name);
        if (!path_under_device(path, busid))
            continue;
        /* this whole disk is on the device: is any of it in use? */
        char disk[48];
        strncpy(disk, de->d_name, sizeof(disk) - 1);
        disk[sizeof(disk) - 1] = '\0';
        for (int i = 0; i < 2 && !hit; i++) {
            FILE *f = fopen(i ? "/proc/swaps" : "/proc/mounts", "r");
            char line[512], dev[64];
            if (!f)
                continue;
            snprintf(dev, sizeof(dev), "/dev/%s", disk);
            while (!hit && fgets(line, sizeof(line), f))
                hit = strncmp(line, dev, strlen(dev)) == 0;
            fclose(f);
        }
    }
    closedir(dir);
    return hit;
}

/* ------------------------------------------------------ class policy */

/* Interface drivers that make a device eligible by default: input,
 * storage, serial. Vendor-class serial bridges (FTDI…) are caught by
 * their driver name, not bInterfaceClass. */
static const char *const eligible_drivers[] = {
    "usbhid", "usb-storage", "uas", "cdc_acm",
    "ftdi_sio", "cp210x", "pl2303", "ch341", NULL,
};

static int class_eligible(unsigned cls)
{
    return cls == 0x03 /* HID */ || cls == 0x08 /* mass storage */ ||
           cls == 0x02 || cls == 0x0a /* CDC comms/data (serial) */;
}

static int device_eligible(const char *busid)
{
    char pattern[80], path[192], ifname[80], buf[64];
    DIR *dir = opendir(USB_DEVICES);
    struct dirent *de;
    int ok = 0;

    if (!dir)
        return 0;
    snprintf(pattern, sizeof(pattern), "%s:", busid);
    while (!ok && (de = readdir(dir))) {
        if (strncmp(de->d_name, pattern, strlen(pattern)) != 0)
            continue;                  /* not an interface of this device */
        strncpy(ifname, de->d_name, sizeof(ifname) - 1);
        ifname[sizeof(ifname) - 1] = '\0';
        snprintf(path, sizeof(path), USB_DEVICES "/%s/bInterfaceClass",
                 ifname);
        if (read_sysfs(path, buf, sizeof(buf)) == 0 &&
            class_eligible((unsigned)strtoul(buf, NULL, 16))) {
            ok = 1;
            break;
        }
        snprintf(path, sizeof(path), USB_DEVICES "/%s/driver", ifname);
        ssize_t n = readlink(path, buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = '\0';
            const char *drv = strrchr(buf, '/');
            drv = drv ? drv + 1 : buf;
            for (int i = 0; eligible_drivers[i]; i++)
                if (strcmp(drv, eligible_drivers[i]) == 0) {
                    ok = 1;
                    break;
                }
        }
    }
    closedir(dir);
    return ok;
}

/* ------------------------------------------------------- bind/unbind */

static int device_bound_to_usbip(const char *busid)
{
    char path[320];

    snprintf(path, sizeof(path), USBIP_DRIVER "/%s", busid);
    return access(path, F_OK) == 0;
}

static int bind_device(const char *busid)
{
    char val[96];

    snprintf(val, sizeof(val), "add %s", busid);
    if (write_sysfs(USBIP_DRIVER "/match_busid", val) < 0)
        return -1;
    /* detach from the generic driver, attach to usbip-host */
    write_sysfs(USB_DRIVER "/unbind", busid);   /* may already be free */
    if (write_sysfs(USBIP_DRIVER "/bind", busid) < 0) {
        snprintf(val, sizeof(val), "del %s", busid);
        write_sysfs(USBIP_DRIVER "/match_busid", val);
        write_sysfs(USB_DRIVER "/bind", busid);
        return -1;
    }
    return 0;
}

static void unbind_device(const char *busid)
{
    char val[96];

    write_sysfs(USBIP_DRIVER "/unbind", busid);
    snprintf(val, sizeof(val), "del %s", busid);
    write_sysfs(USBIP_DRIVER "/match_busid", val);
    write_sysfs(USB_DRIVER "/bind", busid);
}

/* --------------------------------------------------------- usbipd */

static int usbipd_listening(void)
{
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct sockaddr_in a = {
        .sin_family = AF_INET,
        .sin_port = htons(USBIPD_PORT),
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
    };
    int ok;

    if (fd < 0)
        return 0;
    ok = connect(fd, (struct sockaddr *)&a, sizeof(a)) == 0;
    close(fd);
    return ok;
}

static int ensure_usbipd(void)
{
    if (usbipd_listening())
        return 0;
    if (run_cmd("usbipd", "-D", NULL) != 0 &&
        run_cmd("/usr/sbin/usbipd", "-D", NULL) != 0) {
        ulog("cannot start usbipd (package \"usbip\" installed?)");
        return -1;
    }
    for (int i = 0; i < 20; i++) {
        if (usbipd_listening())
            return 0;
        usleep(100 * 1000);
    }
    ulog("usbipd started but not listening on %d", USBIPD_PORT);
    return -1;
}

/* ----------------------------------------------------------- driver */

static void scan_and_bind(void)
{
    DIR *dir = opendir(USB_DEVICES);
    struct dirent *de;

    if (!dir)
        return;
    while ((de = readdir(dir))) {
        char busid[64], path[192], vbuf[16], pbuf[16], cbuf[16];
        char name[64] = "";
        unsigned vend, prod;

        if (!is_busid(de->d_name))
            continue;
        strncpy(busid, de->d_name, sizeof(busid) - 1);
        busid[sizeof(busid) - 1] = '\0';
        if (device_bound_to_usbip(busid))
            continue;

        snprintf(path, sizeof(path), USB_DEVICES "/%s/idVendor", busid);
        if (read_sysfs(path, vbuf, sizeof(vbuf)) < 0)
            continue;
        snprintf(path, sizeof(path), USB_DEVICES "/%s/idProduct", busid);
        if (read_sysfs(path, pbuf, sizeof(pbuf)) < 0)
            continue;
        vend = (unsigned)strtoul(vbuf, NULL, 16);
        prod = (unsigned)strtoul(pbuf, NULL, 16);
        snprintf(path, sizeof(path), USB_DEVICES "/%s/bDeviceClass", busid);
        if (read_sysfs(path, cbuf, sizeof(cbuf)) == 0 &&
            strtoul(cbuf, NULL, 16) == 0x09)
            continue;                  /* hub */
        snprintf(path, sizeof(path), USB_DEVICES "/%s/product", busid);
        read_sysfs(path, name, sizeof(name));

        if (builtin_denied(vend, prod) ||
            id_match(U.deny, U.n_deny, vend, prod))
            continue;
        if (guards_nic(busid)) {
            static int warned;
            if (!warned++)
                ulog("%04x:%04x (%s) carries an active network interface "
                     "— never exported", vend, prod, name);
            continue;
        }
        if (guards_mounted(busid)) {
            ulog("%04x:%04x (%s) has a mounted filesystem — not exported",
                 vend, prod, name);
            continue;
        }
        if (!id_match(U.allow, U.n_allow, vend, prod) &&
            !device_eligible(busid))
            continue;

        if (bind_device(busid) == 0) {
            ulog("exporting %04x:%04x (%s) busid %s", vend, prod,
                 name[0] ? name : "?", busid);
            if (U.n_bound < MAX_BOUND) {
                memcpy(U.bound[U.n_bound], busid, sizeof(busid));
                U.n_bound++;
            }
        } else {
            ulog("bind failed for %04x:%04x busid %s: %s", vend, prod,
                 busid, strerror(errno));
        }
    }
    closedir(dir);
}

static int is_pi3(void)
{
    char model[128] = "";

    read_sysfs("/proc/device-tree/model", model, sizeof(model));
    return strstr(model, "Raspberry Pi 3") != NULL;
}

int usb_export_init(int mode)
{
    const char *env = getenv("SREMFB_USB");

    if (mode < 0 && env && *env)
        mode = atoi(env) ? 1 : 0;
    if (mode < 0)
        mode = is_pi3() ? 0 : 1;   /* Pi 3: off unless forced (its NIC is
                                      USB; the guard protects it, but the
                                      board has little else to offer) */
    if (!mode)
        return 0;

    run_cmd("modprobe", "usbip-host", NULL);
    if (access(USBIP_DRIVER, F_OK) != 0) {
        ulog("usbip-host driver missing (kernel module not available)");
        return 0;
    }
    if (ensure_usbipd() < 0)
        return 0;

    parse_ids("SREMFB_USB_ALLOW", U.allow, &U.n_allow);
    parse_ids("SREMFB_USB_DENY", U.deny, &U.n_deny);
    U.active = 1;
    scan_and_bind();
    ulog("USB export active (%d device(s) bound)", U.n_bound);
    return 1;
}

void usb_export_tick(void)
{
    if (U.active)
        scan_and_bind();
}

void usb_export_stop(void)
{
    if (!U.active)
        return;
    for (int i = 0; i < U.n_bound; i++)
        unbind_device(U.bound[i]);
    U.n_bound = 0;
    U.active = 0;
}
