// SPDX-License-Identifier: MIT
#include <blisp.h>
#include <blisp_util.h>
#include <libserialport.h>
#include <malloc.h>
#include <stdio.h>
#include <string.h>

#ifdef __linux__
#include <linux/serial.h>
#include <sys/ioctl.h>
#endif

#define DEBUG

int32_t blisp_device_init(struct blisp_device* device, struct blisp_chip* chip)
{
    device->chip = chip;
    device->is_usb = false;
    return 0;
}

int32_t blisp_device_open(struct blisp_device* device, const char* port_name)
{
    int ret;
    struct sp_port* serial_port = NULL;

    if (port_name != NULL) {
        ret = sp_get_port_by_name(port_name, &serial_port);
        if (ret != SP_OK) {
            return -1; // TODO: Improve error codes
        }
    } else {
        if (!device->chip->usb_isp_available) {
            return -2; // Can't auto-find device due it doesn't have native USB
        }
        struct sp_port **port_list;
        ret = sp_list_ports(&port_list);
        if (ret != SP_OK) {
            return -1; // TODO: Improve error codes
        }
        for (int i = 0; port_list[i] != NULL; i++) {
            struct sp_port *port = port_list[i];

            int vid, pid;
            sp_get_port_usb_vid_pid(port, &vid, &pid);
            if (vid == 0xFFFF && pid == 0xFFFF) {
                ret = sp_get_port_by_name(sp_get_port_name(port), &serial_port);
                if (ret != SP_OK) {
                    return -1; // TODO: Improve error codes
                }
                break;
            }
        }
        sp_free_port_list(port_list);
        if (serial_port == NULL) {
            return -3; // Device not found
        }
    }

    ret = sp_open(serial_port, SP_MODE_READ_WRITE);
    if (ret != SP_OK) { // TODO: Handle not found
        return -1;
    }
    sp_set_bits(serial_port, 8);
    sp_set_parity(serial_port, SP_PARITY_NONE);
    sp_set_stopbits(serial_port, 1);
    sp_set_flowcontrol(serial_port, SP_FLOWCONTROL_NONE);

    uint32_t vid, pid;
    sp_get_port_usb_vid_pid(serial_port, &vid, &pid);
    device->is_usb = pid == 0xFFFF;
//    if (device->is_usb) {
//        device->current_baud_rate = 2000000;
//    } else {
        device->current_baud_rate = 500000;
//    }

#if 0
    int fd;
    sp_get_port_handle(serial_port, &fd);
    struct serial_struct serial;
    ioctl(fd, TIOCGSERIAL, &serial);
//    serial.flags &= ~(ASYNC_LOW_LATENCY);
    serial.flags |= ASYNC_LOW_LATENCY;
    ioctl(fd, TIOCSSERIAL, &serial);
#endif
    ret = sp_set_baudrate(serial_port, device->current_baud_rate);
    if (ret != SP_OK) {
        return -1; // TODO: Handle this
    }
    device->serial_port = serial_port;

    return 0;
}

int32_t blisp_send_command(struct blisp_device* device, uint8_t command, void* payload, uint16_t payload_size, bool add_checksum)
{
    int ret;
    struct sp_port* serial_port = device->serial_port;

    device->tx_buffer[0] = command;
    device->tx_buffer[1] = 0;
    device->tx_buffer[2] = payload_size & 0xFF;
    device->tx_buffer[3] = (payload_size >> 8) & 0xFF;
    if (add_checksum) {
        uint32_t checksum = 0;
        checksum += device->tx_buffer[2] + device->tx_buffer[3];
        for (uint16_t i = 0; i < payload_size; i++) {
            checksum += *(uint8_t*)((uint8_t*)payload + i);
        }
        device->tx_buffer[1] = checksum & 0xFF;
    }
    if (payload_size != 0) {
        memcpy(&device->tx_buffer[4], payload, payload_size);
    }
    ret = sp_blocking_write(serial_port, device->tx_buffer, 4 + payload_size, 1000);
    if (ret != (4 + payload_size)) {
        return -1;
    }
    return 0;
}

