#include <sys/types.h>
#include <sys/dir.h>
#include <sys/event.h>
// #include <sys/resource.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <limits.h>

// #define PATH_MAX                 1024

/* TODO:
 *   * print fd limits
 *   * add restart option
 *   * unit test
 */

struct wchopt {
    int wt;
    int once;
    char** cmd;
    char* dir;
    struct str_list* excludes;
    struct str_list* watchlist;
};

struct str_list {
    int size;
    struct str_node *head;
    struct str_node *tail;
};

struct str_node {
    char *str;
    struct str_node *next;
};

struct str_list* str_list_create();
void str_list_append(struct str_list* slist, char* p);
void str_list_append_node(struct str_list* slist, struct str_node *node);
void str_list_free(struct str_list* slist, int freestr);
struct str_node* str_list_shift(struct str_list* slist);

void run(struct wchopt *opt);
void walk(char* path, struct str_list* excludes, struct str_list* flist);
int check(char* path, struct str_list* excludes);
char* normpath(char* path);
void onchange(char* file, char** cmd, int wt);


struct str_list* str_list_create() {
    struct str_list *slist = malloc(sizeof(struct str_list));
    slist->size = 0;
    slist->head = NULL;
    slist->tail = NULL;
    return slist;
}

void str_list_append(struct str_list* slist, char* p) {
    struct str_node* snode = malloc(sizeof(struct str_node));
    snode->str = p;             /* do not alloc memory for str */
    snode->next = NULL;
    slist->size++;

    if (slist->tail == NULL) {
        slist->head = snode;
        slist->tail = snode;
    } else {
        slist->tail->next = snode;
        slist->tail = snode;
    }
}

void str_list_append_node(struct str_list* slist, struct str_node *node) {
    slist->tail->next = node;
    slist->tail = node;
    if (slist->tail) {
        slist->tail->next = NULL;
    }
}


struct str_node* str_list_shift(struct str_list* slist) {
    struct str_node* head = slist->head;

    if (!head) {
        return NULL;
    }
    slist->head = head->next;
    if (!slist->head) {
        slist->tail = NULL;
    }

    return head;
};

void str_list_free(struct str_list *slist, int strfree) {
    struct str_node* snode = slist->head;
    while (snode) {
        struct str_node* next = snode->next;
        if (strfree) {
            free(snode->str);
        }
        free(snode);
        snode = next;
    }

    free(slist);
}

void walk(char* path, struct str_list* excludes, struct str_list* flist) {
    DIR *dirp;
    struct dirent * dent;
    int len;
    char newpath[PATH_MAX];

    len = strlen(path);
    strcpy(newpath, path);
    if (len != 1) {             /* root */
        newpath[len++] = '/';
    }

    dirp = opendir(path);
    if (dirp == NULL) {
        fprintf(stderr, "failed to open dir: %s\n", path);
        return;
    }

    // str_list_append(flist, path);
    while ((dent = readdir(dirp))) {
        if (!strcmp(dent->d_name, ".") || !strcmp(dent->d_name, "..")) {
            continue;
        }
        strncpy(newpath + len, dent->d_name, PATH_MAX - len);
        if (check(newpath, excludes) == 1) {
            continue;
        }
        if (dent->d_type == DT_DIR) {
            walk(strdup(newpath), excludes, flist);
        }
        if (dent->d_type == DT_REG) {
            str_list_append(flist, strdup(newpath));
        }
    }

    closedir(dirp);
}

void load(struct str_list* watchlist,
          struct str_list* excludes,
          struct str_list* flist) {

    struct stat fstat;
    struct str_node* watch;

    while ((watch = str_list_shift(watchlist))) {
        if (-1 == lstat(watch->str, &fstat)) {
            fprintf(stderr, "lstat error: %s\n", watch->str);
            continue;
        }
        if (check(watch->str, excludes) == 1) {
            continue;
        }
        if (S_ISREG(fstat.st_mode)) {
            str_list_append_node(flist, watch);
        }
        if (S_ISDIR(fstat.st_mode)) {
            walk(watch->str, excludes, flist);
        }

        fprintf(stderr, "only regular file can be watched: %s\n", watch->str);
    }
}

void run(struct wchopt* opt) {
    int wt = opt->wt;
    int once = opt->once;
    char** cmd = opt->cmd;
    // char* dir = opt->dir;
    struct str_list* excludes = opt->excludes;
    struct str_list* watchlist = opt->watchlist;

    struct str_list* flist;
    struct str_node* fnode;
    int kq;                     /* kqueue */
    int nev;                    /* number of events */
    int fd;
    int isdir = 0;
    flist = str_list_create();
    load(watchlist, excludes, flist);
    free(watchlist);
    // walk(normpath(dir), excludes, flist);
    struct kevent events[flist->size];
    struct kevent changes[flist->size];

    // create a kqueue
    kq = kqueue();
    if (kq < 0) {
        fprintf(stderr, "failed to create kqueue\n");
        exit(1);
    }

    // create kevents
    fnode = flist->head;
    for (int i = 0; i < flist->size; i++) {
        fd = open(fnode->str, O_RDONLY | O_CLOEXEC);
        if (fd == -1) {
            fprintf(stderr, "failed to open file: %s\n", fnode->str);
            fnode = fnode->next;
            continue;
        }
        printf("watch: %s\n", fnode->str);

        EV_SET(&changes[i], fd, EVFILT_VNODE,
               EV_ADD | EV_ENABLE | EV_CLEAR,
               NOTE_WRITE,
               0, fnode->str);

        fnode = fnode->next;
    }

    // read kevents from kqueue
    while (1) {
        nev = kevent(kq, changes, flist->size, events, flist->size, NULL);
        if (nev == -1) {
            fprintf(stderr, "failed to receive kevent");
            continue;
        }

        if (once) {
            nev = 1;
        }
        for (int i = 0; i < nev; i++) {
            if (events[i].fflags & NOTE_WRITE) {
                char *fname = (char*) events[i].udata;
                onchange(fname, cmd, wt);
            }
        }
    }
}

