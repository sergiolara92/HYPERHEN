#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Serialize operations that temporarily change credentials of this process.
 * Recursive ownership permits a guarded session to call guarded helpers.
 * Return zero on success or a pthread error number; never change credentials.
 */
int hhp_self_credentials_lock(void);
int hhp_self_credentials_unlock(void);

#ifdef __cplusplus
}

class HhpSelfCredentialsGuard final {
  bool locked_;

public:
  HhpSelfCredentialsGuard() noexcept
      : locked_(hhp_self_credentials_lock() == 0) {}
  ~HhpSelfCredentialsGuard() noexcept {
    if (locked_) (void)hhp_self_credentials_unlock();
  }
  HhpSelfCredentialsGuard(const HhpSelfCredentialsGuard &) = delete;
  HhpSelfCredentialsGuard &operator=(const HhpSelfCredentialsGuard &) = delete;
  bool ready() const noexcept { return locked_; }
};
#endif
