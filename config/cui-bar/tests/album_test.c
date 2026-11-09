#define main cui_bar_program_main
#include "../bar.c"
#undef main
#include <assert.h>
int main(void) {
    char dir[] = "/tmp/cui-album-test-XXXXXX", path[1024], envpath[2048];
    int pair[2], status; pid_t child; ArtRequest req = {.serial = 17}; ArtReply reply; FILE *f; struct pollfd p;
    assert(mkdtemp(dir)); snprintf(path, sizeof path, "%s/curl", dir);
    f = fopen(path, "w"); assert(f);
    fputs("#!/bin/sh\nwhile [ $# -gt 0 ]; do if [ \"$1\" = --output ]; then shift; printf test > \"$1\"; exit 0; fi; shift; done\nexit 1\n", f);
    fclose(f); assert(!chmod(path, 0700));
    snprintf(envpath, sizeof envpath, "%s:/bin:/usr/bin", dir); assert(!setenv("PATH", envpath, 1));
    assert(!socketpair(AF_UNIX, SOCK_SEQPACKET, 0, pair));
    child = fork(); assert(child >= 0);
    if (!child) { close(pair[0]); album_worker(pair[1]); }
    close(pair[1]); p = (struct pollfd){.fd = pair[0], .events = POLLIN};
    strcpy(req.url, "https://example.org/art.png");
    assert(send(pair[0], &req, sizeof req, 0) == sizeof req);
    assert(poll(&p, 1, 5000) == 1 && recv(pair[0], &reply, sizeof reply, 0) == sizeof reply);
    assert(reply.ok && reply.serial == 17 && !access(reply.path, F_OK));
    req.serial++; strcpy(req.url, "http://example.org/unsafe.png");
    assert(send(pair[0], &req, sizeof req, 0) == sizeof req);
    assert(poll(&p, 1, 5000) == 1 && recv(pair[0], &reply, sizeof reply, 0) == sizeof reply);
    assert(!reply.ok && reply.serial == 18);
    close(pair[0]); assert(waitpid(child, &status, 0) == child);
    unlink(path); rmdir(dir); puts("asynchronous HTTPS album artwork worker passed"); return 0;
}
