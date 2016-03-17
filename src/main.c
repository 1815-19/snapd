/*
 * Copyright (C) 2015 Canonical Ltd
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <linux/sched.h>
#include <sys/mount.h>
#include <sys/apparmor.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>
#include <sched.h>
#include <string.h>
#include <linux/kdev_t.h>
#include <stdlib.h>
#include <regex.h>
#include <grp.h>
#include <fcntl.h>
#include <glob.h>

#include "libudev.h"

#include "utils.h"
#include "seccomp.h"

#define MAX_BUF 1000

bool verify_appname(const char *appname) {
   // these chars are allowed in a appname
   const char* whitelist_re = "^[a-z0-9][a-z0-9+._-]+$";
   regex_t re;
   if (regcomp(&re, whitelist_re, REG_EXTENDED|REG_NOSUB) != 0)
      die("can not compile regex %s", whitelist_re);

   int status = regexec(&re, appname, 0, NULL, 0);
   regfree(&re);

   return (status == 0);
}

void run_snappy_app_dev_add(struct udev *u, const char *path, const char *appname) {
   debug("run_snappy_app_dev_add: %s %s", path, appname);
      struct udev_device *d = udev_device_new_from_syspath(u, path);
      if (d == NULL)
         die("can not find %s", path);
      dev_t devnum = udev_device_get_devnum (d);
      udev_device_unref(d);

      int status = 0;
      pid_t pid = fork();
      if (pid == 0) {
         char buf[64];
         unsigned major = MAJOR(devnum);
         unsigned minor = MINOR(devnum);
         must_snprintf(buf, sizeof(buf), "%u:%u", major, minor);
         if(execl("/lib/udev/snappy-app-dev", "/lib/udev/snappy-app-dev", "add", appname, path, buf, NULL) != 0)
            die("execlp failed");
      }
      if(waitpid(pid, &status, 0) < 0)
         die("waitpid failed");
      if(WIFEXITED(status) && WEXITSTATUS(status) != 0)
         die("child exited with status %i", WEXITSTATUS(status));
      else if(WIFSIGNALED(status))
         die("child died with signal %i", WTERMSIG(status));
}

void setup_udev_snappy_assign(const char *appname) {
   debug("setup_udev_snappy_assign");

   struct udev *u = udev_new();
   if (u == NULL)
      die("udev_new failed");

   const char* static_devices[] = {
      "/sys/class/mem/null",
      "/sys/class/mem/full",
      "/sys/class/mem/zero",
      "/sys/class/mem/random",
      "/sys/class/mem/urandom",
      "/sys/class/tty/tty",
      "/sys/class/tty/console",
      "/sys/class/tty/ptmx",
      NULL,
   };
   int i;
   for(i=0; static_devices[i] != NULL; i++) {
      run_snappy_app_dev_add(u, static_devices[i], appname);
   }

   struct udev_enumerate *devices = udev_enumerate_new(u);
   if (devices == NULL)
      die("udev_enumerate_new failed");

   if (udev_enumerate_add_match_tag (devices, "snappy-assign") != 0)
      die("udev_enumerate_add_match_tag");

   if(udev_enumerate_add_match_property (devices, "SNAPPY_APP", appname) != 0)
      die("udev_enumerate_add_match_property");

   if(udev_enumerate_scan_devices(devices) != 0)
      die("udev_enumerate_scan failed");

   struct udev_list_entry *l = udev_enumerate_get_list_entry (devices);
   while (l != NULL) {
      const char *path = udev_list_entry_get_name (l);
      if (path == NULL)
         die("udev_list_entry_get_name failed");
      run_snappy_app_dev_add(u, path, appname);
      l = udev_list_entry_get_next(l);
   }

   udev_enumerate_unref(devices);
   udev_unref(u);
}

void setup_devices_cgroup(const char *appname) {
   debug("setup_devices_cgroup");

   // extra paranoia
   if(!verify_appname(appname))
      die("appname %s not allowed", appname);

   // create devices cgroup controller
   char cgroup_dir[PATH_MAX];
   must_snprintf(cgroup_dir, sizeof(cgroup_dir), "/sys/fs/cgroup/devices/snappy.%s/", appname);

   if (mkdir(cgroup_dir, 0755) < 0 && errno != EEXIST)
         die("mkdir failed");

   // move ourselves into it
   char cgroup_file[PATH_MAX];
   must_snprintf(cgroup_file, sizeof(cgroup_file), "%s%s", cgroup_dir, "tasks");

   char buf[128];
   must_snprintf(buf, sizeof(buf), "%i", getpid());
   write_string_to_file(cgroup_file, buf);

   // deny by default
   must_snprintf(cgroup_file, sizeof(cgroup_file), "%s%s", cgroup_dir, "devices.deny");
   write_string_to_file(cgroup_file, "a");

}

bool snappy_udev_setup_required(const char *appname) {
   debug("snappy_udev_setup_required");

   // extra paranoia
   if(!verify_appname(appname))
      die("appname %s not allowed", appname);

   char override_file[PATH_MAX];
   must_snprintf(override_file, sizeof(override_file), "/var/lib/apparmor/clicks/%s.json.additional", appname);

   // if a snap package gets unrestricted apparmor access we need to setup
   // a device cgroup.
   //
   // the "needle" string is what gives this access so we search for that
   // here
   const char *needle =
      "{"                          "\n"
      " \"write_path\": ["         "\n"
      "   \"/dev/**\""             "\n"
      " ],"                        "\n"
      " \"read_path\": ["          "\n"
      "   \"/run/udev/data/*\""     "\n"
      " ]\n"
      "}";
   debug("looking for: '%s'", needle);
   char content[strlen(needle)];

   int fd = open(override_file, O_CLOEXEC | O_NOFOLLOW | O_RDONLY);
   if (fd < 0)
      return false;
   int n = read(fd, content, sizeof(content));
   close(fd);
   if (n < sizeof(content))
      return false;

   // memcpy so that we don't have to deal with \0 in the input
   if (memcmp(content, needle, strlen(needle)) == 0) {
      debug("found needle, need to apply udev setup");
      return true;
   }

   return false;
}

bool is_running_on_classic_ubuntu() {
   return (access("/var/lib/dpkg/status", F_OK) == 0);
}

void setup_private_mount(const char* appname) {
    uid_t uid = getuid();
    gid_t gid = getgid();
    char tmpdir[MAX_BUF] = {0};

    // Create a 0700 base directory, this is the base dir that is
    // protected from other users.
    //
    // Under that basedir, we put a 1777 /tmp dir that is then bind
    // mounted for the applications to use
    must_snprintf(tmpdir, sizeof(tmpdir), "/tmp/snap.%d_%s_XXXXXX", uid, appname);
    if (mkdtemp(tmpdir) == NULL) {
        die("unable to create tmpdir");
    }

    // now we create a 1777 /tmp inside our private dir
    mode_t old_mask = umask(0);
    char *d = strdup(tmpdir);
    if (!d) {
        die("Out of memory");
    }
    must_snprintf(tmpdir, sizeof(tmpdir), "%s/tmp", d);
    free(d);

    if (mkdir(tmpdir, 01777) != 0) {
       die("unable to create /tmp inside private dir");
    }
    umask(old_mask);

    // MS_BIND is there from linux 2.4
    if (mount(tmpdir, "/tmp", NULL, MS_BIND, NULL) != 0) {
        die("unable to bind private /tmp");
    }
    // MS_PRIVATE needs linux > 2.6.11
    if (mount("none", "/tmp", NULL, MS_PRIVATE, NULL) != 0) {
       die("unable to make /tmp/ private");
    }

    // do the chown after the bind mount to avoid potential shenanigans
    if (chown("/tmp/", uid, gid) < 0) {
        die("unable to chown tmpdir");
    }

    // ensure we set the various TMPDIRs to our newly created tmpdir
    const char *tmpd[] = {"TMPDIR", "TEMPDIR", "SNAP_APP_TMPDIR", NULL};
    int i;
    for (i=0; tmpd[i] != NULL; i++) {
       if (setenv(tmpd[i], "/tmp", 1) != 0) {
          die("unable to set '%s'", tmpd[i]);
       }
    }
}

void setup_private_pts() {
    struct stat st;
    if (stat("/dev/pts", &st) != 0 || !S_ISDIR(st.st_mode)) {
        die("/dev/pts doesn't exist or is not a directory");
    }

    // ptmxmode=000 or 666
    if (mount("devpts", "/dev/pts", "devpts", MS_MGC_VAL,
              "newinstance,ptmxmode=0666,mode=0620,gid=5")) {
        die("unable to mount a new instance of '/dev/pts'");
    }

    // if /dev/ptmx exists, bind mount over it otherwise, create a symlink
    if (stat("/dev/ptmx", &st) == 0) {
	if (mount("/dev/pts/ptmx", "/dev/ptmx", "none", MS_BIND | MS_NOSUID | MS_NOEXEC, 0)) {
            die("unable to mount '/dev/pts/ptmx'->'/dev/ptmx'");
	}
    } else {
        if (!symlink("/dev/pts/ptmx", "/dev/ptmx")) {
            die("unable to symlink '/dev/pts/ptmx'->'/dev/ptmx'");
	}
    }
}

void setup_snappy_os_mounts() {
   debug("setup_snappy_os_mounts()\n");

   // FIXME: hardcoded "ubuntu-core.*"
   glob_t glob_res;
   if (glob("/snaps/ubuntu-core*/current/", 0, NULL, &glob_res) != 0) {
      die("can not find a snappy os");
   }
   if ((glob_res.gl_pathc =! 1)) {
      die("expected 1 os snap, found %i", (int)glob_res.gl_pathc);
   }
   char *mountpoint = glob_res.gl_pathv[0];

   // we mount some whitelisted directories
   //
   // Note that we do not mount "/etc/" from snappy. We could do that,
   // but if we do we need to ensure that data like /etc/{hostname,hosts,
   // passwd,groups} is in sync between the two systems (probably via
   // selected bind mounts of those files).
   const char *mounts[] = {"/bin", "/sbin", "/lib", "/lib64", "/usr"};
   for (int i=0; i < sizeof(mounts)/sizeof(char*); i++) {
      // we mount the OS snap /bin over the real /bin in this NS
      const char *dst = mounts[i];

      char buf[512];
      must_snprintf(buf, sizeof(buf), "%s%s", mountpoint, dst);
      const char *src = buf;

      debug("mounting %s -> %s\n", src, dst);
      if (mount(src, dst, NULL, MS_BIND, NULL) != 0) {
         die("unable to bind %s to %s", src, dst);
      }
   }

   globfree(&glob_res);
}

