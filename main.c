/*
 * Sends a raw MongoDB wire-protocol OP_MSG {ping: 1, $db: "admin"}
 * over a TCP socket and return exit code 0 if everything is fine.
 *
 * Usage:
 *   ./mongoping [host] [port]
 *   ./mongoping                 -> defaults to 127.0.0.1:27017
 *   ./mongoping 10.0.0.5 27017
 *
 * Exit codes:
 *   0  -> connected, sent, got a reply
 *   1  -> usage / arg error
 *   2  -> could not resolve/connect
 *   3  -> send failed
 *   4  -> no reply / recv failed (DB likely down or not responding)
 *   5  -> server reply not understood
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netdb.h>

/* The raw OP_MSG payload for {ping: 1, "$db": "admin"} constructed by hand:
 *
 *   header:
 *     int32 messageLength = 51
 *     int32 requestID     = 1
 *     int32 responseTo    = 0
 *     int32 opCode        = 2013 (OP_MSG)
 *   body:
 *     uint32 flagBits = 0
 *     section kind byte = 0 (body section)
 *     BSON document:
 *       int32 length = 30
 *       \x10 "ping\0" int32(1)      -- int32 field "ping": 1
 *       \x02 "$db\0" int32(6) "admin\0"  -- string field "$db": "admin"
 *       \x00                        -- document terminator
 */
static const unsigned char PING_MSG[] = {
    0x33, 0x00, 0x00, 0x00, /* messageLength = 51           */
    0x01, 0x00, 0x00, 0x00, /* requestID = 1                */
    0x00, 0x00, 0x00, 0x00, /* responseTo = 0                */
    0xdd, 0x07, 0x00, 0x00, /* opCode = 2013 (OP_MSG)        */
    0x00, 0x00, 0x00, 0x00, /* flagBits = 0                  */
    0x00, /* section kind = 0 (body)       */
    0x1e, 0x00, 0x00, 0x00, /* BSON doc length = 30           */
    0x10, 'p', 'i', 'n', 'g', 0x00, 0x01, 0x00, 0x00, 0x00, /* ping: 1 (int32) */
    0x02, '$', 'd', 'b', 0x00, 0x06, 0x00, 0x00, 0x00,
    'a', 'd', 'm', 'i', 'n', 0x00, /* $db: "admin" (string) */
    0x00 /* BSON doc terminator          */
};

int main(int argc, char **argv) {
    const char *host = (argc > 1) ? argv[1] : "127.0.0.1";
    const char *port = (argc > 2) ? argv[2] : "27017";

    struct addrinfo hints, *res, *rp;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    int gai_err = getaddrinfo(host, port, &hints, &res);
    if (gai_err != 0) {
        fprintf(stderr, "getaddrinfo(%s:%s) failed: %s\n", host, port, gai_strerror(gai_err));
        return 2;
    }

    int fd = -1;
    for (rp = res; rp != NULL; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd == -1)
            continue;

        struct timeval timeout = {.tv_sec = 3, .tv_usec = 0};
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

        if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0)
            break; /* connected */

        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);

    if (fd == -1) {
        fprintf(stderr, "connect to %s:%s failed: %s\n", host, port, strerror(errno));
        return 2;
    }

    ssize_t sent = send(fd, PING_MSG, sizeof(PING_MSG), 0);
    if (sent != (ssize_t) sizeof(PING_MSG)) {
        fprintf(stderr, "send failed (sent %zd of %zu bytes): %s\n",
                sent, sizeof(PING_MSG), strerror(errno));
        close(fd);
        return 3;
    }

    unsigned char buf[4096];
    ssize_t n = recv(fd, buf, sizeof(buf), 0);
    close(fd);

    if (n <= 0) {
        fprintf(stderr, "no reply from %s:%s (%s)\n",
                host, port, (n == 0) ? "connection closed" : strerror(errno));
        return 4;
    }

    /* Quick sanity check: the reply's responseTo (bytes 8-11) should be 1,
     * matching our requestID, and opCode (bytes 12-15) should be 2013. */
    if (n >= 16) {
        int32_t response_to, opcode;
        memcpy(&response_to, buf + 8, 4);
        memcpy(&opcode, buf + 12, 4);
        if (response_to == 1 && opcode == 2013) {
            printf("ok\n");
            return 0;
        }
    }

    return 5;
}
