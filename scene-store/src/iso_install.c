/*
 * iso_install.c - the first-party installer guest app.
 *
 * Starts with SCENE_STORE_PORT set, connects back over TCP, builds a
 * window through the full scene_app stack (titlebar + close + content +
 * a status LABEL) and installs the running system onto a target disk so
 * the disk boots standalone (BIOS -> grub MBR -> kernel + initramfs from
 * the disk's /boot -> the standard init persist=auto flow):
 *
 *   target : argv[1], else the SCENE_INSTALL_TO env var (the host
 *            forwards kernel cmdline installto=DEV through it), else
 *            auto-detect: the first real (non-loop/ram/fd) block device
 *            that nothing is mounted from (/proc/mounts) - the running
 *            root disk is always excluded by that check.
 *
 *   steps  : 1. fdisk: fresh DOS label, one primary partition
 *            2. mke2fs -F -q on the partition (ext2 fs, ext4 driver)
 *            3. mount -t ext4 /dev/XDn /mnt/tgt
 *            4. cp -a of every top-level dir (the persist init's list
 *               plus /boot so the kernel + initramfs land on the disk)
 *            5. grub-install --no-floppy --recheck
 *               --boot-directory=/mnt/tgt/boot (BIOS core.img into the
 *               MBR gap; needs the grub + grub-bios apk packages baked
 *               into the rootfs by build.sh)
 *            6. /mnt/tgt/boot/grub/grub.cfg: menuentry booting
 *               /boot/vmlinuz-* + /boot/initramfs-*.cpio.gz with
 *               autolaunch=iso-terminal persist=auto
 *            7. unmount, "done - restart, boot the disk"
 *
 * On Windows the step bodies are compiled OUT (#ifndef _WIN32): the
 * state machine still runs and reports "(stub)" so the scene wiring is
 * exercised. The QEMU installer + disk boot proofs are the functional
 * verification.
 *
 * Exit: 0 when the window is closed; the installer itself runs to
 * completion and leaves the UI up ("done").
 */
#define _POSIX_C_SOURCE 200809L
#include "scene_app.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>