int32_t blisp_receive_response(struct blisp_device* device, bool expect_payload) {
    // TODO: Check checksum
    int ret;
    struct sp_port* serial_port = device->serial_port;
    ret = sp_blocking_read(serial_port, &device->rx_buffer[0], 2, 1000);
    if (ret < 2) {
#ifdef DEBUG
        fprintf(stderr, "Failed to receive response. (ret = %d)\n", ret);
#endif
        return -1; // TODO: Terrible
    } else if (device->rx_buffer[0] == 'O' && device->rx_buffer[1] == 'K') {
        if (expect_payload) {
            sp_blocking_read(serial_port, &device->rx_buffer[2], 2, 100); // TODO: Check if really we received the data.
            uint16_t data_length = (device->rx_buffer[3] << 8) | (device->rx_buffer[2]);
            sp_blocking_read(serial_port, &device->rx_buffer[0], data_length, 100);
            return data_length;
        }
        return 0;
    } else if (device->rx_buffer[0] == 'P' && device->rx_buffer[1] == 'D') {
        return -3; // TODO: Terrible
    } else if (device->rx_buffer[0] == 'F' && device->rx_buffer[1] == 'L') {
        sp_blocking_read(serial_port, &device->rx_buffer[2], 2, 100);
        device->error_code = (device->rx_buffer[3] << 8) | (device->rx_buffer[2]);
        return -4; // Failed
    }
#ifdef DEBUG
    fprintf(stderr, "Receive response failed... (err: %d, %d - %d)\n", ret, device->rx_buffer[0], device->rx_buffer[1]);
#endif
    return -1;
}

int32_t
blisp_device_handshake(struct blisp_device* device, bool in_ef_loader) {
    int ret;
    uint8_t handshake_buffer[600];
    struct sp_port* serial_port = device->serial_port;

    if (!in_ef_loader && !device->is_usb) {
        sp_set_rts(serial_port, SP_RTS_ON);
        sp_set_dtr(serial_port, SP_DTR_ON);
        sleep_ms(50);
        sp_set_dtr(serial_port, SP_DTR_OFF);
        sleep_ms(100);
        sp_set_rts(serial_port, SP_RTS_OFF);
        sleep_ms(50); // Wait a bit so BootROM can init
    }

    uint32_t bytes_count = device->chip->handshake_byte_multiplier * (float)device->current_baud_rate / 10.0f;
    if (bytes_count > 600) bytes_count = 600;
    memset(handshake_buffer, 'U', bytes_count);

    for (uint8_t i = 0; i < 5; i++) {
        if (!in_ef_loader) {
            if (device->is_usb) {
                sp_blocking_write(serial_port, "BOUFFALOLAB5555RESET\0\0", 22,
                                  100);
            }
        }
        ret = sp_blocking_write(serial_port, handshake_buffer, bytes_count,
                                500);
        if (ret < 0) {
            return -1;
        }

        sp_drain(serial_port); // Wait for write to send all data
        sp_flush(serial_port, SP_BUF_INPUT); // Flush garbage out of RX

        ret = sp_blocking_read(serial_port, device->rx_buffer, 2, 50);
        if (ret >= 2) {
            if (device->rx_buffer[0] == 'O' && device->rx_buffer[1] == 'K') {
                return 0;
            }
        }
    }
    return -4; // didn't received response
}

int32_t blisp_device_get_boot_info(struct blisp_device* device, struct blisp_boot_info* boot_info)
{
    int ret;

    ret = blisp_send_command(device, 0x10, NULL, 0, false);
    if (ret < 0) return ret;

    ret = blisp_receive_response(device, true);
    if (ret < 0) return ret;

    memcpy(boot_info->boot_rom_version, &device->rx_buffer[0], 4); // TODO: Endianess
    if (device->chip->type == BLISP_CHIP_BL70X) {
        memcpy(boot_info->chip_id, &device->rx_buffer[16], 8);
    }
    // TODO: BL60X
    return 0;
}

// TODO: Use struct instead of uint8_t*
int32_t blisp_device_load_boot_header(struct blisp_device* device, uint8_t* boot_header)
{
    int ret;
    ret = blisp_send_command(device, 0x11, boot_header, 176, false);
    if (ret < 0) return ret;
    ret = blisp_receive_response(device, false);
    if (ret < 0) return ret;

    return 0;
}

