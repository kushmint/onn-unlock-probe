/*
 * onn-unlock-probe: non-destructive AVB TA lock-state compatibility check.
 *
 * This talks to the same OP-TEE "AVB" trusted application and command
 * numbers used by the onn 4K Pro 2026 (jarvis2 / jarvis2_row) unlock tool:
 *   command 2 (VALUE_OUTPUT) -> read secure_lock_state (0 = unlocked, 1 = locked)
 *   command 3 (VALUE_INPUT)  -> write secure_lock_state
 *
 * Unlike that tool, this one is NOT a one-shot permanent unlock. It is a
 * read-only check by default. Only with --toggle-test does it attempt a
 * round trip: write 0 (unlock), read back, immediately write 1 (re-lock),
 * read back, and report whether the state was actually restored.
 *
 * It never reboots the device and never schedules a reboot. On Android,
 * ro.boot.flash.locked and verified-boot behavior are latched at boot, so
 * the risk window from an incomplete restore is "until the next reboot",
 * not immediate. Still: if the final read-back after --toggle-test is not
 * back to 1 ("locked"), DO NOT REBOOT the device - treat it as unlocked.
 *
 * This tool makes no assumption that the target device is a known model.
 * It is meant to be run on devices the unlock-tool author does not own
 * (onn Plus, onn 4K Pro 2024, etc.) purely to find out whether the same
 * TA/UUID/command protocol exists there at all, and whether the bit is
 * reversible on that hardware. A failure to open the TEE device or the TA
 * session is itself a useful, non-destructive result: "not compatible".
 */
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/tee.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/system_properties.h>
#include <unistd.h>

#define PROBE_TA_CMD_READ 2u
#define PROBE_TA_CMD_WRITE 3u
#define PROBE_ATTR_VALUE_OUTPUT 2u
#define PROBE_ATTR_VALUE_INPUT 1u

struct reply {
    int transport_rc;
    int saved_errno;
    uint32_t result;
    uint32_t origin;
    uint64_t value;
};

static int g_fd = -1;
static uint32_t g_session;
static int g_session_open = 0;

static void print_prop(const char *name) {
    char value[PROP_VALUE_MAX] = {0};
    __system_property_get(name, value);
    printf("{\"stage\":\"property\",\"name\":\"%s\",\"value\":\"%s\"}\n", name, value);
}

static struct reply invoke(uint32_t command, uint32_t attr, uint64_t value) {
    struct reply out = {-1, 0, UINT32_MAX, UINT32_MAX, UINT64_MAX};
    size_t size = sizeof(struct tee_ioctl_invoke_arg) + 4 * sizeof(struct tee_ioctl_param);
    struct tee_ioctl_invoke_arg *request = calloc(1, size);
    if (!request) return out;
    request->func = command;
    request->session = g_session;
    request->num_params = 4;
    request->ret = UINT32_MAX;
    request->params[0].attr = attr;
    request->params[0].a = value;
    struct tee_ioctl_buf_data buffer = {.buf_ptr = (uintptr_t)request, .buf_len = size};
    printf("{\"stage\":\"request\",\"command\":%u,\"attr\":%u,\"input_value\":%" PRIu64 "}\n",
           command, attr, value);
    out.transport_rc = ioctl(g_fd, TEE_IOC_INVOKE, &buffer);
    out.saved_errno = out.transport_rc < 0 ? errno : 0;
    out.result = request->ret;
    out.origin = request->ret_origin;
    out.value = request->params[0].a;
    printf("{\"stage\":\"response\",\"command\":%u,\"rc\":%d,\"errno\":%d,"
           "\"tee_result\":\"0x%08x\",\"origin\":%u,\"value\":%" PRIu64 "}\n",
           command, out.transport_rc, out.saved_errno, out.result, out.origin, out.value);
    free(request);
    return out;
}

