#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <stdarg.h>

#include <fcntl.h>
#include <unistd.h>
#include <termios.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/file.h>
#include <sys/ioctl.h>

#ifndef CRTSCTS
#define CRTSCTS 020000000000
#endif

#define VERSION           "2.1.0"
#define BUILD_DATE        __DATE__ " " __TIME__

#define RX_BUFFER_SIZE    4096
#define MAX_RESPONSE_SIZE 16384
#define SELECT_TIMEOUT_S  3
#define SELECT_TIMEOUT_US 0
#define TEST_MESSAGE      "RISC-V ACT UART TEST: Hello from M-Mode!\r\n"
#define MAX_OPEN_RETRIES  3

static int verbose_mode = 0;
static FILE *log_file   = NULL;

typedef struct {
    unsigned long   bytes_sent;
    unsigned long   bytes_received;
    unsigned int    tx_errors;
    unsigned int    rx_errors;
    unsigned int    timeouts;
    struct timespec start_time;
    struct timespec end_time;
} uart_stats_t;

static uart_stats_t stats = {0};

static volatile sig_atomic_t keep_running    = 1;
static volatile sig_atomic_t termios_saved   = 0;
static int                   global_fd       = -1;
static struct termios         global_old_tio;

static void print_statistics(void);

static void cleanup_handler(int sig)
{
    (void)sig;
    keep_running = 0;
    if (global_fd >= 0) {
        if (termios_saved)
            tcsetattr(global_fd, TCSANOW, &global_old_tio);
        const char msg[] = "\n[INFO] Signal received - cleaning up...\n";
        { ssize_t _r = write(STDERR_FILENO, msg, sizeof(msg) - 1); (void)_r; }
        print_statistics();
    }
}

static void log_print(const char *fmt, ...)
{
    va_list args;

    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);

    if (log_file) {
        va_start(args, fmt);
        vfprintf(log_file, fmt, args);
        va_end(args);
        fflush(log_file);
    }
}

static void hex_dump(const uint8_t *data, size_t len, const char *prefix)
{
    size_t i;
    if (!verbose_mode) return;

    log_print("%s ", prefix);
    for (i = 0; i < len; i++) {
        log_print("%02X ", data[i]);
        if ((i + 1) % 16 == 0 && i + 1 < len)
            log_print("\n%s   ", prefix);
    }
    log_print("\n");
}

static uint16_t calculate_crc16(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFF;
    size_t   i;
    int      j;

    for (i = 0; i < len; i++) {
        crc ^= data[i];
        for (j = 0; j < 8; j++) {
            if (crc & 1) crc = (crc >> 1) ^ 0xA001;
            else         crc >>= 1;
        }
    }
    return crc;
}

static void check_modem_lines(int fd)
{
    int flags = 0;
    if (!verbose_mode) return;
    if (ioctl(fd, TIOCMGET, &flags) < 0) {
        fprintf(stderr, "[WARN] TIOCMGET failed: %s\n", strerror(errno));
        return;
    }
    log_print("[MODEM] CTS: %s  DSR: %s  DCD: %s  RI: %s\n",
              (flags & TIOCM_CTS) ? "ON" : "OFF",
              (flags & TIOCM_DSR) ? "ON" : "OFF",
              (flags & TIOCM_CAR) ? "ON" : "OFF",
              (flags & TIOCM_RI)  ? "ON" : "OFF");
}

static void print_statistics(void)
{
    double elapsed_ms = 0.0;

    if (stats.start_time.tv_sec > 0 && stats.end_time.tv_sec > 0) {
        elapsed_ms = (stats.end_time.tv_sec  - stats.start_time.tv_sec)  * 1000.0
                   + (stats.end_time.tv_nsec - stats.start_time.tv_nsec) / 1000000.0;
    }

    log_print("\n========== UART Statistics ==========\n");
    log_print("Bytes sent:       %lu\n",  stats.bytes_sent);
    log_print("Bytes received:   %lu\n",  stats.bytes_received);
    log_print("Transmit errors:  %u\n",   stats.tx_errors);
    log_print("Receive errors:   %u\n",   stats.rx_errors);
    log_print("Timeouts:         %u\n",   stats.timeouts);
    if (elapsed_ms > 0.0) {
        double throughput = (stats.bytes_sent + stats.bytes_received)
                            / (elapsed_ms / 1000.0);
        log_print("Throughput:       %.2f B/s\n", throughput);
        log_print("Elapsed time:     %.2f ms\n",  elapsed_ms);
    }
    log_print("=====================================\n");
}