int32_t blisp_device_load_segment_header(struct blisp_device* device, struct blisp_segment_header* segment_header)
{
    int ret;
    ret = blisp_send_command(device, 0x17, segment_header, 16, false);
    if (ret < 0) return ret;
    ret = blisp_receive_response(device, true); // TODO: Handle response
    if (ret < 0) return ret;

    return 0;
}

int32_t blisp_device_load_segment_data(struct blisp_device* device, uint8_t* segment_data, uint32_t segment_data_length)
{
    int ret;
    ret = blisp_send_command(device, 0x18, segment_data, segment_data_length, false);
    if (ret < 0) return ret;
    ret = blisp_receive_response(device, false);
    if (ret < 0) return ret;

    return 0;
}

int32_t blisp_device_check_image(struct blisp_device* device)
{
    int ret;
    ret = blisp_send_command(device, 0x19, NULL, 0, false);
    if (ret < 0) return ret;
    ret = blisp_receive_response(device, false);
    if (ret < 0) return ret;

    return 0;
}

int32_t
blisp_device_write_memory(struct blisp_device* device, uint32_t address,
                          uint32_t value, bool wait_for_res) {
    int ret;
    uint8_t payload[8];
    *(uint32_t*)(payload) = address;
    *(uint32_t*)(payload + 4) = value; // TODO: Endianness
    ret = blisp_send_command(device, 0x50, payload, 8, true);
    if (ret < 0) return ret;
    if (wait_for_res) {
        ret = blisp_receive_response(device, false);
        if (ret < 0) return ret;
    }

    return 0;
}

int32_t blisp_device_run_image(struct blisp_device* device)
{
    int ret;

    if (device->chip->type == BLISP_CHIP_BL70X) { // ERRATA
        ret = blisp_device_write_memory(device, 0x4000F100, 0x4E424845, true);
        if (ret < 0) return ret;
        ret = blisp_device_write_memory(device, 0x4000F104, 0x22010000, true);
        if (ret < 0) return ret;
//        ret = blisp_device_write_memory(device, 0x40000018, 0x00000000);
//        if (ret < 0) return ret;
        ret = blisp_device_write_memory(device, 0x40000018, 0x00000002, false);
        if (ret < 0) return ret;
        return 0;
    }

    ret = blisp_send_command(device, 0x1A, NULL, 0, false);
    if (ret < 0) return ret;
    ret = blisp_receive_response(device, false);
    if (ret < 0) return ret;

    return 0;
}

int32_t
blisp_device_flash_erase(struct blisp_device* device, uint32_t start_address, uint32_t end_address)
{
    uint8_t payload[8];
    *(uint32_t*)(payload + 0) = start_address;
    *(uint32_t*)(payload + 4) = end_address;

    int ret = blisp_send_command(device, 0x30, payload, 8, true);
    if (ret < 0) return ret;
    do {
        ret = blisp_receive_response(device, false);
    } while (ret == -3);

    return 0;
}

int32_t
blisp_device_flash_write(struct blisp_device* device, uint32_t start_address, uint8_t* payload, uint32_t payload_size)
{
    // TODO: Add max payload size (8184?)

    uint8_t* buffer = malloc(4 + payload_size); // TODO: Don't use malloc + add check
    *((uint32_t*)(buffer)) = start_address;
    memcpy(buffer + 4, payload, payload_size);
    int ret = blisp_send_command(device, 0x31, buffer, payload_size + 4, true);
    if (ret < 0) goto exit1;
    ret = blisp_receive_response(device, false);
exit1:
    free(buffer);
    return ret;
}

int32_t blisp_device_program_check(struct blisp_device* device)
{
    int ret = blisp_send_command(device, 0x3A, NULL, 0, true);
    if (ret < 0) return ret;
    ret = blisp_receive_response(device, false);
    if (ret < 0) return ret;

    return 0;
}

int32_t blisp_device_reset(struct blisp_device* device)
{
    int ret = blisp_send_command(device, 0x21, NULL, 0, true);
    if (ret < 0) return ret;
    ret = blisp_receive_response(device, false);
    if (ret < 0) return ret;

    return 0;
}

void blisp_device_close(struct blisp_device* device)
{
    struct sp_port* serial_port = device->serial_port;
    sp_close(serial_port);
}