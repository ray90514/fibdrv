#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kdev_t.h>
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>

MODULE_LICENSE("Dual MIT/GPL");
MODULE_AUTHOR("National Cheng Kung University, Taiwan");
MODULE_DESCRIPTION("Fibonacci engine driver");
MODULE_VERSION("0.1");

#define DEV_FIBONACCI_NAME "fibonacci"

/* MAX_LENGTH is set to 92 because
 * ssize_t can't fit the number > 92
 */
#define MAX_LENGTH 500000
/* log2(f(n)) = 0.6942 * n - 1.16 */
#define BIGN_SIZE (2 + MAX_LENGTH * 7 / 640)

static dev_t fib_dev = 0;
static struct cdev *fib_cdev;
static struct class *fib_class;
static ktime_t kt;
static int mode = 2;
static DEFINE_MUTEX(fib_mutex);

struct BigN {
    int len;
    int max_len;
    unsigned long long *digits;
};

static struct BigN *init_BigN(int digits_num)
{
    struct BigN *n = kmalloc(sizeof(struct BigN), GFP_KERNEL);
    if (!n)
        return NULL;

    n->digits = kmalloc(digits_num * sizeof(unsigned long long), GFP_KERNEL);
    if (!n->digits) {
        kfree(n);
        return NULL;
    }

    n->len = 1;
    n->max_len = digits_num;
    for (int i = 0; i < digits_num; i++)
        n->digits[i] = 0ULL;
    return n;
}

static void free_BigN(struct BigN *n)
{
    kfree(n->digits);
    kfree(n);
}

static inline void swap_BigN(struct BigN *dst, struct BigN *src)
{
    struct BigN temp = *dst;
    *dst = *src;
    *src = temp;
}

static void sub_BigN(struct BigN *output,
                     const struct BigN *x,
                     const struct BigN *y)
{
    unsigned long long borrow = 0;
    int i = 0;

    while (i < y->len) {
        unsigned long long result = x->digits[i] - borrow;
        borrow = borrow > x->digits[i] || result < y->digits[i];
        result -= y->digits[i];
        output->digits[i] = result;
        i++;
    }

    while (borrow && i < x->len) {
        output->digits[i] = x->digits[i] - borrow;
        borrow = x->digits[i] == 0;
        i++;
    }

    while (i < x->len) {
        output->digits[i] = x->digits[i];
        i++;
    }

    output->len = x->len;
    while (output->len > 1 && output->digits[output->len - 1] == 0)
        output->len--;
}

static void add_BigN(struct BigN *output,
                     const struct BigN *x,
                     const struct BigN *y)
{
    const struct BigN *max, *min;
    if (x->len > y->len) {
        max = x;
        min = y;
    } else {
        max = y;
        min = x;
    }
    unsigned long long carry = 0;
    int i = 0;

    while (i < min->len) {
        unsigned long long result = max->digits[i] + carry;
        carry = max->digits[i] > ~carry || result > ~min->digits[i];
        result += min->digits[i];
        output->digits[i] = result;
        i++;
    }

    while (carry && i < max->len) {
        output->digits[i] = max->digits[i] + carry;
        carry = max->digits[i] > ~carry;
        i++;
    }

    while (i < max->len) {
        output->digits[i] = max->digits[i];
        i++;
    }

    output->len = max->len;
    if (output->len < output->max_len) {
        output->digits[output->len] = carry;
        output->len += carry;
    }
}

static void mul_BigN(struct BigN *output,
                     const struct BigN *x,
                     const struct BigN *y,
                     struct BigN *carry)
{
    output->len = x->len + y->len;
    carry->len = output->len;

    if (output->len > output->max_len) {
        output->digits[0] = 0;
        output->len = 0;
        return;
    }

    for (int i = 0; i < output->len; i++) {
        output->digits[i] = 0;
        carry->digits[i] = 0;
    }

    for (int i = 0; i < x->len; i++) {
        for (int j = 0; j < y->len; j++) {
            unsigned long long high, low;
            __asm__("mulq %3"
                    : "=a"(low), "=d"(high)
                    : "%0"(x->digits[i]), "rm"(y->digits[j]));
            /* add the lower of result */
            carry->digits[i + j + 1] += output->digits[i + j] > ~low;
            output->digits[i + j] += low;

            /* add the upper of result */
            if (i + j + 2 < output->max_len)
                carry->digits[i + j + 2] += output->digits[i + j + 1] > ~high;
            output->digits[i + j + 1] += high;
        }
    }
    add_BigN(output, output, carry);
    output->len -= output->digits[output->len - 1] == 0;
}

