#pragma once

#include <synchapi.h>

class Mutex
{
  public:
	Mutex() { InitializeSRWLock(&lock_); }

	void lock() { AcquireSRWLockExclusive(&lock_); }

	void unlock() { ReleaseSRWLockExclusive(&lock_); }

	bool try_lock() { return TryAcquireSRWLockExclusive(&lock_) != 0; }

  private:
	SRWLOCK lock_{};
};

class LockGuard
{
  public:
	explicit LockGuard(Mutex &m) : mutex_(m) { mutex_.lock(); }

	~LockGuard() { mutex_.unlock(); }

	LockGuard(const LockGuard &) = delete;
	LockGuard &operator=(const LockGuard &) = delete;

  private:
	Mutex &mutex_;
};