void setup_slave_mount_namespace() {
   // unshare() and CLONE_NEWNS require linux >= 2.6.16 and glibc >= 2.14
   // if using an older glibc, you'd need -D_BSD_SOURCE or -D_SVID_SORUCE.
   if (unshare(CLONE_NEWNS) < 0) {
      die("unable to set up mount namespace");
   }

   // make our "/" a rslave of the real "/". this means that
   // mounts from the host "/" get propagated to our namespace
   // (i.e. we see new media mounts)
   if (mount("none", "/", NULL, MS_REC|MS_SLAVE, NULL) != 0) {
      die("can not make make / rslave");
   }
}

void mkpath(const char *const path) {
   // If asked to create an empty path, return immediately.
   if (strlen(path) == 0) {
      return;
   }

   // We're going to use strtok_r, which needs to modify the path, so we'll make
   // a copy of it.
   char *path_copy = strdup(path);
   if (path_copy == NULL) {
      die("failed to create user data directory");
   }

   // Open flags to use while we walk the user data path:
   // - Don't follow symlinks
   // - Don't allow child access to file descriptor
   // - Only open a directory (fail otherwise)
   int open_flags = O_NOFOLLOW | O_CLOEXEC | O_DIRECTORY;

   // We're going to create each path segment via openat/mkdirat calls instead
   // of mkdir calls, to avoid following symlinks and placing the user data
   // directory somewhere we never intended for it to go. The first step is to
   // get an initial file descriptor.
   int fd = AT_FDCWD;
   if (path_copy[0] == '/') {
      fd = open("/", open_flags);
      if (fd < 0) {
         free(path_copy);
         die("failed to create user data directory");
      }
   }

   // strtok_r needs a pointer to keep track of where it is in the string.
   char *path_walker;

   // Initialize tokenizer and obtain first path segment.
   char *path_segment = strtok_r(path_copy, "/", &path_walker);
   while (path_segment) {
      // Try to create the directory. It's okay if it already existed, but any
      // other error is fatal.
      if (mkdirat(fd, path_segment, 0755) < 0 && errno != EEXIST) {
         close(fd);
         free(path_copy);
         die("failed to create user data directory");
      }

      // Open the parent directory we just made (and close the previous one) so
      // we can continue down the path.
      int previous_fd = fd;
      fd = openat(fd, path_segment, open_flags);
      close(previous_fd);
      if (fd < 0) {
         free(path_copy);
         die("failed to create user data directory");
      }

      // Obtain the next path segment.
      path_segment = strtok_r(NULL, "/", &path_walker);
   }

   // Close the descriptor for the final directory in the path.
   close(fd);

   free(path_copy);
}

