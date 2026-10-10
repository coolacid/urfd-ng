#include "DeviceIO.h"

#include <fcntl.h>
#include <unistd.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>
#include <cstring>
#include <iostream>

class PosixDevice : public IDeviceIO {
public:
    PosixDevice(): fd_(-1), read_timeout_tenths_(5) {}
    virtual ~PosixDevice() { Close(); }

    bool Open(const std::string &id, std::string &outDescription) override
    {
        // id expected to be a device node path like /dev/ttyUSB0 or a symlink-resolved path
        fd_ = ::open(id.c_str(), O_RDWR | O_NOCTTY | O_SYNC);
        if (fd_ < 0) {
            std::cerr << "PosixDevice: open(" << id << ") failed: " << strerror(errno) << std::endl;
            return false;
        }

        // configure basic 8N1
        struct termios tty;
        if (tcgetattr(fd_, &tty) != 0) {
            std::cerr << "tcgetattr failed: " << strerror(errno) << std::endl;
            Close();
            return false;
        }

        cfmakeraw(&tty);
        tty.c_cflag = (tty.c_cflag & ~CSIZE) | CS8;
        tty.c_cflag |= CLOCAL | CREAD;
        tty.c_cflag &= ~(PARENB | PARODD);
        tty.c_cflag &= ~CSTOPB;
        tty.c_iflag &= ~(IXON | IXOFF | IXANY);
        tty.c_lflag = 0;
        tty.c_oflag = 0;
        tty.c_cc[VMIN]  = 1;
        // VTIME is in tenths of a second. Set a small timeout so blocking reads
        // return if the device doesn't respond, preventing probe hangs.
        tty.c_cc[VTIME] = 5; // 0.5 seconds

        // set default baud to 460800 if supported, otherwise 115200
#ifdef B460800
        cfsetospeed(&tty, B460800);
        cfsetispeed(&tty, B460800);
#else
        cfsetospeed(&tty, B115200);
        cfsetispeed(&tty, B115200);
#endif

        if (tcsetattr(fd_, TCSANOW, &tty) != 0) {
            std::cerr << "tcsetattr failed: " << strerror(errno) << std::endl;
            Close();
            return false;
        }

        outDescription = id;
        return true;
    }

    void Close() override
    {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

    ssize_t Write(const void *buf, size_t len) override
    {
        if (fd_ < 0) return -1;
        return ::write(fd_, buf, len);
    }

    ssize_t Read(void *buf, size_t len) override
    {
        if (fd_ < 0) return -1;
        // If timeout is disabled or zero, perform a blocking read
        if (read_timeout_tenths_ <= 0) {
            return ::read(fd_, buf, len);
        }

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fd_, &rfds);
        struct timeval tv;
        tv.tv_sec = read_timeout_tenths_ / 10;
        tv.tv_usec = (read_timeout_tenths_ % 10) * 100000;
        int rv = select(fd_ + 1, &rfds, NULL, NULL, &tv);
        if (rv <= 0) {
            // rv == 0 -> timeout, rv < 0 -> error
            return (rv == 0) ? 0 : -1;
        }
        return ::read(fd_, buf, len);
    }

    bool Purge() override
    {
        if (fd_ < 0) return false;
        return (tcflush(fd_, TCIOFLUSH) == 0);
    }

    bool SetRTS(bool on) override
    {
        if (fd_ < 0) return false;
        int status;
        if (ioctl(fd_, TIOCMGET, &status) == -1) return false;
        if (on)
            status |= TIOCM_RTS;
        else
            status &= ~TIOCM_RTS;
        return (ioctl(fd_, TIOCMSET, &status) == 0);
    }

    bool SetDTR(bool on) override
    {
        if (fd_ < 0) return false;
        int status;
        if (ioctl(fd_, TIOCMGET, &status) == -1) return false;
        if (on)
            status |= TIOCM_DTR;
        else
            status &= ~TIOCM_DTR;
        return (ioctl(fd_, TIOCMSET, &status) == 0);
    }

    bool SetBaudRate(unsigned long baud) override
    {
        if (fd_ < 0) return false;
        struct termios tty;
        if (tcgetattr(fd_, &tty) != 0) return false;
        speed_t sp = B115200;
        switch (baud) {
#ifdef B921600
            case 921600: sp = B921600; break;
#endif
#ifdef B460800
            case 460800: sp = B460800; break;
#endif
            case 115200: sp = B115200; break;
            case 57600: sp = B57600; break;
            case 38400: sp = B38400; break;
            default: return false;
        }
        cfsetospeed(&tty, sp);
        cfsetispeed(&tty, sp);
        return (tcsetattr(fd_, TCSANOW, &tty) == 0);
    }

    bool SetReadTimeout(int tenths) override
    {
        if (tenths < 0) tenths = 0;
        if (tenths > 255) tenths = 255;
        read_timeout_tenths_ = tenths;
        return true;
    }

    bool SetDataCharacteristics(int bits, int stopbits, int parity) override
    {
        if (fd_ < 0) return false;
        struct termios tty;
        if (tcgetattr(fd_, &tty) != 0) return false;
        tty.c_cflag &= ~CSIZE;
        if (bits == 8) tty.c_cflag |= CS8;
        else if (bits == 7) tty.c_cflag |= CS7;
        else return false;
        if (stopbits == 2) tty.c_cflag |= CSTOPB; else tty.c_cflag &= ~CSTOPB;
        if (parity == 0) tty.c_cflag &= ~PARENB; else tty.c_cflag |= PARENB;
        return (tcsetattr(fd_, TCSANOW, &tty) == 0);
    }

    bool SetFlowControl(int /*flow*/) override
    {
        // Not implemented; could set CRTSCTS
        return true;
    }

private:
    int fd_;
    int read_timeout_tenths_;
};

// Factory helper (user can instantiate PosixDevice directly later)

IDeviceIO *CreatePosixDevice()
{
    return new PosixDevice();
}
