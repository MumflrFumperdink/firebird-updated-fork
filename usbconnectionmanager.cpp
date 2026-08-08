#include "usbconnectionmanager.h"

#include "qmlbridge.h"
#include "core/usb.h"
#include "core/usblink.h"
#include "core/emu.h"
#include "core/mem.h"

extern "C" {
    void send_usb_packet_to_device(int endpointNumber, uint8_t* buffer, int length);
    extern void usb_int_check();
    extern void usblink_receive(int ep, uint8_t *buf, uint32_t size);
    extern void usblink_connect();
    extern void usblink_start_send();
    extern uint16_t usblink_data_checksum(struct packet *packet);
    extern uint8_t usblink_header_checksum(struct packet *packet);
    extern struct packet usblink_send_buffer;

    #define HDR_SIZE 16

    static uint8_t reasm_buf[16 + 4 + 1440]; // header + bigdatasize + max bigdata
    static int reasm_len = 0;

    // Returns -1 if not enough bytes buffered yet to know the full size,
    // otherwise returns the total packet size (header + payload).
    static int packet_total_size(const uint8_t* buf, int have) {
        if (have < HDR_SIZE) return -1;

        uint8_t data_size = buf[12]; // offset of data_size field, see layout below

        if (data_size != 0xFF) {
            return HDR_SIZE + data_size;
        }

        // bigdata case: need 4 more bytes for bigdatasize before we know total length
        if (have < HDR_SIZE + 4) return -1;

        uint32_t bigdatasize;
        memcpy(&bigdatasize, buf + HDR_SIZE, sizeof(bigdatasize)); // wire-endian
        return HDR_SIZE + 4 + bigdatasize;
    }

    static void process_complete_packet(int endpoint, struct packet* in) {
        if (in->src.service == BSWAP16(0xFF)) {
            emuprintf("inject in ACK\n");
        } else {
            emuprintf("inject in %x:%x -> %x:%x \n",
                   BSWAP16(in->src.addr), BSWAP16(in->src.service),
                   BSWAP16(in->dst.addr), BSWAP16(in->dst.service));
            for (int i = 0; i < in->data_size; i++)
                emuprintf(" %02x [%c]", in->data[i], isprint(in->data[i]) ? in->data[i] : '?');
            emuprintf("\n");
        }

        if (in->src.service == BSWAP16(0x4003)) {
            struct packet *out = &usblink_send_buffer;
            // printf("usblink inject addr req.\n");
            out->src.service = BSWAP16(0x4003);
            out->dst.service = BSWAP16(0x4003);
            out->data_size = 4;
            out->ack = 0;
            out->seqno = 1;
            uint16_t tmp = SRC_ADDR;
            memcpy(out->data + 0, &tmp, sizeof(tmp));
            tmp = BSWAP16(0xFF00);
            memcpy(out->data + 2, &tmp, sizeof(tmp));
            out->constant   = CONSTANT;
            out->src.addr   = DST_ADDR;
            out->dst.addr   = SRC_ADDR;
            out->data_check = usblink_data_checksum(&usblink_send_buffer);
            out->hdr_check  = usblink_header_checksum(&usblink_send_buffer);
            send_usb_packet_to_device(endpoint, (uint8_t*)out, sizeof(struct packet));
            return;
        }

        usblink_send_buffer = *in;
        usblink_start_send();
    }

    bool qmlIsConnectedToDevice() {
        return the_qml_bridge->getIsConnectedToDevice();
    }
}


#ifdef Q_OS_WASM
#include <emscripten.h>
#include <QDebug>

EM_JS(void, start_usb_packet_relay, (), {
    if (!globalThis.activeUsbDevice) {
        console.error("No active USB device paired to relay packets from.");
        return;
    }

    const device = globalThis.activeUsbDevice;

    const iface = device.configuration.interfaces[0];
    const interfaceNumber = iface.interfaceNumber;
    const alt = iface.alternate;
    const bulkIn = alt.endpoints.find(e => e.direction === "in" && e.type === "bulk");
    const maxPacketSize = bulkIn.packetSize;
    const enpointNum = bulkIn.endpointNumber;

    async function readLoop() {
        try {

            while (device.opened) {
                const result = await device.transferIn(enpointNum, maxPacketSize);

                if (result.status === 'ok' && result.data.byteLength > 0) {
                    const dataView = result.data;
                    const length = dataView.byteLength;

                    const bufferPointer = Module._malloc(length);

                    const heapView = new Uint8Array(HEAPU8.buffer, bufferPointer, length);
                    heapView.set(new Uint8Array(dataView.buffer, dataView.byteOffset, length));

                    Module._inject_usb_packet_to_emulator(enpointNum, bufferPointer, length);

                    Module._free(bufferPointer);
                }
            }
        } catch (err) {
            if (err.name === 'NotFoundError' || err.name === 'NetworkError' || err.name === 'SecurityError' || err.message.includes('disconnected')) {
                // console.log("disconnect");
                _on_usb_device_disconnected();
            }
            else {
                console.warn("USB Packet Relay disconnected or reset:", err);
            }
        }
    }

    device.open()
        .then(() => device.selectConfiguration(device.configuration.configurationValue))
        .then(() => device.claimInterface(iface.interfaceNumber))
        .then(() => {
            readLoop();
        })
        .catch(err => console.error("Failed to initialize USB interface endpoints:", err));
});