static void mul_BigN_test(struct BigN *output,
                          const struct BigN *x,
                          const struct BigN *y,
                          struct BigN *buf,
                          struct BigN *carry)
{
    if (x->len == 1 || y->len == 1) {
        unsigned long long low = 0;
        for (int i = 0; i < y->len; i++) {
            for (int j = 0; j < x->len; j++) {
                unsigned long long result, high;
                __asm__("mulq %3"
                        : "=a"(result), "=d"(high)
                        : "%0"(x->digits[j]), "rm"(y->digits[i]));
                high += result > ~low;
                output->digits[i + j] = result + low;
                low = high;
            }
        }
        output->len = x->len + y->len;
        output->digits[output->len - 1] = low;
        output->len -= output->digits[output->len - 1] == 0;
        return;
    }
    if (x->len <= 8 || y->len <= 8) {
        mul_BigN(output, x, y, buf);
        return;
    }

    int m = (max(x->len, y->len) + 1) / 2;
    struct BigN buf_right = *buf;
    struct BigN buf_left = *buf;
    struct BigN x_left = *x;
    struct BigN x_right = *x;
    struct BigN y_left = *y;
    struct BigN y_right = *y;
    x_left.digits += m;
    y_left.digits += m;
    x_left.len -= m;
    y_left.len -= m;
    x_right.len = m;
    y_right.len = m;

    /* caculate for y_left + y_right */
    add_BigN(&buf_right, &y_left, &y_right);
    buf_left.digits += buf_right.len;
    buf_left.max_len -= buf_right.len;

    /* caculate for x_left + x_right */
    add_BigN(&buf_left, &x_left, &x_right);

    /* caculate for middle, rigt, left and whole product */
    output->digits += m;
    output->max_len -= m;
    buf->digits += buf_left.len + buf_right.len;
    mul_BigN_test(output, &buf_left, &buf_right, buf, carry);

    buf->digits = buf_right.digits + m * 2;
    buf_right.max_len = m * 2;
    mul_BigN_test(&buf_right, &x_right, &y_right, buf, carry);
    sub_BigN(output, output, &buf_right);
    for (int i = 0; i < m; i++)
        output->digits[i - m] = buf_right.digits[i];
    buf_right.digits += m;
    buf_right.len -= m;
    add_BigN(output, output, &buf_right);

    buf_right.digits -= m;
    mul_BigN_test(&buf_right, &x_left, &y_left, buf, carry);
    sub_BigN(output, output, &buf_right);
    output->digits += m;
    output->len -= m;
    output->max_len -= m;
    add_BigN(output, output, &buf_right);

    output->digits -= m * 2;
    output->len += m * 2;
    output->max_len += m * 2;
    buf->digits -= m * 2;
}

static void lshift_BigN(struct BigN *output)
{
    unsigned long long right = 0;
    for (int i = 0; i < output->len; i++) {
        unsigned long long left = (output->digits[i] & ~(~0ULL >> 1)) > 0;
        output->digits[i] = output->digits[i] << 1 | right;
        right = left;
    }

    if (output->len < output->max_len) {
        output->digits[output->len] = right;
        output->len += right;
    }
}

static void add_constant_BigN(struct BigN *output, unsigned long long c)
{
    unsigned long long carry = output->digits[0] > ~c;
    output->digits[0] += c;

    for (int i = 1; carry && i < output->len; i++) {
        carry = output->digits[i] > ~1ULL;
        output->digits[i]++;
    }

    if (output->len < output->max_len) {
        output->digits[output->len] = carry;
        output->len += carry;
    }
}

static void sub_constant_BigN(struct BigN *output, unsigned long long c)
{
    unsigned long long borrow = c > output->digits[0];
    output->digits[0] -= c;

    for (int i = 1; borrow && i < output->len; i++) {
        borrow = 1ULL > output->digits[i];
        output->digits[i]--;
    }

    while (output->len > 1 && output->digits[output->len - 1] == 0)
        output->len--;
}