void setup_user_data() {
   const char *user_data = getenv("SNAP_USER_DATA");

   // If $SNAP_USER_DATA wasn't defined, check the deprecated
   // $SNAP_APP_USER_DATA_PATH.
   if (user_data == NULL) {
      user_data = getenv("SNAP_APP_USER_DATA_PATH");
      // If it's still not defined, there's nothing to do. No need to die,
      // there's simply no directory to create.
      if (user_data == NULL) {
         return;
      }
   }

   // Only support absolute paths.
   if (user_data[0] != '/') {
      die("user data directory must be an absolute path");
   }

   mkpath(user_data);
}

int main(int argc, char **argv)
{
   const int NR_ARGS = 3;
   if(argc < NR_ARGS+1)
      die("Usage: %s <appname> <apparmor> <binary>", argv[0]);

   const char *appname = argv[1];
   const char *aa_profile = argv[2];
   const char *binary = argv[3];

   if(!verify_appname(appname))
      die("appname %s not allowed", appname);

   // this code always needs to run as root for the cgroup/udev setup,
   // however for the tests we allow it to run as non-root
   if(geteuid() != 0 && getenv("UBUNTU_CORE_LAUNCHER_NO_ROOT") == NULL) {
      die("need to run as root or suid");
   }

   if(geteuid() == 0) {

      // ensure we run in our own slave mount namespace, this will
      // create a new mount namespace and make it a slave of "/"
      //
      // Note that this means that no mount actions inside our
      // namespace are propagated to the main "/". We need this
      // both for the private /tmp we create and for the bind
      // mounts we do on a classic ubuntu system
      //
      // This also means you can't run an automount daemon unter
      // this launcher
      setup_slave_mount_namespace();

      // do the mounting if run on a non-native snappy system
      if(is_running_on_classic_ubuntu()) {
         setup_snappy_os_mounts();
      }

      // set up private mounts
      setup_private_mount(appname);

      // set up private /dev/pts
      setup_private_pts();

      // this needs to happen as root
      if(snappy_udev_setup_required(appname)) {
         setup_devices_cgroup(appname);
         setup_udev_snappy_assign(appname);
      }

      // the rest does not so drop privs back to calling user
      unsigned real_uid = getuid();
      unsigned real_gid = getgid();

      // Note that we do not call setgroups() here because its ok
      // that the user keeps the groups he already belongs to
      if (setgid(real_gid) != 0)
         die("setgid failed");
      if (setuid(real_uid) != 0)
         die("setuid failed");

      if(real_gid != 0 && (getuid() == 0 || geteuid() == 0))
         die("dropping privs did not work");
      if(real_uid != 0 && (getgid() == 0 || getegid() == 0))
         die("dropping privs did not work");
   }

   // Ensure that the user data path exists.
   setup_user_data();

   //https://wiki.ubuntu.com/SecurityTeam/Specifications/SnappyConfinement#ubuntu-snapp-launch

   int rc = 0;
   // set apparmor rules
   rc = aa_change_onexec(aa_profile);
   if (rc != 0) {
   if (getenv("SNAPPY_LAUNCHER_INSIDE_TESTS") == NULL)
      die("aa_change_onexec failed with %i", rc);
   }

   // set seccomp
   rc = seccomp_load_filters(aa_profile);
   if (rc != 0)
      die("seccomp_load_filters failed with %i", rc);

   // and exec the new binary
   argv[NR_ARGS] = (char*)binary,
   execv(binary, (char *const*)&argv[NR_ARGS]);
   perror("execv failed");
   return 1;
}