void onchange(char* file, char** cmd, int wt) {
    /* char* file not in use */
    int pid;
    int ppid;
    int status;                 /* not in use */

    pid = fork();
    if (pid < 0) {              /* fork error */
        fprintf(stderr, "failed to fork");
        return;
    }
    if (pid > 0) {              /* parent */
        waitpid(pid, &status, WUNTRACED);
        return;
    }
    if(wt) {                    /* child */
        execvp(cmd[0], cmd);
    }

    ppid = fork();
    if(ppid < 0) {              /* fork error */
        fprintf(stderr, "failed to fork in the child process");
        _exit(1);
    }
    if (ppid > 0) {             /* child */
        _exit(0);
    }

    execvp(cmd[0], cmd);        /* grandchild */
}

char* normpath(char* path) {
    char* real = malloc((PATH_MAX + 1)*sizeof(char));
    char* ret = realpath(path, real);
    if (ret) {
        return real;
    }

    fprintf(stderr, "Cannot find realpath: %s", path);
    return NULL;
}

/**
 * Return:
 *   0 - valid
 *   1 - invalid
 */
int check(char* path, struct str_list *excludes) {
    struct str_node* exclude = excludes->head;
    while (exclude) {
        if (strcmp(path, exclude->str) == 0) {
            return 1;
        }
        exclude = exclude->next;
    }
    return 0;
}

int main(int argc, char* argv[]) {
    extern char *optarg;
    extern int optind;

    struct wchopt* opt = malloc(sizeof(struct wchopt));
    opt->wt = 1;
    opt->once = 1;
    opt->cmd = NULL;
    opt->excludes = str_list_create();
    opt->watchlist = str_list_create();
    // opt->dir = ".";

    int cmdsize;
    struct str_node* fnode;
    char* exclude;
    char* wf;
    int c;
    static char usage[] = "Usage: %s [-01wW] [-f paths] [-x exclude] command\n\n"
        "Options:\n"
        "  -h           Show this help message\n"
        "  -1           Run the command only once even if mulitple events occured at the same time. DEFAULT\n"
        "  -0           Disalbe -1\n"
        "  -w           Wait for the last command to exit. DEFAULT.\n"
        "  -W           Do not wait the last command.\n"
        "  -f=paths     Watch dir/file. DEFAULT is the current directory.\n"
        "  -F=paths     Files and directories to ignore. You can specify multiple paths.\n";

    while ((c = getopt(argc, argv, "h01wWf:F:")) != -1) {
        switch(c) {
        case 'h':
            fprintf(stderr, usage, argv[0]);
            exit(1);
        case '1':
            opt->once = 1;
            break;
        case '0':
            opt->once = 0;
            break;
        case 'w':
            opt->wt = 1;
            break;
        case 'W':
            opt->wt = 0;
            break;
        case 'f':
            optind--;           /* NOW argv[optind] == optarg */
            for (; optind < argc; optind++) {
                // current token is an option flag
                if (argv[optind][0] == '-') {
                    break;
                }
                if ((wf = normpath(argv[optind]))) {
                    str_list_append(opt->watchlist, wf);
                }
            }
            break;
        case 'F':
            // capture mutliple arguments so that wildcard matching can be used
            optind--;           /* NOW argv[optind] == optarg */
            for (; optind < argc; optind++) {
                // current token is an option flag
                if (argv[optind][0] == '-') {
                    break;
                }
                if ((exclude = normpath(argv[optind]))) {
                    str_list_append(opt->excludes, exclude);
                }
            }
            break;
        case '?':
            fprintf(stderr, usage, argv[0]);
            exit(1);
        }
    }
    if ((optind+1) > argc) {
        fprintf(stderr, usage, argv[0]);
        exit(1);
    }

    cmdsize = argc - optind + 1; /* include the last NULL */
    opt->cmd = malloc(cmdsize * sizeof(char*));
    opt->cmd[cmdsize - 1] = NULL;
    for (int i = 0; i < cmdsize - 1; i++) {
        opt->cmd[i] = strdup(argv[optind]);
        optind++;
    }

    if (!opt->watchlist->head) {
        str_list_append(opt->watchlist, normpath("."));
    }

    run(opt);
}