EM_JS(void, send_usb_packet_to_device, (int endpointNumber, uint8_t* buffer, int length), {
    if (!globalThis.activeUsbDevice || !globalThis.activeUsbDevice.opened) return;

    const sharedView = HEAPU8.subarray(buffer, buffer + length);
    const unsharedBytes = new Uint8Array(length);
    unsharedBytes.set(sharedView);
    globalThis.activeUsbDevice.transferOut(endpointNumber, unsharedBytes)
        .catch(err => {
            if (err.name === 'NotFoundError' || err.name === 'NetworkError' || err.name === 'SecurityError' || err.message.includes('disconnected')) {
                // console.log("disconnect");
                _on_usb_device_disconnected();
            }
            else {
                console.error("Failed to execute downstream USB transfer:", err)
            }
        });
});



extern "C" {
    EMSCRIPTEN_KEEPALIVE void on_usb_device_connected(int vendorId, int productId) {
        // qDebug() << "Device Connected!"
        //          << "VID:" << QString::number(vendorId, 16).toUpper()
        //          << "PID:" << QString::number(productId, 16).toUpper();


        // https://the-sz.com/products/usbid/index.php?v=0x0451
        // Vendor ID 0x451 is for Texas Instruments
        // Product ID 0xE012 is for TI-Nspire Calculators
        // Product ID 0xE022 is for TI-Nspire CX II
        if (vendorId != 0x451 || !(productId == 0xE012 || productId == 0xE022)) {
            printf("Unknown device. vendorId=0x%x, productId=0x%x", vendorId, productId);
            return;
        }

        usblink_reset();

        the_qml_bridge->setIsConnectedToDevice(true);

        usblink_connect();
        start_usb_packet_relay();
    }

    EMSCRIPTEN_KEEPALIVE void inject_usb_packet_to_emulator(int endpoint, uint8_t* buffer, int length) {
        if (!buffer || length <= 0) return;

        if (reasm_len + length > (int)sizeof(reasm_buf)) {
            printf("reassembly overflow, dropping stream\n");
            reasm_len = 0;
            return;
        }
        memcpy(reasm_buf + reasm_len, buffer, length);
        reasm_len += length;

        for (;;) {
            int full_size = packet_total_size(reasm_buf, reasm_len);
            if (full_size < 0) break;           // don't even know the size yet
            if (reasm_len < full_size) break;   // know the size, but not all bytes have arrived

            struct packet* pkt = (struct packet*)reasm_buf;
            process_complete_packet(endpoint, pkt);

            memmove(reasm_buf, reasm_buf + full_size, reasm_len - full_size);
            reasm_len -= full_size;
        }
    }

    EMSCRIPTEN_KEEPALIVE void on_usb_device_disconnected() {
        the_qml_bridge->setIsConnectedToDevice(false);

        EM_ASM({
            globalThis.activeUsbDevice = null;
        });

        usb.portsc &= ~1;   // Clear Current Connect Status (Bit 0)
        usb.portsc |= 0x00000002;  // Set Connect Status Change flag

        usb.usbsts |= 0x10;        // 0x10 = Host System Error status bit
        usb.usbsts |= 0x04;        // Port Change Detect
        usb.usbsts |= 0x02;         // USB Error Interrupt (USBERRINT - tells OS a packet failed)
        usb.usbsts |= 0x01;         // USBINT (USB Interrupt triggered)

        usb.epsr = 0;             // Reset endpoint pipeline

        usblink_reset();

        usb_int_check();
    }
}

#else

void send_usb_packet_to_device(int endpointNumber, uint8_t* buffer, int length) {
    const QByteArray buffer_arr = QByteArray((char*)buffer, length);
    the_usb_connection_manager->sendCommand(buffer_arr);
}

USBConnectionManager *the_usb_connection_manager = new USBConnectionManager();


USBConnectionManager::USBConnectionManager(QObject *parent) : QObject(parent) {
    m_device = new QUsbDevice(this);
}