static int parse_uart_params(const char *s,
                              int        *data_bits,
                              char       *parity,
                              int        *stop_bits)
{
    if (!s || strlen(s) != 3) {
        fprintf(stderr, "[ERROR] UART params must be 3 characters, e.g. '8N1'\n");
        return -1;
    }

    *data_bits = s[0] - '0';
    if (*data_bits < 5 || *data_bits > 8) {
        fprintf(stderr, "[ERROR] data_bits must be 5-8, got '%c'\n", s[0]);
        return -1;
    }

    *parity = (char)toupper((unsigned char)s[1]);
    if (*parity != 'N' && *parity != 'E' && *parity != 'O') {
        fprintf(stderr, "[ERROR] parity must be N/E/O, got '%c'\n", s[1]);
        return -1;
    }

    *stop_bits = s[2] - '0';
    if (*stop_bits != 1 && *stop_bits != 2) {
        fprintf(stderr, "[ERROR] stop_bits must be 1 or 2, got '%c'\n", s[2]);
        return -1;
    }

    return 0;
}

static speed_t baud_to_termios(int baud)
{
    switch (baud) {
        case 50:      return B50;
        case 75:      return B75;
        case 110:     return B110;
        case 134:     return B134;
        case 150:     return B150;
        case 200:     return B200;
        case 300:     return B300;
        case 600:     return B600;
        case 1200:    return B1200;
        case 1800:    return B1800;
        case 2400:    return B2400;
        case 4800:    return B4800;
        case 9600:    return B9600;
        case 19200:   return B19200;
        case 38400:   return B38400;
        case 57600:   return B57600;
        case 115200:  return B115200;
        case 230400:  return B230400;
        case 460800:  return B460800;
        case 921600:  return B921600;
        default:
            fprintf(stderr,
                    "[ERROR] Unsupported baud rate: %d\n"
                    "        Valid: 9600 19200 38400 57600 115200 230400 460800 921600\n",
                    baud);
            return B0;
    }
}

static int uart_open(const char    *device,
                     int            baud,
                     int            data_bits,
                     char           parity,
                     int            stop_bits,
                     int            hw_flow_control,
                     struct termios *old_tio)
{
    int            fd;
    struct termios tio;
    speed_t        speed;

    speed = baud_to_termios(baud);
    if (speed == B0) return -1;

    if (data_bits < 5 || data_bits > 8) {
        fprintf(stderr, "[ERROR] data_bits must be 5-8, got %d\n", data_bits);
        return -1;
    }
    if (parity != 'N' && parity != 'E' && parity != 'O') {
        fprintf(stderr, "[ERROR] parity must be N/E/O, got '%c'\n", parity);
        return -1;
    }
    if (stop_bits != 1 && stop_bits != 2) {
        fprintf(stderr, "[ERROR] stop_bits must be 1 or 2, got %d\n", stop_bits);
        return -1;
    }

    /*
     * O_RDWR   : bidirectional access
     * O_NOCTTY : don't become the controlling terminal
     * O_NDELAY : don't block on open waiting for DCD
     */
    fd = open(device, O_RDWR | O_NOCTTY | O_NDELAY);
    if (fd < 0) {
        fprintf(stderr,
                "[ERROR] Cannot open '%s': %s\n"
                "        Check path and permissions "
                "(hint: sudo usermod -aG dialout $USER)\n",
                device, strerror(errno));
        return -1;
    }

    /* Exclusive lock: prevent two processes using the same port */
    if (flock(fd, LOCK_EX | LOCK_NB) < 0) {
        fprintf(stderr, "[ERROR] Device '%s' is already in use\n", device);
        close(fd);
        return -1;
    }

    /* Switch back to blocking mode now that open succeeded */
    if (fcntl(fd, F_SETFL, 0) < 0) {
        fprintf(stderr, "[ERROR] fcntl: %s\n", strerror(errno));
        close(fd);
        return -1;
    }

    /* Save original settings so we can restore them on exit */
    if (tcgetattr(fd, old_tio) < 0) {
        fprintf(stderr, "[ERROR] tcgetattr: %s\n", strerror(errno));
        close(fd);
        return -1;
    }
    termios_saved = 1;

    memset(&tio, 0, sizeof(tio));

