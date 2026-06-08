#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include "game.h"

#define MAX_WORD 256
#define BUF      512

typedef struct {
    secret_word_t  words[2];    
    int            fds[2];    
    volatile int   done[2];   
    pthread_mutex_t lock;
    pthread_cond_t  cond;
} game_t;

static game_t G;

static int net_readline(int fd, char *buf, size_t cap)
{
    size_t i = 0;
    for (;;) {
        char c;
        ssize_t n = recv(fd, &c, 1, 0);
        if (n <= 0) return -1;
        if (c == '\n') break;
        if (c != '\r' && i < cap - 1) buf[i++] = c;
    }
    buf[i] = '\0';
    return (int)i;
}

static void net_sendline(int fd, const char *fmt, ...)
{
    char buf[BUF];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, (int)sizeof(buf) - 2, fmt, ap);
    va_end(ap);
    if (n <= 0 || n >= (int)sizeof(buf) - 1) return;
    buf[n]   = '\n';
    buf[n+1] = '\0';
    send(fd, buf, (size_t)(n + 1), MSG_NOSIGNAL);
}

static void fmt_masked(const secret_word_t *w, char *out, size_t cap)
{
    size_t j = 0;
    for (size_t i = 0; i < w->word_length && j + 1 < cap; i++) {
        char c = '_';
        if (secret_word_letter_at(w, i, &c) == SECRET_WORD_LETTER_REVEALED)
            out[j++] = c;
        else
            out[j++] = '_';
    }
    out[j] = '\0';
}

static void fmt_wrong(const secret_word_t *w, char *out, size_t cap)
{
    size_t pos  = 0;
    int    first = 1;
    for (char c = 'a'; c <= 'z'; c++) {
        if (!letter_set_contains(w->incorrect_guesses, c)) continue;
        if (!first && pos + 3 < cap) { out[pos++] = ','; out[pos++] = ' '; }
        if (pos + 1 < cap) out[pos++] = c;
        first = 0;
    }
    out[pos] = '\0';
}

static void *player_thread(void *arg)
{
    int id   = (int)(intptr_t)arg;
    int fd   = G.fds[id];
    secret_word_t *word = &G.words[id];
    char masked[MAX_WORD], wrong[BUF], line[BUF];

    fmt_masked(word, masked, sizeof masked);
    fmt_wrong (word, wrong,  sizeof wrong);
    net_sendline(fd, "WORD %s",  masked);
    net_sendline(fd, "WRONG %s", wrong);

    /* cikul na poznavane */
    while (!secret_word_is_solved(word)) {
        if (net_readline(fd, line, sizeof line) < 0) goto done;
        char guess = line[0];
        secret_word_guess(word, guess);

        fmt_masked(word, masked, sizeof masked);
        fmt_wrong (word, wrong,  sizeof wrong);
        net_sendline(fd, "WORD %s",  masked);
        net_sendline(fd, "WRONG %s", wrong);
    }

    net_sendline(fd, "SOLVED");

    /* sinhronizaciq na dvata igracha */
    pthread_mutex_lock(&G.lock);
    G.done[id] = 1;
    pthread_cond_broadcast(&G.cond);
    while (!G.done[0] || !G.done[1])
        pthread_cond_wait(&G.cond, &G.lock);
    pthread_mutex_unlock(&G.lock);

    /* rezultat */
    {
        int my_err  = (int)secret_word_incorrect_guess_count(&G.words[id]);
        int opp_err = (int)secret_word_incorrect_guess_count(&G.words[1 - id]);
        const char *verdict =
            (my_err < opp_err) ? "WIN"  :
            (my_err > opp_err) ? "LOSE" : "TIE";

        char my_wrong[BUF], opp_wrong[BUF];
        fmt_wrong(&G.words[id],     my_wrong,  sizeof my_wrong);
        fmt_wrong(&G.words[1 - id], opp_wrong, sizeof opp_wrong);

        net_sendline(fd, "FINAL %s", verdict);
        net_sendline(fd, "YOUR %s",  my_wrong);
        net_sendline(fd, "OPP %s",   opp_wrong);
    }

done:
    close(fd);
    return NULL;
}

int main(int argc, char *argv[])
{
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <port>\n", argv[0]);
        return 1;
    }

    signal(SIGPIPE, SIG_IGN);

    int port = atoi(argv[1]);
    int srv  = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) { perror("socket"); return 1; }

    int yes = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons((uint16_t)port);

    if (bind(srv, (struct sockaddr *)&addr, sizeof addr) < 0) {
        perror("bind"); close(srv); return 1;
    }
    if (listen(srv, 2) < 0) {
        perror("listen"); close(srv); return 1;
    }

    printf("Listening on %d...\n", port);
    fflush(stdout);

    pthread_mutex_init(&G.lock, NULL);
    pthread_cond_init (&G.cond, NULL);
    G.done[0] = 0;
    G.done[1] = 0;


    char raw[2][MAX_WORD];
    for (int i = 0; i < 2; ) {
        G.fds[i] = accept(srv, NULL, NULL);
        if (G.fds[i] < 0) { perror("accept"); close(srv); return 1; }
        if (net_readline(G.fds[i], raw[i], sizeof raw[i]) < 0) {
            net_sendline(G.fds[i], "ERR connection error");
            close(G.fds[i]);
            continue;   
        }
        i++;
    }
    close(srv);   

    
    if (!secret_word_init_from_c_string(&G.words[0], raw[1])) {
        net_sendline(G.fds[1], "ERR invalid word");
        close(G.fds[0]); close(G.fds[1]);
        return 0;
    }
    if (!secret_word_init_from_c_string(&G.words[1], raw[0])) {
        net_sendline(G.fds[0], "ERR invalid word");
        secret_word_free(&G.words[0]);
        close(G.fds[0]); close(G.fds[1]);
        return 0;
    }

    net_sendline(G.fds[0], "OK");
    net_sendline(G.fds[1], "OK");

    pthread_t t0, t1;
    pthread_create(&t0, NULL, player_thread, (void *)(intptr_t)0);
    pthread_create(&t1, NULL, player_thread, (void *)(intptr_t)1);
    pthread_join(t0, NULL);
    pthread_join(t1, NULL);

    secret_word_free(&G.words[0]);
    secret_word_free(&G.words[1]);
    pthread_mutex_destroy(&G.lock);
    pthread_cond_destroy (&G.cond);
    return 0;
}
