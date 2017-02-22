#define _XOPEN_SOURCE 500
#define _GNU_SOURCE

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ftw.h>
#include <search.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>



#include <unistd.h>
#include <sys/inotify.h>
#include <sys/types.h>
#include <dirent.h>

#define WFD2STR_BUF_SZ 128
#define FILEPATH_BUF_SZ 1024
#define HTAB_SIZE 4096
#define BUF_LEN (sizeof(struct inotify_event) + 255 + 1)

struct ibc_opts {
    char *directory;
    char *output_dir;
};

struct ibc {
    int fd;
    struct hsearch_data htab;
};

static struct ibc ibc;

static char *wfd2str(int wfd);
static const char *get_inotify_event_path(int wfd, const char *name);
static int parse_opts(struct ibc_opts *ibc_opts, int argc, char *argv[]);
static void free_opts(struct ibc_opts *ibc_opts);
static int add_watches(struct ibc_opts *ibc_opts);
static int cp(const char *to, const char *from);

static int nftw_callback(const char *fpath, const struct stat *sb,
        int typeflag, struct FTW *ftwbuf);

int main(int argc, char *argv[]) {
    int err, i;
    struct ibc_opts ibc_opts;
    struct inotify_event *evt;
    char buf[BUF_LEN], output[FILEPATH_BUF_SZ];
    const char *fp;

    memset(&ibc, 0, sizeof(struct ibc));
    err = hcreate_r(HTAB_SIZE, &ibc.htab);
    if (err == 0) {
        perror("hcreate_r");
        goto hcreate_error;
    }

    err = parse_opts(&ibc_opts, argc, argv);
    if (err == -1) {
        goto parsing_error;
    }

    for (i = 0; i < argc; i++)
        memset(argv[i], 0, strlen(argv[i]));


    err = ibc.fd = inotify_init();
    if (err == -1) {
        perror("inotify_init");
        goto inotify_init_error;
    }

    err = add_watches(&ibc_opts);
    if (err == -1) {
        goto add_watches_error;
    }

    while(read(ibc.fd, buf, BUF_LEN) > 0) {
        evt = (struct inotify_event *) buf;
        fp = get_inotify_event_path(evt->wd, evt->name);
        if (fp) {
            snprintf(output, FILEPATH_BUF_SZ, "%s/%s", ibc_opts.output_dir,
                    evt->name);
            cp(output, fp);
        }
    }

add_watches_error:
    close(ibc.fd);
inotify_init_error:
    free_opts(&ibc_opts);
parsing_error:
hcreate_error:
    hdestroy_r(&ibc.htab);
    return err;
}

static int parse_opts(struct ibc_opts *ibc_opts, int argc, char *argv[]) {
    int err;

    memset(ibc_opts, 0, sizeof(struct ibc_opts));
    int opt;
    while ((opt = getopt(argc, argv, "hd:o:")) != -1) {
       switch (opt) {
           case 'h':
               break;
           case 'd':
               ibc_opts->directory = strdup(optarg);
               break;
           case 'o':
               ibc_opts->output_dir = strdup(optarg);
               break;
           default:
               err = -1;
               fprintf(stderr, "Unknown option '%c'. Use -h to get help\n",
                       opt);
               goto unknown_option_err;
       }
    }

    if (ibc_opts->directory == NULL || ibc_opts->output_dir == NULL)
        err = -1;

unknown_option_err:
    return err;
}

static int cp(const char *to, const char *from)
{
    int fd_to, fd_from;
    char buf[4096];
    ssize_t nread;
    int saved_errno;

    fd_from = open(from, O_RDONLY);
    if (fd_from < 0) {
        return -1;
    }

    fd_to = open(to, O_WRONLY | O_CREAT | O_EXCL, 0666);
    if (fd_to < 0) {
        goto out_error;
    }

    while (nread = read(fd_from, buf, sizeof buf), nread > 0)
    {
        char *out_ptr = buf;
        ssize_t nwritten;

        do {
            nwritten = write(fd_to, out_ptr, nread);

            if (nwritten >= 0)
            {
                nread -= nwritten;
                out_ptr += nwritten;
            }
            else if (errno != EINTR)
            {
                goto out_error;
            }
        } while (nread > 0);
    }

    if (nread == 0)
    {
        if (close(fd_to) < 0)
        {
            fd_to = -1;
            goto out_error;
        }
        close(fd_from);

        /* Success! */
        return 0;
    }

  out_error:
    saved_errno = errno;

    close(fd_from);
    if (fd_to >= 0)
        close(fd_to);

    errno = saved_errno;
    return -1;
}

static char *wfd2str(int wfd) {
    char buf[WFD2STR_BUF_SZ];
    snprintf(buf, WFD2STR_BUF_SZ, "%d", wfd);
    return strdup(buf);
}

static const char *get_inotify_event_path(int wfd, const char *name) {
    static char buf[FILEPATH_BUF_SZ];

    int err;
    char *rv;
    ENTRY entry, *entry_rv;

    rv = NULL;
     entry.key = wfd2str(wfd);
     entry.data = NULL;

    err = hsearch_r(entry, FIND, &entry_rv, &ibc.htab);
    if (err == 0) {
        perror("hsearch_r");
        goto hsearch_r_err;
    }

    if (entry_rv != NULL) {
         snprintf(buf, FILEPATH_BUF_SZ, "%s/%s", (char *) entry_rv->data,
                 name);
         rv = buf;
    }

hsearch_r_err:
     free(entry.key);
     return rv;
}

void free_opts(struct ibc_opts *ibc_opts) {
     if (!ibc_opts && ibc_opts->directory)
         return;

    free(ibc_opts->directory);
    memset(ibc_opts, 0, sizeof(struct ibc_opts));
}

int add_watches(struct ibc_opts *ibc_opts) {
    return nftw(ibc_opts->directory, nftw_callback, 100, FTW_PHYS);
}

static int nftw_callback(const char *fpath, const struct stat *sb,
        int typeflag, struct FTW *ftwbuf) {
    int wfd, err;
    ENTRY entry, *entry_rv;

    if (typeflag != FTW_D)
        return 0;

    err = wfd = inotify_add_watch(ibc.fd, fpath, IN_CLOSE_WRITE | IN_MOVED_TO);
    if (err == -1) {
        perror("inotify_add_watch");
         return -1;
    }

    /* TODO: Memory assigned but never free, only in start so it will not
     * cause problems */
    entry.key = wfd2str(wfd);
    entry.data = (void *) strdup(fpath);
    err = hsearch_r(entry, ENTER, &entry_rv, &ibc.htab);
    if (err == 0) {
        perror("hsearch_r");
        return -1;
    }
    return 0;
}
