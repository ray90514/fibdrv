#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#define FIB_DEV "/dev/fibonacci"
#define MAX_10P 10000000000000000000ULL
#define MAX_LENGTH 100
#define MAX_SIZE 2 + MAX_LENGTH * 7 / 640

/*int print_fib_128 (int n, unsigned __int128 value)
{
    if (value > ~0ULL) {
        unsigned long long leading = value / MAX_10P;
        unsigned long long trail = value % MAX_10P;
        return printf("Reading from " FIB_DEV
               " at offset %d, returned the sequence "
               "%llu%llu.\n",
               n, leading, trail);
    } else {
        return printf("Reading from " FIB_DEV
               " at offset %d, returned the sequence "
               "%llu.\n",
               n, value);
    }
}*/

void print_fib_BigN(int n, unsigned long long *digits, int len)
{
    unsigned __int128 value = 0;

    printf("Reading from " FIB_DEV " at offset %d, returned the sequence ", n);
    for (int i = 0; i < len; i++) {
        value = 0;
        digits[len] = 0;
        for (int j = len - 1; j >= i; j--) {
            value = value << 64 | digits[j];
            digits[j + 1] = value / MAX_10P;
            value %= MAX_10P;
        }
        digits[i] = value;
        len += digits[len] > 0;
    }
    printf("%llu", digits[len - 1]);
    for (int i = len - 2; i >= 0; i--) {
        printf("%019llu", digits[i]);
    }
    printf(".\n");
}

/*void print_fib_BigN_hex (int n, unsigned long long *digits, int len)
{
    printf("%llx ",digits[len - 1]);
    for (int i = len - 2; i >= 0; i--) {
        printf("%019llx ", digits[i]);
    }
    printf("\n");
}*/

int main(int argc, char *argv[])
{
    long long sz;

    // unsigned __int128 buf[1];
    unsigned long long buf[MAX_SIZE];
    char write_buf[] = "testing writing";
    int offset =
        MAX_LENGTH; /* TODO: try test something bigger than the limit */

    int fd = open(FIB_DEV, O_RDWR);
    if (fd < 0) {
        perror("Failed to open character device");
        exit(1);
    }

    if (argc == 2) {
        long long n;
        sscanf(argv[1], "%lld", &n);
        lseek(fd, n, SEEK_SET);
        size_t digits_size = (2 + n / 90) * sizeof(unsigned long long);
        unsigned long long *buffer = malloc(digits_size);
        sz = read(fd, buffer, digits_size) / sizeof(unsigned long long);
        print_fib_BigN(n, buffer, sz);
        free(buffer);
        close(fd);
        return 0;
    }

    for (int i = 0; i <= offset; i++) {
        sz = write(fd, write_buf, strlen(write_buf));
        printf("Writing to " FIB_DEV ", returned the sequence %lld\n", sz);
    }

    for (int i = 0; i <= offset; i++) {
        lseek(fd, i, SEEK_SET);
        sz = read(fd, buf, sizeof(buf)) / sizeof(unsigned long long);
        print_fib_BigN(i, buf, sz);
    }

    for (int i = offset; i >= 0; i--) {
        lseek(fd, i, SEEK_SET);
        sz = read(fd, buf, sizeof(buf)) / sizeof(unsigned long long);
        print_fib_BigN(i, buf, sz);
    }

    close(fd);
    return 0;
}
