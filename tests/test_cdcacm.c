// CDC ACM interface discovery, against descriptor layouts real devices use:
// an STM32 Betaflight VCP, a device whose data interface comes first, one
// with no control interface, and a CH340 (vendor class, must not match).
#include "cdcacm.h"

#include "check.h"

static struct libusb_endpoint_descriptor ep(unsigned char addr, unsigned char type) {
    struct libusb_endpoint_descriptor e = { 0 };
    e.bEndpointAddress = addr;
    e.bmAttributes = type;
    return e;
}

static struct libusb_interface_descriptor itf(int num, int cls, int sub, const struct libusb_endpoint_descriptor *eps,
                                              int neps) {
    struct libusb_interface_descriptor d = { 0 };
    d.bInterfaceNumber = (unsigned char)num;
    d.bInterfaceClass = (unsigned char)cls;
    d.bInterfaceSubClass = (unsigned char)sub;
    d.endpoint = eps;
    d.bNumEndpoints = (unsigned char)neps;
    return d;
}

int main(void) {
    int comm, data;
    unsigned char in = 0, out = 0;

    // STM32 VCP (Betaflight): 0 = CDC control (interrupt IN), 1 = data (bulk OUT 0x01, IN 0x81)
    struct libusb_endpoint_descriptor stm_ctrl[] = { ep(0x82, LIBUSB_TRANSFER_TYPE_INTERRUPT) };
    struct libusb_endpoint_descriptor stm_data[] = { ep(0x01, LIBUSB_TRANSFER_TYPE_BULK),
                                                    ep(0x81, LIBUSB_TRANSFER_TYPE_BULK) };
    struct libusb_interface_descriptor stm_alt[] = { itf(0, 0x02, 0x02, stm_ctrl, 1), itf(1, 0x0a, 0, stm_data, 2) };
    struct libusb_interface stm_if[] = { { &stm_alt[0], 1 }, { &stm_alt[1], 1 } };
    struct libusb_config_descriptor stm = { 0 };
    stm.bNumInterfaces = 2;
    stm.interface = stm_if;
    CHECK(cdcacm_find(&stm, &comm, &data, &in, &out) == 0, "STM32 VCP not found");
    CHECK(comm == 0 && data == 1, "STM32: comm %d data %d, want 0 and 1", comm, data);
    CHECK(in == 0x81 && out == 0x01, "STM32: in %02x out %02x", in, out);

    // Data interface listed first, other endpoint numbers
    struct libusb_interface_descriptor rev_alt[] = { itf(1, 0x0a, 0, stm_data, 2), itf(0, 0x02, 0x02, stm_ctrl, 1) };
    struct libusb_interface rev_if[] = { { &rev_alt[0], 1 }, { &rev_alt[1], 1 } };
    struct libusb_config_descriptor rev = stm;
    rev.interface = rev_if;
    CHECK(cdcacm_find(&rev, &comm, &data, &in, &out) == 0 && comm == 0 && data == 1, "order shouldn't matter");

    // No control interface at all: still usable
    struct libusb_interface solo_if[] = { { &stm_alt[1], 1 } };
    struct libusb_config_descriptor solo = { 0 };
    solo.bNumInterfaces = 1;
    solo.interface = solo_if;
    CHECK(cdcacm_find(&solo, &comm, &data, &in, &out) == 0, "data-only device not found");
    CHECK(comm == -1 && data == 1, "data-only: comm %d data %d", comm, data);

    // CH340: one vendor-class interface (0xff) with bulk + interrupt -> not CDC
    struct libusb_endpoint_descriptor ch_eps[] = { ep(0x82, LIBUSB_TRANSFER_TYPE_BULK), ep(0x02, LIBUSB_TRANSFER_TYPE_BULK),
                                                  ep(0x81, LIBUSB_TRANSFER_TYPE_INTERRUPT) };
    struct libusb_interface_descriptor ch_alt[] = { itf(0, 0xff, 1, ch_eps, 3) };
    struct libusb_interface ch_if[] = { { &ch_alt[0], 1 } };
    struct libusb_config_descriptor ch = { 0 };
    ch.bNumInterfaces = 1;
    ch.interface = ch_if;
    CHECK(cdcacm_find(&ch, &comm, &data, &in, &out) == -1, "CH340 must not look like CDC ACM");

    // Data class but only interrupt endpoints: not usable
    struct libusb_endpoint_descriptor bad_eps[] = { ep(0x81, LIBUSB_TRANSFER_TYPE_INTERRUPT),
                                                   ep(0x01, LIBUSB_TRANSFER_TYPE_INTERRUPT) };
    struct libusb_interface_descriptor bad_alt[] = { itf(1, 0x0a, 0, bad_eps, 2) };
    struct libusb_interface bad_if[] = { { &bad_alt[0], 1 } };
    struct libusb_config_descriptor bad = solo;
    bad.interface = bad_if;
    CHECK(cdcacm_find(&bad, &comm, &data, &in, &out) == -1, "data interface without bulk endpoints");

    TEST_MAIN_END("test_cdcacm");
}
