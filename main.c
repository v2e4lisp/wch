#include <sys/types.h>
#include <sys/dir.h>
#include <sys/event.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#define PATH_MAX                 1024

struct str_list* str_list_create();
void str_list_append(struct str_list* slist, const char* p);
void str_list_free(struct str_list* slist);

void walk(char* path, struct str_list* excludes, struct str_list* flist);
void run(char* dir, struct str_list* excludes, struct str_list* commands);
int check(char* path, struct str_list *excludes);
char* strip_dot_slash(char* path);

struct str_list {
    int size;
    struct str_node *head;
    struct str_node *tail;
};

struct str_node {
    char *str;
    struct str_node *next;
};

struct str_list* str_list_create() {
    struct str_list *slist = malloc(sizeof(struct str_list));
    slist->size = 0;
    slist->head = NULL;
    slist->tail = NULL;
    return slist;
}

void str_list_append(struct str_list* slist, const char* p) {
    struct str_node* snode = malloc(sizeof(struct str_node));
    snode->str = strdup(p);
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

void str_list_free(struct str_list *slist) {
    struct str_node* snode = slist->head;
    while (snode) {
        struct str_node* next = snode->next;
        free(snode->str);
        free(snode);
        snode = next;
    }

    free(slist);
}

void walk(char* path, struct str_list* excludes, struct str_list* flist) {
    if (check(path, excludes) != 0) {
        return;
    }

    DIR *dirp;
    struct dirent * dent;
    int len;
    char newpath[PATH_MAX];
    char* p;

    len = strlen(path);
    strcpy(newpath, path);
    newpath[len++] = '/';

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
        p = strip_dot_slash(newpath);
        if (dent->d_type == DT_DIR) {
            walk(p, excludes, flist);
        }
        if (dent->d_type == DT_REG) {
            if (check(p, excludes) == 0) {
                str_list_append(flist, p);
            }
        }
    }

    closedir(dirp);
}

void run(char* dir, struct str_list* excludes, struct str_list* commands) {
    struct str_list* flist;
    struct str_node* fnode;
    int kq;                     /* kqueue */
    int nev;                    /* number of events */
    int fd;
    flist = str_list_create();
    walk(".", excludes, flist);
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
        fd = open(fnode->str, O_RDONLY);
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

        for (int i = 0; i < nev; i++) {
            if (events[i].fflags & NOTE_WRITE) {
                char *fname = (char*) events[i].udata;
                printf("file changed: %s\n", fname);
            }
        }
    }
}

char* strip_dot_slash(char* path) {
    if (strlen(path) > 2 && path[0] == '.' && path[1] == '/') {
        path = path + 2;
    }
    return path;
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

    struct str_list* commands = str_list_create();
    struct str_list* excludes = str_list_create();
    struct str_node* fnode;
    int err;
    char* dir = ".";
    int c;
    char* exclude;
    static char usage[] = "usage: %s [-d dir] [-x exclude] command\n";

    while ((c = getopt(argc, argv, "d:x:")) != -1) {
        switch(c) {
        case 'd':
            dir = optarg;
            break;
        case 'x':
            // capture mutliple arguments so that wildcard matching can be used
            optind--;           /* NOW argv[optind] == optarg */
            for (; optind < argc; optind++) {
                // current token is an option flag
                if (argv[optind][0] == '-') {
                    break;
                }
                exclude = strip_dot_slash(argv[optind]);
                str_list_append(excludes, exclude);
            }
            break;
        case '?':
            err = 1;
            break;
        }
    }

    if (err) {
        fprintf(stderr, usage, argv[0]);
        exit(1);
    }
    if ((optind+1) > argc) {
        fprintf(stderr, "%s: no command.\n", argv[0]);
        fprintf(stderr, usage, argv[0]);
        exit(1);
    }

    if (optind < argc) {
        for (; optind < argc; optind++) {
            str_list_append(commands, argv[optind]);
        }
    }

    run(dir, excludes, commands);
}