static struct BigN *fib_sequence_fast(long long k)
{
    /* log2(f(n)) = 0.6942 * n - 1.16 */
    int digits_num = 2 + k * 7 / 640;
    struct BigN *a = init_BigN(digits_num);
    struct BigN *b = init_BigN(digits_num);
    struct BigN *aa = init_BigN(digits_num);
    struct BigN *bb = init_BigN(digits_num);
    struct BigN *carry = init_BigN(digits_num);
    unsigned long long i = 1ULL << fls(k) >> 1;

    if (!(a && b && aa && bb && carry))
        return NULL;
    a->digits[0] = 0ULL;
    b->digits[0] = 1ULL;

    while (i > 1) {
        mul_BigN(aa, a, a, carry);
        mul_BigN(bb, b, b, carry);
        sub_BigN(a, bb, aa);
        lshift_BigN(a);
        (k & (i << 1)) ? add_constant_BigN(a, 2) : sub_constant_BigN(a, 2);
        sub_BigN(a, a, aa);
        add_BigN(b, aa, bb);
        if (k & i) {
            add_BigN(a, a, b);  // m++
            swap_BigN(a, b);
        }
        i >>= 1;
    }
    /* last round */
    if (k & i) {
        mul_BigN(aa, a, a, carry);
        mul_BigN(bb, b, b, carry);
        add_BigN(a, aa, bb);
    } else {
        lshift_BigN(b);
        sub_BigN(b, b, a);
        mul_BigN(aa, b, a, carry);
        swap_BigN(aa, a);
    }

    free_BigN(b);
    free_BigN(aa);
    free_BigN(bb);
    free_BigN(carry);
    return a;
}

static struct BigN *fib_sequence_test(long long k)
{
    /* log2(f(n)) = 0.6942 * n - 1.16 */
    int digits_num = 2 + k * 7 / 640;
    struct BigN *a = init_BigN(digits_num);
    struct BigN *b = init_BigN(digits_num);
    struct BigN *aa = init_BigN(digits_num);
    struct BigN *bb = init_BigN(digits_num);
    struct BigN *carry = NULL;  // init_BigN(digits_num);
    struct BigN *buf = init_BigN(digits_num);
    unsigned long long i = 1ULL << fls(k) >> 1;

    if (!(a && b && aa && bb && buf))
        return NULL;
    a->digits[0] = 0ULL;
    b->digits[0] = 1ULL;

    while (i > 1) {
        mul_BigN_test(aa, a, a, buf, carry);
        mul_BigN_test(bb, b, b, buf, carry);
        sub_BigN(a, bb, aa);
        lshift_BigN(a);
        (k & (i << 1)) ? add_constant_BigN(a, 2) : sub_constant_BigN(a, 2);
        sub_BigN(a, a, aa);
        add_BigN(b, aa, bb);
        if (k & i) {
            add_BigN(a, a, b);  // m++
            swap_BigN(a, b);
        }
        i >>= 1;
    }
    /* last round */
    if (k & i) {
        mul_BigN_test(aa, a, a, buf, carry);
        mul_BigN_test(bb, b, b, buf, carry);
        add_BigN(a, aa, bb);

    } else {
        lshift_BigN(b);
        sub_BigN(b, b, a);
        mul_BigN_test(aa, b, a, buf, carry);
        swap_BigN(aa, a);
    }

    free_BigN(b);
    free_BigN(aa);
    free_BigN(bb);
    // free_BigN(carry);
    free_BigN(buf);
    return a;
}

static struct BigN *fib_sequence_iterative(long long k)
{
    int digits_num = 2 + k * 7 / 640;
    struct BigN *f_n_prev = init_BigN(digits_num);
    struct BigN *f_n = init_BigN(digits_num);

    if (!(f_n_prev && f_n))
        return NULL;

    f_n_prev->digits[0] = 0ULL;
    f_n->digits[0] = 1ULL;

    for (long long i = 2; i <= k; i++) {
        add_BigN(f_n_prev, f_n_prev, f_n);
        swap_BigN(f_n_prev, f_n);
    }
    if (k == 0)
        f_n->digits[0] = 0ULL;

    free_BigN(f_n_prev);
    return f_n;
}

static int fib_open(struct inode *inode, struct file *file)
{
    if (!mutex_trylock(&fib_mutex)) {
        printk(KERN_ALERT "fibdrv is in use");
        return -EBUSY;
    }
    return 0;
}

