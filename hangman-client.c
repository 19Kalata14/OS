#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define BUF 512

static int sock;

static int net_readline(char *buf, size_t cap)
{
    size_t i = 0;
    for (;;) {
        char c;
        ssize_t n = recv(sock, &c, 1, 0);
        if (n <= 0) return -1;
        if (c == '\n') break;
        if (c != '\r' && i < cap - 1) buf[i++] = c;
    }
    buf[i] = '\0';
    return (int)i;
}

static void net_sendline(const char *s)
{
    char buf[BUF];
    int n = snprintf(buf, sizeof buf, "%s\n", s);
    if (n > 0) send(sock, buf, (size_t)n, MSG_NOSIGNAL);
}

/* dali bukvata e veche poznata*/
static int already_guessed(char guess, const char *masked, const char *wrong)
{
    char g = (char)tolower((unsigned char)guess);
    for (const char *p = masked; *p; p++)
        if (*p != '_' && (char)tolower((unsigned char)*p) == g)
            return 1;
    for (const char *p = wrong; *p; p++)
        if (isalpha((unsigned char)*p) && (char)tolower((unsigned char)*p) == g)
            return 1;
    return 0;
}

int main(int argc, char *argv[])
{
    if (argc != 4) {
        fprintf(stderr, "Usage: %s <host> <port> <word>\n", argv[0]);
        return 1;
    }
    const char *host = argv[1];
    int         port = atoi(argv[2]);
    const char *word = argv[3];

    signal(SIGPIPE, SIG_IGN);

    setvbuf(stdout, NULL, _IONBF, 0);

    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) { perror("socket"); return 1; }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) <= 0) {
        fprintf(stderr, "Invalid address: %s\n", host);
        return 1;
    }
    if (connect(sock, (struct sockaddr *)&addr, sizeof addr) < 0) {
        perror("connect"); return 1;
    }

    /* prashtame dumata */
    net_sendline(word);

    char line[BUF];
    if (net_readline(line, sizeof line) < 0) {
        fprintf(stderr, "Server disconnected.\n");
        return 1;
    }
    if (strncmp(line, "OK", 2) != 0) {
        fprintf(stderr, "Server: %s\n", line);
        close(sock);
        return 1;
    }

    /* cikul na igra */
    char masked[BUF] = "";
    char wrong[BUF]  = "";

    for (;;) {
        if (net_readline(line, sizeof line) < 0) goto result;
        if (strncmp(line, "WORD ", 5) == 0) {
            strncpy(masked, line + 5, sizeof masked - 1);
            masked[sizeof masked - 1] = '\0';
        }

        if (net_readline(line, sizeof line) < 0) goto result;
        if (strncmp(line, "WRONG ", 6) == 0) {
            strncpy(wrong, line + 6, sizeof wrong - 1);
            wrong[sizeof wrong - 1] = '\0';
        }

        printf("Word: %s\n", masked);
        printf("Incorrect guesses: %s\n", wrong);

        if (strchr(masked, '_') == NULL) break;

        char guess = 0;
        for (;;) {
            char inp[BUF];
            if (fgets(inp, sizeof inp, stdin) == NULL) goto result;

            size_t len = strlen(inp);
            while (len > 0 && (inp[len-1] == '\n' || inp[len-1] == '\r'))
                inp[--len] = '\0';

            if (len != 1) {
                printf("Please enter only one letter.\n");
                continue;
            }
            guess = inp[0];

            if (already_guessed(guess, masked, wrong)) {
                printf("Already guessed '%c', try again.\n", guess);
                continue;
            }
            break;
        }

        char g[3] = { guess, '\0' };
        net_sendline(g);
    }

    /* chakane na rezultat */
result:;
    char verdict[32]    = "";
    char my_wrong[BUF]  = "";
    char opp_wrong[BUF] = "";
    int  got = 0;  

    while (got != 7) {
        if (net_readline(line, sizeof line) < 0) break;
        if (strncmp(line, "FINAL ", 6) == 0) {
            strncpy(verdict, line + 6, sizeof verdict - 1);
            verdict[sizeof verdict - 1] = '\0';
            got |= 1;
        } else if (strncmp(line, "YOUR ", 5) == 0) {
            strncpy(my_wrong, line + 5, sizeof my_wrong - 1);
            my_wrong[sizeof my_wrong - 1] = '\0';
            got |= 2;
        } else if (strncmp(line, "OPP ", 4) == 0) {
            strncpy(opp_wrong, line + 4, sizeof opp_wrong - 1);
            opp_wrong[sizeof opp_wrong - 1] = '\0';
            got |= 4;
        }
    }

    if      (strcmp(verdict, "WIN")  == 0) printf("YOU WIN! :)\n");
    else if (strcmp(verdict, "LOSE") == 0) printf("You Lose! :(\n");
    else                                    printf("Tie :/\n");

    printf("Your incorrect guesses: %s\n",      my_wrong);
    printf("Opponent's incorrect guesses: %s\n", opp_wrong);

    close(sock);
    return 0;
}
