#include "SafirmaAuth.hpp"
#include "../config/ConfigManager.hpp"
#include "../core/hyprlock.hpp"
#include "../helpers/Log.hpp"

#include <cerrno>
#include <cstring>
#include <hyprlang.hpp>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

CSafirmaAuth::~CSafirmaAuth() {
    terminate();
}

void CSafirmaAuth::startAuth() {
    if (m_state.load() != SAFIRMA_IDLE)
        return;

    m_cancelled.store(false);
    m_failText.clear();
    m_promptText.clear();

    static const auto TIMEOUTCFG = g_pConfigManager->getValue<Hyprlang::INT>("auth:safirma:timeout");
    int               timeout    = *TIMEOUTCFG;
    if (timeout < 30)
        timeout = 30;
    if (timeout > 120)
        timeout = 120;

    m_remainingSeconds.store(timeout);
    m_state.store(SAFIRMA_CONNECTING);

    m_thread = std::thread([this, timeout]() { authThread(timeout); });
}

void CSafirmaAuth::cancelAuth() {
    m_cancelled.store(true);
    m_state.store(SAFIRMA_IDLE);

    if (m_socketFd >= 0) {
        close(m_socketFd);
        m_socketFd = -1;
    }

    if (m_thread.joinable())
        m_thread.join();

    m_remainingSeconds.store(30);
}

void CSafirmaAuth::retryAuth() {
    if (m_thread.joinable())
        m_thread.join();

    m_socketFd = -1;
    m_state.store(SAFIRMA_IDLE);
    m_cancelled.store(false);
    m_remainingSeconds.store(30);
    m_failText.clear();
    m_promptText.clear();
}

void CSafirmaAuth::handleInput(const std::string& input) {
    if (input == "safirma:start") {
        startAuth();
    } else if (input == "safirma:cancel") {
        cancelAuth();
    } else if (input == "safirma:retry") {
        retryAuth();
    }
}

bool CSafirmaAuth::checkWaiting() {
    auto s = m_state.load();
    return s == SAFIRMA_CONNECTING || s == SAFIRMA_WAITING;
}

std::optional<std::string> CSafirmaAuth::getLastFailText() {
    if (!m_failText.empty())
        return std::optional(m_failText);
    return std::nullopt;
}

std::optional<std::string> CSafirmaAuth::getLastPrompt() {
    if (!m_promptText.empty())
        return std::optional(m_promptText);
    return std::nullopt;
}

void CSafirmaAuth::terminate() {
    cancelAuth();
}

bool CSafirmaAuth::connectUDS(int& fd, const std::string& socketPath) {
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        Log::logger->log(Log::ERR, "safirma: socket() failed: {}", strerror(errno));
        return false;
    }

    sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socketPath.c_str(), sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        Log::logger->log(Log::WARN, "safirma: connect() to {} failed: {}", socketPath, strerror(errno));
        close(fd);
        fd = -1;
        return false;
    }

    Log::logger->log(Log::INFO, "safirma: connected to {}", socketPath);
    return true;
}

bool CSafirmaAuth::writeFrame(int fd, const std::string& data) {
    uint32_t len = data.size();
    uint8_t  header[4];
    header[0] = len & 0xFF;
    header[1] = (len >> 8) & 0xFF;
    header[2] = (len >> 16) & 0xFF;
    header[3] = (len >> 24) & 0xFF;

    if (write(fd, header, 4) < 0)
        return false;
    if (write(fd, data.data(), data.size()) < 0)
        return false;
    return true;
}

bool CSafirmaAuth::readFrame(int fd, std::string& out, int timeoutSecs) {

    // Set receive timeout
    struct timeval tv;
    tv.tv_sec  = timeoutSecs;
    tv.tv_usec = 0;
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
        Log::logger->log(Log::ERR, "safirma: setsockopt SO_RCVTIMEO failed: {}", strerror(errno));
        return false;
    }

    // Read 4-byte length header
    uint8_t header[4];
    ssize_t n = read(fd, header, 4);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            Log::logger->log(Log::WARN, "safirma: read timeout");
        else
            Log::logger->log(Log::ERR, "safirma: read header failed: {}", strerror(errno));
        return false;
    }
    if (n == 0) {
        Log::logger->log(Log::WARN, "safirma: connection closed by peer");
        return false;
    }
    if (n < 4) {
        Log::logger->log(Log::ERR, "safirma: short header read: {} bytes", n);
        return false;
    }

    uint32_t payloadLen = header[0] | (header[1] << 8) | (header[2] << 16) | (header[3] << 24);

    if (payloadLen > 65536) {
        Log::logger->log(Log::ERR, "safirma: response too large: {} bytes", payloadLen);
        return false;
    }

    std::string payload(payloadLen, '\0');
    size_t      totalRead = 0;
    while (totalRead < payloadLen) {
        n = read(fd, &payload[totalRead], payloadLen - totalRead);
        if (n <= 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                Log::logger->log(Log::WARN, "safirma: read timeout during payload");
            else if (n == 0)
                Log::logger->log(Log::WARN, "safirma: connection closed during payload");
            else
                Log::logger->log(Log::ERR, "safirma: read payload failed: {}", strerror(errno));
            return false;
        }
        totalRead += n;
    }

    out = payload;
    return true;
}

