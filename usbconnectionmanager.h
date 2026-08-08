#ifndef USBCONNECTIONMANAGER_H
#define USBCONNECTIONMANAGER_H

#include <QtGlobal>

#ifndef Q_OS_WASM

#include <QObject>
#include <QUsbDevice>
#include <QUsbEndpoint>


class USBConnectionManager : public QObject {
    Q_OBJECT
public:
    explicit USBConnectionManager(QObject *parent = nullptr);
    void initDevice();
    void sendCommand(const QByteArray &payload);

public slots:
    void onDataReceived();
    void onErrorOccurred(int status);

private:
    QUsbDevice *m_device;
    QUsbEndpoint *m_inEndpoint;
    QUsbEndpoint *m_outEndpoint;
};

extern USBConnectionManager *the_usb_connection_manager;

#endif

#endif // USBCONNECTIONMANAGER_H