static void cleanup_and_exit(int code, const char *outcome) __attribute__((noreturn));
static void cleanup_and_exit(int code, const char *outcome) {
    if (g_session_open) {
        struct tee_ioctl_close_session_arg end = {.session = g_session};
        int rc = ioctl(g_fd, TEE_IOC_CLOSE_SESSION, &end);
        printf("{\"stage\":\"close\",\"rc\":%d,\"errno\":%d}\n", rc, rc < 0 ? errno : 0);
    }
    if (g_fd >= 0) close(g_fd);
    alarm(0);
    printf("{\"stage\":\"finished\",\"outcome\":\"%s\",\"exit_code\":%d,\"rebooted\":false}\n",
           outcome, code);
    exit(code);
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);

    int toggle_test = 0;
    if (argc == 2 && strcmp(argv[1], "--toggle-test") == 0) {
        toggle_test = 1;
    } else if (argc != 1) {
        fprintf(stderr,
                "usage: probe [--toggle-test]\n"
                "  (no args)      read-only: report the current AVB lock-state bit\n"
                "  --toggle-test  additionally attempt a reversible 1->0->1 round trip\n"
                "This never reboots the device.\n");
        return 2;
    }

    if (getuid() != 2000 || geteuid() != 2000) {
        fprintf(stderr, "Must be run as the adb shell user (uid 2000): adb shell <path>\n");
        return 3;
    }

    printf("{\"stage\":\"start\",\"toggle_test_requested\":%s}\n", toggle_test ? "true" : "false");
    print_prop("ro.product.device");
    print_prop("ro.product.model");
    print_prop("ro.product.manufacturer");
    print_prop("ro.build.fingerprint");
    print_prop("ro.boot.flash.locked");
    print_prop("ro.boot.verifiedbootstate");
    print_prop("ro.boot.bootloader");
    print_prop("ro.product.cpu.abi");

    alarm(toggle_test ? 20 : 8);

    g_fd = open("/dev/tee0", O_RDWR | O_CLOEXEC);
    if (g_fd < 0) {
        printf("{\"stage\":\"tee_open_failed\",\"errno\":%d}\n", errno);
        cleanup_and_exit(10, "no_tee_device");
    }

    struct tee_ioctl_version_data version = {0};
    if (ioctl(g_fd, TEE_IOC_VERSION, &version) < 0) {
        printf("{\"stage\":\"tee_version_failed\",\"errno\":%d}\n", errno);
        cleanup_and_exit(11, "tee_version_ioctl_failed");
    }
    printf("{\"stage\":\"tee_version\",\"impl_id\":%u,\"gen_caps\":%u}\n",
           version.impl_id, version.gen_caps);
    if (version.impl_id != TEE_IMPL_ID_OPTEE || !(version.gen_caps & TEE_GEN_CAP_GP)) {
        cleanup_and_exit(12, "not_optee_gp");
    }

    size_t open_size = sizeof(struct tee_ioctl_open_session_arg) + 4 * sizeof(struct tee_ioctl_param);
    struct tee_ioctl_open_session_arg *open_arg = calloc(1, open_size);
    if (!open_arg) cleanup_and_exit(13, "allocation_failed");
    const uint8_t uuid[16] = {
        0x02, 0x3f, 0x8f, 0x1a, 0x29, 0x2a, 0x43, 0x2b,
        0x8f, 0xc4, 0xde, 0x84, 0x71, 0x35, 0x80, 0x67
    };
    memcpy(open_arg->uuid, uuid, sizeof(uuid));
    open_arg->clnt_login = TEE_IOCTL_LOGIN_PUBLIC;
    open_arg->num_params = 4;
    open_arg->ret = UINT32_MAX;
    struct tee_ioctl_buf_data buffer = {.buf_ptr = (uintptr_t)open_arg, .buf_len = open_size};
    int result = ioctl(g_fd, TEE_IOC_OPEN_SESSION, &buffer);
    printf("{\"stage\":\"open\",\"rc\":%d,\"errno\":%d,\"tee_result\":\"0x%08x\",\"origin\":%u}\n",
           result, result < 0 ? errno : 0, open_arg->ret, open_arg->ret_origin);
    if (result < 0 || open_arg->ret != 0) {
        free(open_arg);
        cleanup_and_exit(14, "ta_not_present_or_session_refused");
    }
    g_session = open_arg->session;
    g_session_open = 1;
    free(open_arg);

    /* Step 1: read-only. This alone is zero-risk and always happens. */
    struct reply initial = invoke(PROBE_TA_CMD_READ, PROBE_ATTR_VALUE_OUTPUT, UINT32_MAX);
    if (initial.transport_rc != 0 || initial.result != 0) {
        cleanup_and_exit(20, "initial_read_failed");
    }
    if (initial.value != 0 && initial.value != 1) {
        cleanup_and_exit(21, "initial_value_unexpected");
    }
    if (initial.value == 0) {
        /* Already unlocked: nothing to toggle, nothing to restore, no write issued. */
        cleanup_and_exit(0, "already_unlocked_no_write_attempted");
    }
    /* initial.value == 1 (locked) from here on. */
    if (!toggle_test) {
        cleanup_and_exit(0, "locked_read_only_rerun_with_toggle_test_to_probe_further");
    }

    /* Step 2: attempt to flip to unlocked. */
    struct reply unlock_write = invoke(PROBE_TA_CMD_WRITE, PROBE_ATTR_VALUE_INPUT, 0);
    if (unlock_write.transport_rc != 0 || unlock_write.result != 0) {
        cleanup_and_exit(30, "unlock_write_failed_state_presumed_unchanged");
    }
    struct reply mid = invoke(PROBE_TA_CMD_READ, PROBE_ATTR_VALUE_OUTPUT, UINT32_MAX);
    if (mid.transport_rc != 0 || mid.result != 0) {
        cleanup_and_exit(31, "CRITICAL_midstate_read_failed_state_unknown_do_not_reboot");
    }
    if (mid.value != 0) {
        /* The write ioctl reported success but the bit didn't move. Nothing to restore. */
        cleanup_and_exit(32, "unlock_write_had_no_effect_state_still_locked");
    }

    /* Step 3: immediately attempt to restore the original (locked) state. */
    struct reply relock_write = invoke(PROBE_TA_CMD_WRITE, PROBE_ATTR_VALUE_INPUT, 1);
    if (relock_write.transport_rc != 0 || relock_write.result != 0) {
        cleanup_and_exit(40, "CRITICAL_relock_write_failed_device_left_unlocked_do_not_reboot");
    }
    struct reply final_state = invoke(PROBE_TA_CMD_READ, PROBE_ATTR_VALUE_OUTPUT, UINT32_MAX);
    if (final_state.transport_rc != 0 || final_state.result != 0) {
        cleanup_and_exit(41, "CRITICAL_final_read_failed_state_unknown_do_not_reboot");
    }
    if (final_state.value == 1) {
        cleanup_and_exit(0, "toggle_test_passed_state_fully_restored");
    }
    if (final_state.value == 0) {
        cleanup_and_exit(42, "CRITICAL_relock_ineffective_bit_appears_one_way_device_left_unlocked_do_not_reboot");
    }
    cleanup_and_exit(43, "CRITICAL_final_value_unexpected_do_not_reboot");
}
