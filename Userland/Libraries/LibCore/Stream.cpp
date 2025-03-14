/*
 * Copyright (c) 2018-2021, Andreas Kling <kling@serenityos.org>
 * Copyright (c) 2021, sin-ack <sin-ack@protonmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "Stream.h"
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#ifdef __serenity__
#    include <serenity.h>
#endif

namespace Core::Stream {

bool Stream::read_or_error(Bytes buffer)
{
    VERIFY(buffer.size());

    size_t nread = 0;
    do {
        if (is_eof())
            return false;

        auto result = read(buffer.slice(nread));
        if (result.is_error()) {
            if (result.error().is_errno() && result.error().code() == EINTR) {
                continue;
            }

            return false;
        }

        nread += result.value();
    } while (nread < buffer.size());

    return true;
}

bool Stream::write_or_error(ReadonlyBytes buffer)
{
    VERIFY(buffer.size());

    size_t nwritten = 0;
    do {
        auto result = write(buffer.slice(nwritten));
        if (result.is_error()) {
            if (result.error().is_errno() && result.error().code() == EINTR) {
                continue;
            }

            return false;
        }

        nwritten += result.value();
    } while (nwritten < buffer.size());

    return true;
}

ErrorOr<off_t> SeekableStream::tell() const
{
    // Seek with 0 and SEEK_CUR does not modify anything despite the const_cast,
    // so it's safe to do this.
    return const_cast<SeekableStream*>(this)->seek(0, SeekMode::FromCurrentPosition);
}

ErrorOr<off_t> SeekableStream::size()
{
    auto original_position = TRY(tell());

    auto seek_result = seek(0, SeekMode::FromEndPosition);
    if (seek_result.is_error()) {
        // Let's try to restore the original position, just in case.
        auto restore_result = seek(original_position, SeekMode::SetPosition);
        if (restore_result.is_error()) {
            dbgln("Core::SeekableStream::size: Couldn't restore initial position, stream might have incorrect position now!");
        }

        return seek_result.release_error();
    }

    TRY(seek(original_position, SeekMode::SetPosition));
    return seek_result.value();
}

ErrorOr<NonnullOwnPtr<File>> File::open(StringView const& filename, OpenMode mode, mode_t permissions)
{
    auto file = TRY(adopt_nonnull_own_or_enomem(new (nothrow) File(mode)));
    TRY(file->open_path(filename, permissions));
    return file;
}

ErrorOr<NonnullOwnPtr<File>> File::adopt_fd(int fd, OpenMode mode)
{
    if (fd < 0) {
        return Error::from_errno(EBADF);
    }

    if (!has_any_flag(mode, OpenMode::ReadWrite)) {
        dbgln("Core::File::adopt_fd: Attempting to adopt a file with neither Read nor Write specified in mode");
        return Error::from_errno(EINVAL);
    }

    auto file = TRY(adopt_nonnull_own_or_enomem(new (nothrow) File(mode)));
    file->m_fd = fd;
    return file;
}

ErrorOr<void> File::open_path(StringView const& filename, mode_t permissions)
{
    VERIFY(m_fd == -1);

    int flags = 0;
    if (has_flag(m_mode, OpenMode::ReadWrite)) {
        flags |= O_RDWR | O_CREAT;
    } else if (has_flag(m_mode, OpenMode::Read)) {
        flags |= O_RDONLY;
    } else if (has_flag(m_mode, OpenMode::Write)) {
        flags |= O_WRONLY | O_CREAT;
        bool should_truncate = !has_any_flag(m_mode, OpenMode::Append | OpenMode::MustBeNew);
        if (should_truncate)
            flags |= O_TRUNC;
    }

    if (has_flag(m_mode, OpenMode::Append))
        flags |= O_APPEND;
    if (has_flag(m_mode, OpenMode::Truncate))
        flags |= O_TRUNC;
    if (has_flag(m_mode, OpenMode::MustBeNew))
        flags |= O_EXCL;
    if (!has_flag(m_mode, OpenMode::KeepOnExec))
        flags |= O_CLOEXEC;
    if (!has_flag(m_mode, OpenMode::Nonblocking))
        flags |= O_NONBLOCK;

#ifdef __serenity__
    int fd = ::serenity_open(filename.characters_without_null_termination(), filename.length(), flags, permissions);
#else
    String filename_with_null_terminator { filename };
    int fd = ::open(filename_with_null_terminator.characters(), flags, permissions);
#endif

    if (fd < 0) {
        return Error::from_errno(errno);
    }

    m_fd = fd;
    return {};
}

bool File::is_readable() const { return has_flag(m_mode, OpenMode::Read); }
bool File::is_writable() const { return has_flag(m_mode, OpenMode::Write); }

ErrorOr<size_t> File::read(Bytes buffer)
{
    if (!has_flag(m_mode, OpenMode::Read)) {
        // NOTE: POSIX says that if the fd is not open for reading, the call
        //       will return EBADF. Since we already know whether we can or
        //       can't read the file, let's avoid a syscall.
        return Error::from_errno(EBADF);
    }

    ssize_t rc = ::read(m_fd, buffer.data(), buffer.size());
    if (rc < 0) {
        return Error::from_errno(errno);
    }

    m_last_read_was_eof = rc == 0;
    return rc;
}

ErrorOr<size_t> File::write(ReadonlyBytes buffer)
{
    if (!has_flag(m_mode, OpenMode::Write)) {
        // NOTE: Same deal as Read.
        return Error::from_errno(EBADF);
    }

    ssize_t rc = ::write(m_fd, buffer.data(), buffer.size());
    if (rc < 0) {
        return Error::from_errno(errno);
    }

    return rc;
}

bool File::is_eof() const { return m_last_read_was_eof; }

bool File::is_open() const { return m_fd >= 0; }

void File::close()
{
    if (!is_open()) {
        return;
    }

    // NOTE: The closing of the file can be interrupted by a signal, in which
    // case EINTR will be returned by the close syscall. So let's try closing
    // the file until we aren't interrupted by rude signals. :^)
    int rc;
    do {
        rc = ::close(m_fd);
    } while (rc < 0 && errno == EINTR);

    VERIFY(rc == 0);
    m_fd = -1;
}

ErrorOr<off_t> File::seek(i64 offset, SeekMode mode)
{
    int syscall_mode;
    switch (mode) {
    case SeekMode::SetPosition:
        syscall_mode = SEEK_SET;
        break;
    case SeekMode::FromCurrentPosition:
        syscall_mode = SEEK_CUR;
        break;
    case SeekMode::FromEndPosition:
        syscall_mode = SEEK_END;
        break;
    default:
        VERIFY_NOT_REACHED();
    }

    off_t rc = lseek(m_fd, offset, syscall_mode);
    if (rc < 0) {
        return Error::from_errno(errno);
    }

    m_last_read_was_eof = false;
    return rc;
}

ErrorOr<int> Socket::create_fd(SocketDomain domain, SocketType type)
{
    int socket_domain;
    switch (domain) {
    case SocketDomain::Inet:
        socket_domain = AF_INET;
        break;
    case SocketDomain::Local:
        socket_domain = AF_LOCAL;
        break;
    default:
        VERIFY_NOT_REACHED();
    }

    int socket_type;
    switch (type) {
    case SocketType::Stream:
        socket_type = SOCK_STREAM;
        break;
    case SocketType::Datagram:
        socket_type = SOCK_DGRAM;
        break;
    default:
        VERIFY_NOT_REACHED();
    }

    int rc = ::socket(socket_domain, socket_type, 0);
    if (rc < 0) {
        return Error::from_errno(errno);
    }

    return rc;
}

ErrorOr<IPv4Address> Socket::resolve_host(String const& host, SocketType type)
{
    int socket_type;
    switch (type) {
    case SocketType::Stream:
        socket_type = SOCK_STREAM;
        break;
    case SocketType::Datagram:
        socket_type = SOCK_DGRAM;
        break;
    default:
        VERIFY_NOT_REACHED();
    }

    struct addrinfo hints = {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = socket_type;
    hints.ai_flags = 0;
    hints.ai_protocol = 0;

    struct addrinfo* results = nullptr;
    int rc = getaddrinfo(host.characters(), nullptr, &hints, &results);
    if (rc != 0) {
        if (rc == EAI_SYSTEM) {
            return Error::from_errno(errno);
        }

        return Error::from_string_literal(gai_strerror(rc));
    }

    auto* socket_address = bit_cast<struct sockaddr_in*>(results->ai_addr);
    NetworkOrdered<u32> network_ordered_address { socket_address->sin_addr.s_addr };

    freeaddrinfo(results);

    return IPv4Address { network_ordered_address };
}

ErrorOr<void> Socket::connect_local(int fd, String const& path)
{
    auto address = SocketAddress::local(path);
    auto maybe_sockaddr = address.to_sockaddr_un();
    if (!maybe_sockaddr.has_value()) {
        dbgln("Core::Stream::Socket::connect_local: Could not obtain a sockaddr_un");
        return Error::from_errno(EINVAL);
    }

    auto addr = maybe_sockaddr.release_value();
    int rc = ::connect(fd, bit_cast<struct sockaddr*>(&addr), sizeof(addr));
    if (rc < 0) {
        return Error::from_errno(errno);
    }

    return {};
}

ErrorOr<void> Socket::connect_inet(int fd, SocketAddress const& address)
{
    auto addr = address.to_sockaddr_in();
    int rc = ::connect(fd, bit_cast<struct sockaddr*>(&addr), sizeof(addr));
    if (rc < 0) {
        return Error::from_errno(errno);
    }

    return {};
}

ErrorOr<size_t> PosixSocketHelper::read(Bytes buffer)
{
    if (!is_open()) {
        return Error::from_errno(ENOTCONN);
    }

    ssize_t rc = ::recv(m_fd, buffer.data(), buffer.size(), 0);
    if (rc < 0) {
        return Error::from_errno(errno);
    }

    m_last_read_was_eof = rc == 0;
    // If a socket read is EOF, then no more data can be read from it because
    // the protocol has disconnected. In this case, we can just disable the
    // notifier if we have one.
    if (m_last_read_was_eof && m_notifier)
        m_notifier->set_enabled(false);

    return rc;
}

ErrorOr<size_t> PosixSocketHelper::write(ReadonlyBytes buffer)
{
    if (!is_open()) {
        return Error::from_errno(ENOTCONN);
    }

    ssize_t rc = ::send(m_fd, buffer.data(), buffer.size(), 0);
    if (rc < 0) {
        return Error::from_errno(errno);
    }

    return rc;
}

void PosixSocketHelper::close()
{
    if (!is_open()) {
        return;
    }

    if (m_notifier)
        m_notifier->set_enabled(false);

    int rc;
    do {
        rc = ::close(m_fd);
    } while (rc < 0 && errno == EINTR);

    VERIFY(rc == 0);
    m_fd = -1;
}

ErrorOr<bool> PosixSocketHelper::can_read_without_blocking(int timeout) const
{
    struct pollfd the_fd = { .fd = m_fd, .events = POLLIN, .revents = 0 };

    int rc;
    do {
        rc = ::poll(&the_fd, 1, timeout);
    } while (rc < 0 && errno == EINTR);

    if (rc < 0) {
        return Error::from_errno(errno);
    }

    return (the_fd.revents & POLLIN) > 0;
}

ErrorOr<void> PosixSocketHelper::set_blocking(bool enabled)
{
    int value = enabled ? 0 : 1;
    int rc = ::ioctl(m_fd, FIONBIO, &value);

    if (rc < 0) {
        return Error::from_errno(errno);
    }

    return {};
}

ErrorOr<void> PosixSocketHelper::set_close_on_exec(bool enabled)
{
    int flags = ::fcntl(m_fd, F_GETFD);
    if (flags < 0)
        return Error::from_errno(errno);

    if (enabled)
        flags |= FD_CLOEXEC;
    else
        flags &= ~FD_CLOEXEC;

    int rc = ::fcntl(m_fd, F_SETFD, flags);
    if (rc < 0)
        return Error::from_errno(errno);

    return {};
}

void PosixSocketHelper::setup_notifier()
{
    if (!m_notifier)
        m_notifier = Core::Notifier::construct(m_fd, Core::Notifier::Read);
}

ErrorOr<NonnullOwnPtr<TCPSocket>> TCPSocket::connect(String const& host, u16 port)
{
    auto ip_address = TRY(resolve_host(host, SocketType::Stream));
    return connect(SocketAddress { ip_address, port });
}

ErrorOr<NonnullOwnPtr<TCPSocket>> TCPSocket::connect(SocketAddress const& address)
{
    auto socket = TRY(adopt_nonnull_own_or_enomem(new (nothrow) TCPSocket()));

    auto fd = TRY(create_fd(SocketDomain::Inet, SocketType::Stream));
    socket->m_helper.set_fd(fd);

    auto result = connect_inet(fd, address);
    if (result.is_error()) {
        ::close(fd);
        return result.release_error();
    }

    socket->setup_notifier();
    return socket;
}

ErrorOr<NonnullOwnPtr<TCPSocket>> TCPSocket::adopt_fd(int fd)
{
    if (fd < 0) {
        return Error::from_errno(EBADF);
    }

    auto socket = TRY(adopt_nonnull_own_or_enomem(new (nothrow) TCPSocket()));
    socket->m_helper.set_fd(fd);
    socket->setup_notifier();
    return socket;
}

ErrorOr<size_t> PosixSocketHelper::pending_bytes() const
{
    if (!is_open()) {
        return Error::from_errno(ENOTCONN);
    }

    int value;
    int rc = ::ioctl(m_fd, FIONREAD, &value);
    if (rc < 0) {
        return Error::from_errno(errno);
    }

    return static_cast<size_t>(value);
}

ErrorOr<NonnullOwnPtr<UDPSocket>> UDPSocket::connect(String const& host, u16 port)
{
    auto ip_address = TRY(resolve_host(host, SocketType::Datagram));
    return connect(SocketAddress { ip_address, port });
}

ErrorOr<NonnullOwnPtr<UDPSocket>> UDPSocket::connect(SocketAddress const& address)
{
    auto socket = TRY(adopt_nonnull_own_or_enomem(new (nothrow) UDPSocket()));

    auto fd = TRY(create_fd(SocketDomain::Inet, SocketType::Datagram));
    socket->m_helper.set_fd(fd);

    auto result = connect_inet(fd, address);
    if (result.is_error()) {
        ::close(fd);
        return result.release_error();
    }

    socket->setup_notifier();
    return socket;
}

ErrorOr<NonnullOwnPtr<LocalSocket>> LocalSocket::connect(String const& path)
{
    auto socket = TRY(adopt_nonnull_own_or_enomem(new (nothrow) LocalSocket()));

    auto fd = TRY(create_fd(SocketDomain::Local, SocketType::Stream));
    socket->m_helper.set_fd(fd);

    auto result = connect_local(fd, path);
    if (result.is_error()) {
        ::close(fd);
        return result.release_error();
    }

    socket->setup_notifier();
    return socket;
}

}
