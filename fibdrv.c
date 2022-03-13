#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kdev_t.h>
#include <linux/kernel.h>
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
    n->len = 1;
    n->max_len = digits_num;
    n->digits = kmalloc(digits_num * sizeof(unsigned long long), GFP_KERNEL);
    if (!n->digits)
        return NULL;
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

    for (int i = 0; i < x->len; i++) {
        unsigned long long result = x->digits[i] - borrow;
        borrow = borrow > x->digits[i] || result < y->digits[i];
        result -= y->digits[i];
        output->digits[i] = result;
    }

    output->len = x->len;
    while (output->len > 1 && output->digits[output->len - 1] == 0)
        output->len--;
}

static void add_BigN(struct BigN *output,
                     const struct BigN *x,
                     const struct BigN *y)
{
    int max_len = max(x->len, y->len);
    unsigned long long carry = 0;

    for (int i = 0; i < max_len; i++) {
        unsigned long long result = x->digits[i] + carry;
        carry = x->digits[i] > ~carry || result > ~y->digits[i];
        result += y->digits[i];
        output->digits[i] = result;
    }

    if (output->len < output->max_len) {
        output->digits[max_len] = carry;
        output->len = max_len + carry;
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
            unsigned __int128 result =
                (unsigned __int128) x->digits[i] * y->digits[j];

            /* add the lower of result */
            carry->digits[i + j + 1] +=
                output->digits[i + j] > (unsigned long long) (~result & ~0ULL);
            output->digits[i + j] += result & ~0ULL;

            /* add the upper of result */
            result >>= 64;
            if (i + j + 2 < output->max_len)
                carry->digits[i + j + 2] +=
                    output->digits[i + j + 1] > (unsigned long long) ~result;
            output->digits[i + j + 1] += result;
        }
    }
    add_BigN(output, output, carry);
    output->len -= output->digits[output->len - 1] == 0;
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

static void constant_add_BigN(struct BigN *output, unsigned long long c)
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

static void constant_sub_BigN(struct BigN *output, unsigned long long c)
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
        (k & (i << 1)) ? constant_add_BigN(a, 2) : constant_sub_BigN(a, 2);
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

/*static struct BigN *fib_sequence_iterative(long long k)
{
    int digits_num = 2 + k * 7 / 640;
    struct BigN *f_n = init_BigN(digits_num);
    struct BigN *f_n_next = init_BigN(digits_num);

    if(!(f_n && f_n_next))
        return NULL;

    f_n->digits[0] = 0ULL;
    f_n_next->digits[0] = 1ULL;

    for (long long i = 2; i <= k; i++) {
        add_BigN(f_n, f_n, f_n_next);
        swap_BigN(f_n, f_n_next);
    }
    free_BigN(f_n);

    if(k == 0)
        f_n_next->digits[0] = 0ULL;
    return f_n_next;
}*/

/*static unsigned __int128 fib_sequence_fast(long long k)
{
    unsigned __int128 a = 0, b = 1;
    unsigned __int128 aa = 0, bb = 0;
    unsigned __int128 plus_two = 2, minus_two = -plus_two;
    long long i = 1LL << (fls(k) - 1);

    if(k < 2)
        return k;

    while(i) {
        aa = a*a;
        bb = b*b;
        a = (bb - aa) * 2 - aa + ((k & i << 1) ? 2 : -2);
        b = aa + bb;
        //unsigned __int128 t1 = a * (2 * b - a);
        //unsigned __int128 t2 = a * a + b * b;
        //a = t1; b = t2; // m *= 2
        if (k & i) {
            aa = a + b; // m++
            a = b; b = aa;
        }
        i >>= 1;
    }

    return a;
}

static unsigned __int128 fib_sequence_iterative(long long k)
{
    unsigned __int128 f_n = 0;
    unsigned __int128 f_n_next = 1;
    unsigned __int128 f_k = k;

    for (long long i = 2; i <= k; i++) {
        f_k = f_n + f_n_next;
        f_n = f_n_next;
        f_n_next = f_k;
    }
    return f_k;
}*/

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
    struct BigN *fib_result = fib_sequence_fast(*offset);
    if (!fib_result)
        return -1;

    size_t sz = fib_result->len * sizeof(unsigned long long);
    if (size < sz) {
        free_BigN(fib_result);
        /*printk("Buffer is too small",sz);*/
        return -1;
    }

    int remain = copy_to_user(buf, fib_result->digits, sz);
    if (remain) {
        free_BigN(fib_result);
        /*printk("Copy fail",sz);*/
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
    return 1;
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
