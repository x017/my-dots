/* Download remote MPRIS artwork off the UI and notification worker threads.
 * Files live in a private, short-lived directory and never enter the repo. */
typedef struct { unsigned int serial; char url[1024]; } ArtRequest;
typedef struct { unsigned int serial; int ok; char path[512]; } ArtReply;
static void album_worker(int fd) {
    char dir[] = "/tmp/cui-bar-art-XXXXXX", output[256]; ArtRequest req; ArtReply reply;
    signal(SIGCHLD, SIG_DFL); signal(SIGPIPE, SIG_IGN);
    if (!mkdtemp(dir)) { close(fd); _exit(1); }
    for (;;) {
        struct stat st; ssize_t n = recv(fd, &req, sizeof req, 0);
        if (n != sizeof req) break;
        memset(&reply, 0, sizeof reply); reply.serial = req.serial;
        req.url[sizeof req.url - 1] = 0;
        if (!strncmp(req.url, "https://", 8)) {
            char *argv[] = {"curl", "--silent", "--show-error", "--fail", "--proto", "=https", "--max-redirs", "0",
                            "--connect-timeout", "3", "--max-time", "6", "--max-filesize", "2097152", "--output", reply.path,
                            "--", req.url, NULL};
            snprintf(reply.path, sizeof reply.path, "%s/%u", dir, req.serial);
            if (!run_command(argv, NULL, output, sizeof output, 7000) && !stat(reply.path, &st) &&
                S_ISREG(st.st_mode) && st.st_size > 0 && st.st_size <= 2 * 1024 * 1024) reply.ok = 1;
            else unlink(reply.path);
        }
        if (send(fd, &reply, sizeof reply, MSG_NOSIGNAL) != sizeof reply) break;
    }
    {
        DIR *d = opendir(dir); struct dirent *entry;
        if (d) { while ((entry = readdir(d))) {
            char path[512];
            if (entry->d_name[0] == '.') continue;
            if (snprintf(path, sizeof path, "%s/%s", dir, entry->d_name) < (int)sizeof path) unlink(path);
        } closedir(d); }
        rmdir(dir);
    }
    close(fd); _exit(0);
}