    /* CREAD: enable receiver; CLOCAL: ignore modem control lines */
    tio.c_cflag = CREAD | CLOCAL;

    /* Character size */
    tio.c_cflag &= ~CSIZE;
    switch (data_bits) {
        case 5:  tio.c_cflag |= CS5; break;
        case 6:  tio.c_cflag |= CS6; break;
        case 7:  tio.c_cflag |= CS7; break;
        default: tio.c_cflag |= CS8; break;
    }

    /* Parity */
    switch (parity) {
        case 'E': tio.c_cflag |=  PARENB; tio.c_cflag &= ~PARODD; break;
        case 'O': tio.c_cflag |=  PARENB; tio.c_cflag |=  PARODD; break;
        default:  tio.c_cflag &= ~PARENB;                          break;
    }

    /* Stop bits */
    if (stop_bits == 2) tio.c_cflag |=  CSTOPB;
    else                tio.c_cflag &= ~CSTOPB;

    /* Hardware flow control (RTS/CTS) */
    if (hw_flow_control) {
        tio.c_cflag |= CRTSCTS;
        printf("[INFO] Hardware flow control (RTS/CTS) enabled\n");
    } else {
        tio.c_cflag &= ~CRTSCTS;
    }

    /*
     * Raw input: discard framing/parity error bytes only (IGNPAR).
     * XON/XOFF, CR->LF translation and all other transforms are off.
     */
    tio.c_iflag = IGNPAR;

    /* Raw output: no post-processing */
    tio.c_oflag = 0;

    /*
     * No canonical mode, no echo, no signal generation.
     * read() delivers bytes as they arrive without any line buffering.
     */
    tio.c_lflag = 0;

    /*
     * VMIN=0 / VTIME=0: read() returns immediately with whatever is
     * in the buffer (or 0 if empty). Timeout is handled by select().
     */
    tio.c_cc[VMIN]  = 0;
    tio.c_cc[VTIME] = 0;

    if (cfsetispeed(&tio, speed) < 0 || cfsetospeed(&tio, speed) < 0) {
        fprintf(stderr, "[ERROR] cfsetispeed/cfsetospeed: %s\n", strerror(errno));
        close(fd);
        return -1;
    }

    /*
     * TCSAFLUSH: wait for pending output to drain, discard unread
     * input, then apply the new settings atomically.
     */
    if (tcsetattr(fd, TCSAFLUSH, &tio) < 0) {
        fprintf(stderr, "[ERROR] tcsetattr: %s\n", strerror(errno));
        close(fd);
        return -1;
    }

    log_print("[INFO] Opened '%s' — %d baud %d%c%d%s\n",
              device, baud, data_bits, parity, stop_bits,
              hw_flow_control ? " RTS/CTS" : "");

    check_modem_lines(fd);
    return fd;
}

static int uart_open_with_retry(const char    *device,
                                 int            baud,
                                 int            data_bits,
                                 char           parity,
                                 int            stop_bits,
                                 int            hw_flow_control,
                                 struct termios *old_tio,
                                 int            max_retries)
{
    int fd;
    int attempt;

    for (attempt = 1; attempt <= max_retries; attempt++) {
        fd = uart_open(device, baud, data_bits, parity, stop_bits,
                       hw_flow_control, old_tio);
        if (fd >= 0) return fd;

        if (attempt < max_retries) {
            log_print("[WARN] Open failed — retry %d/%d in 1 second...\n",
                      attempt, max_retries);
            sleep(1);
        }
    }

    fprintf(stderr, "[ERROR] Could not open '%s' after %d attempt(s)\n",
            device, max_retries);
    return -1;
}

static int uart_send(int fd, const char *data, size_t len)
{
    size_t   sent = 0;
    ssize_t  n;
    size_t   dump_len;
    uint16_t crc;

    clock_gettime(CLOCK_MONOTONIC, &stats.start_time);

    while (sent < len) {
        n = write(fd, data + sent, len - sent);
        if (n < 0) {
            if (errno == EINTR) continue;
            fprintf(stderr, "[ERROR] write() failed: %s\n", strerror(errno));
            stats.tx_errors++;
            return -1;
        }
        sent += (size_t)n;
    }

    /* Block until all bytes have left the hardware transmit FIFO */
    if (tcdrain(fd) < 0)
        fprintf(stderr, "[WARN] tcdrain() failed: %s\n", strerror(errno));

    stats.bytes_sent += sent;

    log_print("[TX  ] %zu byte(s): %.*s", sent, (int)sent, data);

    dump_len = (sent > 64) ? 64 : sent;
    hex_dump((const uint8_t *)data, dump_len, "[HEX]");

    if (verbose_mode) {
        crc = calculate_crc16((const uint8_t *)data, sent);
        log_print("[CRC ] CRC-16 of TX payload: 0x%04X\n", crc);
    }

    return 0;
}