#if defined(_WIN32)
#include <windows.h>
static void msleep(unsigned m) { Sleep(m); }
#else
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
static void msleep(unsigned m)
{
    struct timespec ts = { (time_t)(m / 1000), (long)(m % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}
#endif

/* ---- layout ------------------------------------------------------------- */
#define INS_X 100
#define INS_Y 60
#define INS_W 320
#define INS_H 150
#define STATUS_NODE 40012u

/* ---- installer state machine -------------------------------------------- */
enum {
    ST_DETECT = 0,   /* pick the target disk ("target: /dev/vdb") */
    ST_FDISK,        /* partition                                 */
    ST_MKFS,         /* filesystem                                */
    ST_MOUNT,        /* mount                                     */
    ST_COPY,         /* copy rootfs                               */
    ST_GRUB,         /* grub-install                              */
    ST_CFG,          /* grub.cfg                                  */
    ST_UMOUNT,       /* unmount                                   */
    ST_DONE,         /* done                                      */
    ST_FAIL          /* fatal                                     */
};

static scene_app *g_app;
static int        g_step = ST_DETECT;
static char       g_dev[64];      /* whole disk, e.g. /dev/vdb   */
static char       g_part[64];     /* partition,   e.g. /dev/vdb1 */
static char       g_msg[160];

static void dlog(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fflush(stderr);
}

static void set_status(const char *s)
{
    snprintf(g_msg, sizeof(g_msg), "%s", s);
    scene_app_set_text(g_app, STATUS_NODE, 0, s);
    scene_app_present(g_app);
    scene_app_flush(g_app);
    dlog("iso-install: %s\n", s);
}

#if !defined(_WIN32)

/* ---- POSIX-only: the real step bodies ------------------------------------ */

/* Run a shell pipeline, appending its output to /var/log/iso-install.log
 * (DISK-backed: /tmp is a tmpfs in the running system and its contents
 * die with the power cut; the persist disk keeps the log for post-mortem
 * reads). The last five lines are echoed to stderr -> serial. Returns
 * the command's exit status. */
static int sh(const char *cmd)
{
    char buf[672];
    int rc;
    snprintf(buf, sizeof(buf),
             "(%s) >> /var/log/iso-install.log 2>&1; "
             "rc=$?; echo rc=$rc >> /var/log/iso-install.log; "
             "tail -5 /var/log/iso-install.log >&2; exit $rc", cmd);
    rc = system(buf);
    return rc == -1 ? 1 : (WIFEXITED(rc) ? WEXITSTATUS(rc) : 1);
}

/* True when nothing is mounted from /dev/<base> (any partition of it). */
static int disk_unmounted(const char *base)
{
    FILE *f = fopen("/proc/mounts", "r");
    char line[512];
    int hit = 0;
    if (!f)
        return 1;
    while (fgets(line, sizeof(line), f)) {
        char src[256];
        if (sscanf(line, "%255s", src) == 1) {
            if (strncmp(src, "/dev/", 5) == 0 &&
                strncmp(src + 5, base, strlen(base)) == 0) {
                hit = 1;
                break;
            }
        }
    }
    fclose(f);
    return !hit;
}

/* Auto-detect: first real disk (has /sys/block/X/device) that is neither
 * loop/ram/fd and that nothing is mounted from. */
static int detect_dev(char *out, size_t outsz)
{
    struct dirent **list = NULL;
    int n = scandir("/sys/block", &list, NULL, alphasort);
    int i;
    for (i = 0; i < n; i++) {
        const char *name = list[i]->d_name;
        char path[256];
        struct stat st;
        if (strncmp(name, "loop", 4) == 0 || strncmp(name, "ram", 3) == 0 ||
            strncmp(name, "fd", 2) == 0 || strncmp(name, "sr", 2) == 0)
            continue;
        snprintf(path, sizeof(path), "/sys/block/%.40s/device", name);
        if (stat(path, &st) != 0)
            continue;
        snprintf(path, sizeof(path), "/dev/%.40s", name);
        if (stat(path, &st) != 0 || !S_ISBLK(st.st_mode))
            continue;
        if (!disk_unmounted(name))
            continue;
        snprintf(out, outsz, "/dev/%.40s", name);
        free(list);
        return 0;
    }
    free(list);
    return -1;
}

/* /dev/vdb -> /dev/vdb1 ; /dev/nvme0n1 -> /dev/nvme0n1p1 */
static void mk_part_name(const char *dev, char *out, size_t outsz)
{
    const char *base = strrchr(dev, '/');
    size_t n;
    base = base ? base + 1 : dev;
    n = strlen(base);
    if (n > 0 && base[n - 1] >= '0' && base[n - 1] <= '9')
        snprintf(out, outsz, "%.48sp1", dev);
    else
        snprintf(out, outsz, "%.48s1", dev);
}

/* Top-level dirs the rootfs copy carries. /boot is included so the
 * kernel + initramfs land on the installed disk (the live /boot exists
 * because the initramfs now carries it). */
static const char *const g_copy_dirs[] = {
    "bin", "sbin", "usr", "lib", "lib64", "etc", "home", "root",
    "var", "opt", "boot"
};

static int step_detect(void)
{
    const char *want = getenv("SCENE_INSTALL_TO");
    struct stat st;
    if (want && *want)
        snprintf(g_dev, sizeof(g_dev), "%.48s", want);
    else if (detect_dev(g_dev, sizeof(g_dev)) != 0) {
        set_status("no target disk");
        g_step = ST_FAIL;
        return -1;
    }
    if (stat(g_dev, &st) != 0 || !S_ISBLK(st.st_mode)) {
        snprintf(g_msg, sizeof(g_msg), "bad target %s", g_dev);
        set_status(g_msg);
        g_step = ST_FAIL;
        return -1;
    }
    mk_part_name(g_dev, g_part, sizeof(g_part));
    snprintf(g_msg, sizeof(g_msg), "target: %s", g_dev);
    set_status(g_msg);
    return 0;
}

static int step_fdisk(void)
{
    char cmd[256];
    int rc, tries;
    set_status("partitioning...");
    /* util-linux sfdisk in SCRIPT mode (non-interactive by design; the
     * busybox fdisk applet is not compiled into this busybox build and
     * interactive fdisk cannot be scripted reliably across versions):
     * a DOS label with one Linux partition starting at sector 2048. */
    snprintf(cmd, sizeof(cmd),
             "printf 'label: dos\\nstart=2048, type=83\\n' | sfdisk %s",
             g_dev);
    rc = sh(cmd);
    dlog("iso-install: fdisk rc=%d\n", rc);
    if (rc != 0) {
        set_status("fdisk failed");
        return -1;
    }
    /* The partition node can lag the table write; poll up to 10 s. */
    for (tries = 0; tries < 10; tries++) {
        snprintf(cmd, sizeof(cmd), "[ -b %s ]", g_part);
        rc = sh(cmd);
        dlog("iso-install: node check %d rc=%d\n", tries, rc);
        if (rc == 0)
            return 0;
        sleep(1);
    }
    set_status("partition node missing");
    return -1;
}

static int step_mkfs(void)
{
    char cmd[256];
    set_status("formatting...");
    snprintf(cmd, sizeof(cmd), "mke2fs -F -q %s", g_part);
    if (sh(cmd) != 0) {
        set_status("format failed");
        return -1;
    }
    return 0;
}

static int step_mount(void)
{
    char cmd[256];
    set_status("mounting...");
    snprintf(cmd, sizeof(cmd),
             "mkdir -p /mnt/tgt && mount -t ext4 %s /mnt/tgt", g_part);
    if (sh(cmd) != 0) {
        set_status("mount failed");
        return -1;
    }
    return 0;
}

static int step_copy(void)
{
    char cmd[1024];
    int rc, i;
    set_status("copying rootfs...");
    snprintf(cmd, sizeof(cmd), "bad=0; for d in ");
    for (i = 0; i < (int)(sizeof(g_copy_dirs) / sizeof(g_copy_dirs[0])); i++)
        snprintf(cmd + strlen(cmd), sizeof(cmd) - strlen(cmd), "%s ",
                 g_copy_dirs[i]);
    snprintf(cmd + strlen(cmd), sizeof(cmd) - strlen(cmd),
             "; do [ -e /$d ] && cp -a /$d /mnt/tgt/ || bad=1; done; "
             "mkdir -p /mnt/tgt/dev/pts /mnt/tgt/proc /mnt/tgt/sys "
             "/mnt/tgt/run /mnt/tgt/tmp && touch /mnt/tgt/.iso-rootfs-v1; "
             "exit $bad");
    rc = sh(cmd);
    if (rc != 0) {
        set_status("copy failed");
        return -1;
    }
    return 0;
}

static int step_grub(void)
{
    char cmd[384];
    int tries;
    set_status("installing grub...");
    snprintf(cmd, sizeof(cmd),
             "grub-install --no-floppy --recheck "
             "--boot-directory=/mnt/tgt/boot %s", g_dev);
    if (sh(cmd) != 0) {
        set_status("grub failed");
        return -1;
    }
    for (tries = 0; tries < 10; tries++) {
        if (sh("test -f /mnt/tgt/boot/grub/grub.cfg") == 0 ||
            sh("test -d /mnt/tgt/boot/grub/i386-pc") == 0)
            break;
        sh("sleep 1");
    }
    return 0;
}

static int step_cfg(void)
{
    DIR *d;
    struct dirent *de;
    char kern[128] = "", initrd[128] = "";
    FILE *f;
    set_status("writing grub.cfg...");
    d = opendir("/mnt/tgt/boot");
    if (!d) {
        set_status("no /boot on target");
        return -1;
    }
    while ((de = readdir(d))) {
        if (strncmp(de->d_name, "vmlinuz-", 8) == 0)
            snprintf(kern, sizeof(kern), "%.110s", de->d_name);
        else if (strncmp(de->d_name, "initramfs-", 10) == 0 &&
                 strstr(de->d_name, ".cpio.gz"))
            snprintf(initrd, sizeof(initrd), "%.120s", de->d_name);
    }
    closedir(d);
    if (!*kern || !*initrd) {
        set_status("kernel/initramfs missing");
        return -1;
    }
    f = fopen("/mnt/tgt/boot/grub/grub.cfg", "w");
    if (!f) {
        set_status("grub.cfg write failed");
        return -1;
    }
    fprintf(f, "set timeout=2\nset default=0\n");
    fprintf(f, "menuentry \"scene-store\" {\n");
    fprintf(f, "    linux /boot/%s quiet autolaunch=iso-terminal persist=auto\n",
            kern);
    fprintf(f, "    initrd /boot/%s\n", initrd);
    fprintf(f, "}\n");
    fclose(f);
    dlog("iso-install: grub.cfg: %s + %s\n", kern, initrd);
    return 0;
}

static int step_umount(void)
{
    set_status("unmounting...");
    if (sh("umount /mnt/tgt") != 0)
        sh("umount -l /mnt/tgt");
    return 0;
}

/* One step per loop pass. Returns 1 when the install finished. */
static int inst_tick(void)
{
    switch (g_step) {
    case ST_DETECT: g_step = step_detect() != 0 ? ST_FAIL : ST_FDISK; break;
    case ST_FDISK:  g_step = step_fdisk()  != 0 ? ST_FAIL : ST_MKFS;  break;
    case ST_MKFS:   g_step = step_mkfs()   != 0 ? ST_FAIL : ST_MOUNT; break;
    case ST_MOUNT:  g_step = step_mount()  != 0 ? ST_FAIL : ST_COPY;  break;
    case ST_COPY:   g_step = step_copy()   != 0 ? ST_FAIL : ST_GRUB;  break;
    case ST_GRUB:   g_step = step_grub()   != 0 ? ST_FAIL : ST_CFG;   break;
    case ST_CFG:    g_step = step_cfg()    != 0 ? ST_FAIL : ST_UMOUNT; break;
    case ST_UMOUNT: step_umount();
                    g_step = ST_DONE;
                    set_status("done - restart, boot the disk");
                    return 1;
    case ST_DONE:
    case ST_FAIL:
    default:
        return 1;
    }
    return g_step == ST_FAIL;
}

#else /* _WIN32: stub step bodies, the state machine still drives */

static int inst_tick(void)
{
    static const char *const msgs[] = {
        "target: win32 stub",
        "partitioning... (stub)",
        "formatting... (stub)",
        "mounting... (stub)",
        "copying... (stub)",
        "grub... (stub)",
        "grub.cfg... (stub)",
        "unmounting... (stub)",
        "done - restart, boot the disk"
    };
    (void)g_dev; (void)g_part;
    if (g_step >= ST_DETECT && g_step <= ST_DONE)
        set_status(msgs[g_step]);
    g_step++;
    if (g_step > ST_DONE) {
        g_step = ST_DONE;
        return 1;
    }
    return 0;
}

#endif

/* ---- window close --------------------------------------------------------- */

static scene_node_id g_content;

static void on_activate(void *ud, uint64_t seq, scene_node_id id)
{
    (void)ud; (void)seq;
    if (id == g_content - 1)        /* close button = base+3 */
        exit(0);
}

int main(int argc, char **argv)
{
    const char *port;
    scene_node_id content;
    int i;

    if (argc > 1 && strcmp(argv[1], "--help") == 0) {
        printf("usage: iso-install [DEVICE]\n"
               "       installs the running system onto a disk and makes\n"
               "       it BIOS-bootable (grub). DEVICE default: the\n"
               "       SCENE_INSTALL_TO env var, else auto-detect.\n");
        return 0;
    }
    if (argc > 1 && strncmp(argv[1], "/dev/", 5) == 0) {
        /* an explicit argv device wins over the env override */
#if defined(_WIN32)
        _putenv_s("SCENE_INSTALL_TO", argv[1]);
#else
        setenv("SCENE_INSTALL_TO", argv[1], 1);
#endif
    }

    port = getenv("SCENE_STORE_PORT");
    if (!port) return 2;
    dlog("iso-install: start port=%s\n", port);

    char target[64];
    snprintf(target, sizeof(target), "127.0.0.1:%s", port);
    scene_transport *t = scene_tcp_client(target);
    if (!t) { dlog("iso-install: tcp client failed\n"); return 3; }

    scene_app_cbs cbs;
    memset(&cbs, 0, sizeof(cbs));
    cbs.activate = on_activate;
    g_app = scene_app_new_on(t, target, &cbs, NULL);
    if (!g_app) { dlog("iso-install: app_new failed\n"); return 4; }
    scene_tcp_set_nonblock(t, 1);

    for (i = 0; i < 500; i++) {
        scene_app_pump(g_app);
        scene_app_flush(g_app);
        if (scene_client_welcomed(scene_app_client(g_app))) break;
        msleep(5);
    }
    if (i >= 500) { dlog("iso-install: no welcome\n"); return 5; }
    dlog("iso-install: welcomed\n");

    content = scene_app_create_window_role(g_app, INS_X, INS_Y, INS_W, INS_H,
                                           "iso-install", SCENE_ROLE_GENERIC);
    if (content == SCENE_NO_PARENT) {
        dlog("iso-install: window create failed\n");
        return 6;
    }
    g_content = content;
    {
        static const scene_rect sr = {INS_X + 4, INS_Y + 40, INS_W - 8, 16};
        scene_client_create_node(scene_app_client(g_app), content,
                                 STATUS_NODE, SCENE_ROLE_LABEL, &sr,
                                 SCENE_FLAG_VISIBLE);
    }
    scene_app_present(g_app);
    scene_app_flush(g_app);

    for (;;) {
        scene_app_pump(g_app);
        scene_app_flush(g_app);
        inst_tick();
        msleep(5);
    }
}