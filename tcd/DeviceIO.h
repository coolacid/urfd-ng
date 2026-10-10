#pragma once
#ifndef DEVICE_IO_H
#define DEVICE_IO_H

#include <string>
#include <cstddef>

class IDeviceIO {
public:
    virtual ~IDeviceIO() {}
    // Open device by identifier (serial or device node depending on backend)
    virtual bool Open(const std::string &id, std::string &outDescription) = 0;
    virtual void Close() = 0;
    virtual ssize_t Write(const void *buf, size_t len) = 0;
    virtual ssize_t Read(void *buf, size_t len) = 0;
    virtual bool Purge() = 0;
    // control lines
    virtual bool SetRTS(bool on) = 0;
    virtual bool SetDTR(bool on) = 0;
    // configure serial parameters where applicable
    virtual bool SetBaudRate(unsigned long baud) = 0;
    virtual bool SetDataCharacteristics(int bits, int stopbits, int parity) = 0;
    virtual bool SetFlowControl(int flow) = 0;
    // Set read timeout in tenths of a second (VTIME). Return true if supported.
    virtual bool SetReadTimeout(int tenths) { (void)tenths; return false; }
};

// Factory for POSIX device implementation (implemented in DeviceIO_posix.cpp)
IDeviceIO *CreatePosixDevice();

#endif // DEVICE_IO_H