#define EP_IN  0x81
#define EP_OUT 0x01
void USBConnectionManager::initDevice() {
    // Vendor ID 0x451 is for Texas Instruments
    // Product ID 0xE012 is for TI-Nspire Calculators
    // Product ID 0xE022 is for TI-Nspire CX II
    QUsb::Id devId;
    devId.vid = 0x451;
    devId.pid = 0xE012;
    m_device->setId(devId);

    if (m_device->open() != 0) {
        qDebug() << "Failed to claim device interface.";
        return;
    }

    m_inEndpoint = new QUsbEndpoint(m_device, QUsbEndpoint::Type::bulkEndpoint, EP_IN);
    m_outEndpoint = new QUsbEndpoint(m_device, QUsbEndpoint::bulkEndpoint, EP_OUT);

    if (!m_inEndpoint->open(QIODevice::ReadOnly)) {
        qDebug() << "Fatal: Failed to open the USB in endpoint stream! Status:" << m_inEndpoint->status();
        return;
    }
    m_inEndpoint->setPolling(true);

    if (!m_outEndpoint->open(QIODevice::WriteOnly)) {
        qDebug() << "Fatal: Failed to open the USB out endpoint stream! Status:" << m_outEndpoint->status();
        return;
    } else {
        //qDebug() << "USB Endpoint pipeline opened successfully.";
    }

    usblink_reset();
    the_qml_bridge->setIsConnectedToDevice(true);
    usblink_connect();

    connect(m_inEndpoint, &QUsbEndpoint::readyRead, this, &USBConnectionManager::onDataReceived);
    connect(m_inEndpoint, &QUsbEndpoint::error, this, &USBConnectionManager::onErrorOccurred);
    connect(m_outEndpoint, &QUsbEndpoint::error, this, &USBConnectionManager::onErrorOccurred);

    struct packet *out = &usblink_send_buffer;
    // printf("usblink inject addr req.\n");
    out->src.service = BSWAP16(0x4003);
    out->dst.service = BSWAP16(0x4003);
    out->data_size = 4;
    out->ack = 0;
    out->seqno = 1;
    uint16_t tmp = SRC_ADDR;
    memcpy(out->data + 0, &tmp, sizeof(tmp));
    tmp = BSWAP16(0xFF00);
    memcpy(out->data + 2, &tmp, sizeof(tmp));
    out->constant   = CONSTANT;
    out->src.addr   = DST_ADDR;
    out->dst.addr   = SRC_ADDR;
    out->data_check = usblink_data_checksum(&usblink_send_buffer);
    out->hdr_check  = usblink_header_checksum(&usblink_send_buffer);
    send_usb_packet_to_device(1, (uint8_t*)out, sizeof(struct packet));


    // Start continuous non-blocking polling
    m_inEndpoint->read(64); // Requests an initial 64-byte frame block
}

void USBConnectionManager::onDataReceived() {
    if (!m_inEndpoint->isOpen()) return;

    QByteArray data = m_inEndpoint->readAll();

    // qDebug() << "Async data arrived via Qt slots! Size:" << data.size();
    // qDebug() << "Data Hex:" << data.toHex().toUpper();

    if (!data.cbegin() || data.length() <= 0) return;

    if (reasm_len + data.length() > (int)sizeof(reasm_buf)) {
        printf("reassembly overflow, dropping stream\n");
        reasm_len = 0;
        return;
    }
    memcpy(reasm_buf + reasm_len, data.cbegin(), data.length());
    reasm_len += data.length();

    for (;;) {
        int full_size = packet_total_size(reasm_buf, reasm_len);
        if (full_size < 0) break;           // don't even know the size yet
        if (reasm_len < full_size) break;   // know the size, but not all bytes have arrived

        struct packet* pkt = (struct packet*)reasm_buf;
        process_complete_packet(1, pkt);

        memmove(reasm_buf, reasm_buf + full_size, reasm_len - full_size);
        reasm_len -= full_size;

        // Trigger next read command frame to maintain continuous streaming loop
        m_inEndpoint->read(64);
    }
}

void USBConnectionManager::sendCommand(const QByteArray &payload) {
    if (!m_outEndpoint || !m_outEndpoint->isOpen()) {
        qDebug() << "Cannot send command: Outbound USB endpoint is not open!";
        return;
    }

    // 1. Write the raw byte array down to the physical hardware link
    qint64 bytesWritten = m_outEndpoint->write(payload);

    if (bytesWritten < 0) {
        qDebug() << "Write transaction rejected by host controller system.";
    } else {

        // OPTIONAL: Force the thread to block until the hardware confirms receipt (so no early reads)
        //m_outEndpoint->waitForBytesWritten(1000);
    }
}

struct usb_td { // Transfer descriptor
    uint32_t next_td;
    uint32_t flags;
    uint32_t bufptr[5];
};

void USBConnectionManager::onErrorOccurred(int status) {
    if (status == QUsbEndpoint::transferTimeout) return;
    if (status == QUsbEndpoint::transferError && the_qml_bridge->getIsConnectedToDevice()) {
        printf("=== USB DISCONNECT TRIGGERED ===\n");
        QTimer::singleShot(0, this, [this]() {
            the_qml_bridge->setIsConnectedToDevice(false);

             m_inEndpoint->setPolling(false);

            if (m_inEndpoint) {
                m_inEndpoint->disconnect();
                m_inEndpoint->close();
            }

            if (m_outEndpoint) {
                m_outEndpoint->disconnect();
                m_outEndpoint->close();
            }

            if (m_device) {
                m_device->disconnect();
                m_device->close();
            }

            usb_int_check();

        });
    }
    qDebug() << "Endpoint stream intercepted error status code:" << status;
}

#endif