void CSafirmaAuth::authThread(int timeoutSecs) {

    // Determine socket path
    const char* envPath = getenv("SAFIRMA_SOCKET_PATH");
    std::string socketPath = envPath ? std::string(envPath) : "/tmp/safirma/safirma.sock";

    // Connect UDS
    if (!connectUDS(m_socketFd, socketPath)) {
        m_failText = "Cannot connect to safirmad";
        m_state.store(SAFIRMA_ERROR);
        return;
    }

    // Send authenticate request
    std::string request = "{\"type\":\"authenticate\"}";
    if (!writeFrame(m_socketFd, request)) {
        Log::logger->log(Log::ERR, "safirma: write failed: {}", strerror(errno));
        close(m_socketFd);
        m_socketFd = -1;
        m_failText = "Connection lost";
        m_state.store(SAFIRMA_ERROR);
        return;
    }

    m_promptText = "Open SAFIRMA on your phone";
    m_state.store(SAFIRMA_WAITING);

    // Countdown loop: wait for response or timeout
    while (m_remainingSeconds.load() > 0 && !m_cancelled.load()) {

        // Try to read a frame with 1-second timeout slices
        struct timeval tv;
        tv.tv_sec  = 1;
        tv.tv_usec = 0;
        setsockopt(m_socketFd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        uint8_t  header[4];
        ssize_t  n = read(m_socketFd, header, 4);

        if (m_cancelled.load()) {
            close(m_socketFd);
            m_socketFd = -1;
            return;
        }

        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // Timeout slice — decrement counter and continue
                m_remainingSeconds.store(m_remainingSeconds.load() - 1);
                g_pHyprlock->enqueueForceUpdateTimers();
                continue;
            }
            Log::logger->log(Log::ERR, "safirma: read error: {}", strerror(errno));
            close(m_socketFd);
            m_socketFd = -1;
            m_failText = "Connection lost";
            m_state.store(SAFIRMA_ERROR);
            return;
        }

        if (n == 0) {
            close(m_socketFd);
            m_socketFd = -1;
            m_failText = "Connection closed";
            m_state.store(SAFIRMA_ERROR);
            return;
        }

        if (n < 4) {
            Log::logger->log(Log::ERR, "safirma: short header");
            continue;
        }

        uint32_t payloadLen = header[0] | (header[1] << 8) | (header[2] << 16) | (header[3] << 24);
        if (payloadLen > 65536) {
            Log::logger->log(Log::ERR, "safirma: response too large");
            close(m_socketFd);
            m_socketFd = -1;
            m_failText = "Protocol error";
            m_state.store(SAFIRMA_ERROR);
            return;
        }

        std::string payload(payloadLen, '\0');
        size_t totalRead = 0;
        while (totalRead < payloadLen && !m_cancelled.load()) {
            n = read(m_socketFd, &payload[totalRead], payloadLen - totalRead);
            if (n <= 0)
                break;
            totalRead += n;
        }

        if (m_cancelled.load()) {
            close(m_socketFd);
            m_socketFd = -1;
            return;
        }

        if (totalRead < payloadLen) {
            close(m_socketFd);
            m_socketFd = -1;
            m_failText = "Connection lost";
            m_state.store(SAFIRMA_ERROR);
            return;
        }

        // Parse response — minimal string search (no JSON library)
        if (payload.find("\"ok\":true") != std::string::npos) {
            m_state.store(SAFIRMA_APPROVED);
            g_pAuth->enqueueUnlock();
            close(m_socketFd);
            m_socketFd = -1;
            return;
        }

        if (payload.find("\"ok\":false") != std::string::npos) {
            m_failText = "Authentication denied";
            m_state.store(SAFIRMA_DENIED);
            g_pAuth->enqueueFail(m_failText, AUTH_IMPL_SAFIRMA);
            close(m_socketFd);
            m_socketFd = -1;
            return;
        }

        if (payload.find("\"type\":\"error\"") != std::string::npos) {
            // Try to extract message field
            auto msgPos = payload.find("\"message\":\"");
            if (msgPos != std::string::npos) {
                msgPos += 11; // skip "\"message\":\""
                auto endPos = payload.find("\"", msgPos);
                if (endPos != std::string::npos)
                    m_failText = payload.substr(msgPos, endPos - msgPos);
                else
                    m_failText = "Authentication error";
            } else
                m_failText = "Authentication error";

            m_state.store(SAFIRMA_ERROR);
            g_pAuth->enqueueFail(m_failText, AUTH_IMPL_SAFIRMA);
            close(m_socketFd);
            m_socketFd = -1;
            return;
        }

        // Unknown response — keep waiting
        Log::logger->log(Log::WARN, "safirma: unexpected response: {}", payload);
    }

    // If we exited the loop because timer reached 0
    if (m_remainingSeconds.load() <= 0 && !m_cancelled.load()) {
        m_failText = "Challenge expired";
        m_state.store(SAFIRMA_EXPIRED);
        g_pAuth->enqueueFail(m_failText, AUTH_IMPL_SAFIRMA);
    }

    if (m_socketFd >= 0) {
        close(m_socketFd);
        m_socketFd = -1;
    }
}
