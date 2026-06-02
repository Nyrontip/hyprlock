#pragma once

#include "Auth.hpp"

#include <atomic>
#include <memory>
#include <optional>
#include <string>
#include <thread>

enum eSafirmaState {
    SAFIRMA_IDLE = 0,
    SAFIRMA_CONNECTING,
    SAFIRMA_WAITING,
    SAFIRMA_APPROVED,
    SAFIRMA_DENIED,
    SAFIRMA_EXPIRED,
    SAFIRMA_ERROR,
};

class CSafirmaAuth : public IAuthImplementation {
  public:
    virtual ~CSafirmaAuth();

    // IAuthImplementation
    virtual eAuthImplementations       getImplType() {
        return AUTH_IMPL_SAFIRMA;
    }
    virtual void                       init() {} /* no-op: startAuth() on demand */
    virtual void                       handleInput(const std::string& input);
    virtual bool                       checkWaiting();
    virtual std::optional<std::string> getLastFailText();
    virtual std::optional<std::string> getLastPrompt();
    virtual void                       terminate();

    // Public API for overlay
    eSafirmaState getState() const {
        return m_state.load();
    }
    int           getRemainingSeconds() const {
        return m_remainingSeconds.load();
    }
    void          startAuth();
    void          cancelAuth();
    void          retryAuth();

  private:
    void authThread(int timeoutSecs);
    void killDaemon();
    bool connectUDS(int& fd, const std::string& socketPath);
    bool writeFrame(int fd, const std::string& data);
    bool readFrame(int fd, std::string& out, int timeoutSecs);

    std::thread              m_thread;
    std::atomic<eSafirmaState> m_state{SAFIRMA_IDLE};
    std::atomic<int>         m_remainingSeconds{30};
    std::atomic<bool>        m_cancelled{false};
    std::string              m_failText;
    std::string              m_promptText;
    int                      m_socketFd  = -1;
    pid_t                    m_childPid  = -1;
};

inline CSafirmaAuth* g_pSafirmaAuth = nullptr;