static int fib_release(struct inode *inode, struct file *file)
{
    mutex_unlock(&fib_mutex);
    return 0;
}

/* calculate the fibonacci number at given offset */
static ssize_t fib_read(struct file *file,
                        char *buf,
                        size_t size,
                        loff_t *offset)
{
    struct BigN *fib_result;

    kt = ktime_get();
    switch (mode) {
    case 1:
        fib_result = fib_sequence_iterative(*offset);
        break;
    case 2:
        fib_result = fib_sequence_fast(*offset);
        break;
    case 3:
        fib_result = fib_sequence_test(*offset);
        break;
    default:
        fib_result = fib_sequence_test(*offset);
    }
    kt = ktime_sub(ktime_get(), kt);
    if (!fib_result) {
        printk("Memory allocation fail");
        return -1;
    }

    size_t sz = fib_result->len * sizeof(unsigned long long);
    if (size < sz) {
        free_BigN(fib_result);
        printk("Buffer is too small");
        printk("%ld %ld", size, sz);
        return -1;
    }

    int remain = copy_to_user(buf, fib_result->digits, sz);
    if (remain) {
        free_BigN(fib_result);
        printk("Copy fail");
        return -1;
    }

    free_BigN(fib_result);
    return sz;
}

/* write operation is skipped */
static ssize_t fib_write(struct file *file,
                         const char *buf,
                         size_t size,
                         loff_t *offset)
{
    if (size)
        mode = size;
    return ktime_to_ns(kt);
}

static loff_t fib_device_lseek(struct file *file, loff_t offset, int orig)
{
    loff_t new_pos = 0;
    switch (orig) {
    case 0: /* SEEK_SET: */
        new_pos = offset;
        break;
    case 1: /* SEEK_CUR: */
        new_pos = file->f_pos + offset;
        break;
    case 2: /* SEEK_END: */
        new_pos = MAX_LENGTH - offset;
        break;
    }

    if (new_pos > MAX_LENGTH)
        new_pos = MAX_LENGTH;  // max case
    if (new_pos < 0)
        new_pos = 0;        // min case
    file->f_pos = new_pos;  // This is what we'll use now
    return new_pos;
}

const struct file_operations fib_fops = {
    .owner = THIS_MODULE,
    .read = fib_read,
    .write = fib_write,
    .open = fib_open,
    .release = fib_release,
    .llseek = fib_device_lseek,
};

static int __init init_fib_dev(void)
{
    int rc = 0;

    mutex_init(&fib_mutex);

    // Let's register the device
    // This will dynamically allocate the major number
    rc = alloc_chrdev_region(&fib_dev, 0, 1, DEV_FIBONACCI_NAME);

    if (rc < 0) {
        printk(KERN_ALERT
               "Failed to register the fibonacci char device. rc = %i",
               rc);
        return rc;
    }

    fib_cdev = cdev_alloc();
    if (fib_cdev == NULL) {
        printk(KERN_ALERT "Failed to alloc cdev");
        rc = -1;
        goto failed_cdev;
    }
    fib_cdev->ops = &fib_fops;
    rc = cdev_add(fib_cdev, fib_dev, 1);

    if (rc < 0) {
        printk(KERN_ALERT "Failed to add cdev");
        rc = -2;
        goto failed_cdev;
    }

    fib_class = class_create(THIS_MODULE, DEV_FIBONACCI_NAME);

    if (!fib_class) {
        printk(KERN_ALERT "Failed to create device class");
        rc = -3;
        goto failed_class_create;
    }

    if (!device_create(fib_class, NULL, fib_dev, NULL, DEV_FIBONACCI_NAME)) {
        printk(KERN_ALERT "Failed to create device");
        rc = -4;
        goto failed_device_create;
    }
    return rc;
failed_device_create:
    class_destroy(fib_class);
failed_class_create:
    cdev_del(fib_cdev);
failed_cdev:
    unregister_chrdev_region(fib_dev, 1);
    return rc;
}

static void __exit exit_fib_dev(void)
{
    mutex_destroy(&fib_mutex);
    device_destroy(fib_class, fib_dev);
    class_destroy(fib_class);
    cdev_del(fib_cdev);
    unregister_chrdev_region(fib_dev, 1);
}

module_init(init_fib_dev);
module_exit(exit_fib_dev);