static int uart_recv_and_validate(int fd, const char *expected_response,
                                   int timeout_sec)
{
    char           rx_buf[RX_BUFFER_SIZE];
    char          *full_response;
    size_t         total         = 0;
    unsigned int   timeout_count = 0;
    fd_set         read_fds;
    struct timeval timeout;
    int            sel_ret;
    ssize_t        n;
    size_t         dump_len;
    int            ret           = 0;

    full_response = calloc(1, MAX_RESPONSE_SIZE);
    if (!full_response) {
        fprintf(stderr, "[ERROR] Out of memory for receive buffer\n");
        return -1;
    }

    log_print("[INFO] Waiting up to %d second(s) for incoming data...\n",
              timeout_sec);

    while (keep_running && total < MAX_RESPONSE_SIZE - 1) {

        /* Re-initialise each iteration — Linux modifies both in place */
        FD_ZERO(&read_fds);
        FD_SET(fd, &read_fds);
        timeout.tv_sec  = timeout_sec;
        timeout.tv_usec = SELECT_TIMEOUT_US;

        sel_ret = select(fd + 1, &read_fds, NULL, NULL, &timeout);

        if (sel_ret < 0) {
            if (errno == EINTR) continue;
            fprintf(stderr, "[ERROR] select() failed: %s\n", strerror(errno));
            stats.rx_errors++;
            ret = -1;
            goto done;
        }

        if (sel_ret == 0) {
            timeout_count++;
            stats.timeouts++;
            if (total == 0) {
                if (timeout_count >= 2) {
                    fprintf(stderr, "[ERROR] Timeout — no response received\n");
                    ret = -1;
                    goto done;
                }
                continue;
            }
            log_print("[INFO] Receive timeout — no more data.\n");
            break;
        }

        if (!FD_ISSET(fd, &read_fds))
            continue;

        n = read(fd, rx_buf, sizeof(rx_buf) - 1);

        if (n > 0) {
            timeout_count = 0;
            stats.bytes_received += (unsigned long)n;

            rx_buf[n] = '\0';

            size_t space = MAX_RESPONSE_SIZE - total - 1;
            size_t copy  = ((size_t)n < space) ? (size_t)n : space;
            memcpy(full_response + total, rx_buf, copy);
            total += copy;
            full_response[total] = '\0';

            log_print("[RX  ] %zd byte(s): %.*s",
                      n, (int)(n > 64 ? 64 : n), rx_buf);
            if (n > 64) log_print("...");
            log_print("\n");

            dump_len = (size_t)n > 64 ? 64 : (size_t)n;
            hex_dump((const uint8_t *)rx_buf, dump_len, "[HEX]");
            fflush(stdout);

            if (expected_response && strstr(full_response, expected_response)) {
                log_print("[INFO] Validated — expected response received\n");
                clock_gettime(CLOCK_MONOTONIC, &stats.end_time);
                goto done;
            }

        } else if (n == 0) {
            log_print("[INFO] EOF — device closed the connection\n");
            break;
        } else {
            if (errno == EAGAIN || errno == EINTR) continue;
            fprintf(stderr, "[ERROR] read() failed: %s\n", strerror(errno));
            stats.rx_errors++;
            ret = -1;
            goto done;
        }
    }

    clock_gettime(CLOCK_MONOTONIC, &stats.end_time);

    if (expected_response && !strstr(full_response, expected_response)) {
        fprintf(stderr, "[ERROR] Expected response not found: '%s'\n",
                expected_response);
        stats.rx_errors++;
        ret = -1;
    }

done:
    free(full_response);
    return ret;
}

static void uart_close(int fd, const struct termios *old_tio)
{
    if (old_tio && termios_saved)
        tcsetattr(fd, TCSANOW, old_tio);
    close(fd);
    global_fd = -1;
    log_print("[INFO] Port closed.\n");
}

/*
 * Usage: uart_interface [-v] [--log FILE] <device> [baud] [format] [timeout] [expected]
 *
 *   -v / --verbose   hex dumps, CRC output, modem line status
 *   --log FILE       append all output to FILE in addition to stdout
 *   device           /dev/ttyUSB0, /dev/ttyS0, /dev/ttyAMA0 ...
 *   baud             baud rate (default: 115200)
 *   format           8N1, 7E2, 8O1 etc. (default: 8N1)
 *   timeout          receive timeout in seconds (default: 3)
 *   expected         optional substring to validate in the response
 */
int main(int argc, char *argv[])
{
    const char *device;
    int         baud            = 115200;
    int         data_bits       = 8;
    char        parity          = 'N';
    int         stop_bits       = 1;
    int         hw_flow_control = 0;
    int         timeout_sec     = SELECT_TIMEOUT_S;
    const char *expected        = NULL;
    int         fd;
    int         i;

    log_print("%s version %s (built %s)\n", "uart_interface", VERSION, BUILD_DATE);
    fflush(stdout);

    /* Parse flags: -v / --verbose and --log <file> */
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
            verbose_mode = 1;
            printf("[INFO] Verbose mode enabled\n");
            for (int j = i; j < argc - 1; j++) argv[j] = argv[j + 1];
            argc--; i--;
        } else if (strcmp(argv[i], "--log") == 0 && i + 1 < argc) {
            log_file = fopen(argv[i + 1], "a");
            if (!log_file) {
                fprintf(stderr, "[WARN] Cannot open log file '%s': %s\n",
                        argv[i + 1], strerror(errno));
            } else {
                printf("[INFO] Logging to '%s'\n", argv[i + 1]);
            }
            for (int j = i; j < argc - 2; j++) argv[j] = argv[j + 2];
            argc -= 2; i--;
        }
    }

    if (argc < 2) {
        fprintf(stderr,
                "Usage: %s [-v] [--log FILE] <device> [baud] [format] [timeout] [expected]\n"
                "  -v / --verbose   hex dumps, CRC, modem line status\n"
                "  --log FILE       also write output to FILE\n"
                "  Examples:\n"
                "    %s /dev/ttyUSB0\n"
                "    %s /dev/ttyUSB0 115200 8N1\n"
                "    %s -v /dev/ttyS0 9600 7E2 5 \"ACK\"\n"
                "  format:  <data_bits><parity><stop_bits>  e.g. 8N1 7E2 8O2\n"
                "  timeout: receive wait in seconds (default %d)\n",
                argv[0], argv[0], argv[0], argv[0], SELECT_TIMEOUT_S);
        return EXIT_FAILURE;
    }

    device = argv[1];

    if (argc >= 3) {
        baud = atoi(argv[2]);
        if (baud <= 0) {
            fprintf(stderr, "[ERROR] Invalid baud rate: '%s'\n", argv[2]);
            return EXIT_FAILURE;
        }
    }

    if (argc >= 4) {
        if (parse_uart_params(argv[3], &data_bits, &parity, &stop_bits) != 0)
            return EXIT_FAILURE;
    }

    if (argc >= 5) {
        timeout_sec = atoi(argv[4]);
        if (timeout_sec <= 0) {
            fprintf(stderr, "[ERROR] Timeout must be a positive integer, got '%s'\n",
                    argv[4]);
            return EXIT_FAILURE;
        }
    }

    if (argc >= 6)
        expected = argv[5];

    signal(SIGINT,  cleanup_handler);
    signal(SIGTERM, cleanup_handler);

    fd = uart_open_with_retry(device, baud, data_bits, parity, stop_bits,
                              hw_flow_control, &global_old_tio,
                              MAX_OPEN_RETRIES);
    if (fd < 0) {
        if (log_file) fclose(log_file);
        return EXIT_FAILURE;
    }

    global_fd = fd;

    if (uart_send(fd, TEST_MESSAGE, strlen(TEST_MESSAGE)) < 0) {
        uart_close(fd, &global_old_tio);
        print_statistics();
        if (log_file) fclose(log_file);
        return EXIT_FAILURE;
    }

    if (uart_recv_and_validate(fd, expected, timeout_sec) < 0) {
        uart_close(fd, &global_old_tio);
        print_statistics();
        if (log_file) fclose(log_file);
        return EXIT_FAILURE;
    }

    uart_close(fd, &global_old_tio);
    print_statistics();
    if (log_file) fclose(log_file);
    return EXIT_SUCCESS;
}
