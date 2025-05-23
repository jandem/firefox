/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "SandboxBroker.h"
#include "SandboxInfo.h"

#include "SandboxProfilerParent.h"
#include "SandboxLogging.h"

#include "SandboxBrokerUtils.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#ifdef XP_LINUX
#  include <sys/prctl.h>
#endif

#include <utility>

#include "GeckoProfiler.h"
#include "SpecialSystemDirectory.h"
#include "base/string_util.h"
#include "mozilla/Assertions.h"
#include "mozilla/Atomics.h"
#include "mozilla/DebugOnly.h"
#include "mozilla/Sprintf.h"
#include "mozilla/ipc/FileDescriptor.h"
#include "nsAppDirectoryServiceDefs.h"
#include "nsDirectoryServiceDefs.h"
#include "nsThreadUtils.h"
#include "sandbox/linux/system_headers/linux_syscalls.h"

namespace mozilla {

namespace {

// kernel level limit defined at
// https://elixir.bootlin.com/linux/latest/source/include/linux/sched.h#L301
// used at
// https://elixir.bootlin.com/linux/latest/source/include/linux/sched.h#L1087
static const int kThreadNameMaxSize = 16;

static Atomic<size_t> gNumBrokers;

}  // namespace

// This constructor signals failure by setting mFileDesc and aClientFd to -1.
SandboxBroker::SandboxBroker(UniquePtr<const Policy> aPolicy, int aChildPid,
                             int& aClientFd)
    : mChildPid(aChildPid), mPolicy(std::move(aPolicy)) {
  ++gNumBrokers;
  int fds[2];
  if (0 != socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, fds)) {
    SANDBOX_LOG_ERRNO("SandboxBroker: socketpair failed (%d brokers)",
                      gNumBrokers.operator size_t());
    mFileDesc = -1;
    aClientFd = -1;
    return;
  }
  mFileDesc = fds[0];
  aClientFd = fds[1];

  // When the thread will start it may already be too late to correctly care
  // about reference handling. Make sure the increment happens before the thread
  // is created to avoid races. Details in SandboxBroker::ThreadMain().
  NS_ADDREF_THIS();

  if (!PlatformThread::Create(0, this, &mThread)) {
    SANDBOX_LOG_ERRNO("SandboxBroker: thread creation failed (%d brokers)",
                      gNumBrokers.operator size_t());
    close(mFileDesc);
    close(aClientFd);
    mFileDesc = -1;
    aClientFd = -1;
  }
}

already_AddRefed<SandboxBroker> SandboxBroker::Create(
    UniquePtr<const Policy> aPolicy, int aChildPid,
    ipc::FileDescriptor& aClientFdOut) {
  int clientFd;
  // Can't use MakeRefPtr here because the constructor is private.
  RefPtr<SandboxBroker> rv(
      new SandboxBroker(std::move(aPolicy), aChildPid, clientFd));
  if (clientFd < 0) {
    rv = nullptr;
  } else {
    // FileDescriptor can be constructed from an int, but that dup()s
    // the fd; instead, transfer ownership:
    aClientFdOut = ipc::FileDescriptor(UniqueFileHandle(clientFd));
  }
  return rv.forget();
}

void SandboxBroker::Terminate() {
  // If the constructor failed, there's nothing to be done here.
  if (mFileDesc < 0) {
    return;
  }

  // Join() on the same thread while working with errno EDEADLK is technically
  // not POSIX compliant:
  // https://pubs.opengroup.org/onlinepubs/9799919799/functions/pthread_join.html#:~:text=refers%20to%20the%20calling%20thread
  if (mThread != pthread_self()) {
    shutdown(mFileDesc, SHUT_RD);
    // The thread will now get EOF even if the client hasn't exited.
    PlatformThread::Join(mThread);
  } else {
    // Nothing is waiting for this thread, so detach it to avoid
    // memory leaks.
    int rv = pthread_detach(pthread_self());
    MOZ_ALWAYS_TRUE(rv == 0);
  }

  // Now that the thread has exited, the fd will no longer be accessed.
  close(mFileDesc);
  // Having ensured that this object outlives the thread, this
  // destructor can now return.

  mFileDesc = -1;
}

SandboxBroker::~SandboxBroker() {
  Terminate();
  --gNumBrokers;
}

SandboxBroker::Policy::Policy() = default;
SandboxBroker::Policy::~Policy() = default;

SandboxBroker::Policy::Policy(const Policy& aOther)
    : mMap(aOther.mMap.Clone()) {}

// Chromium
// sandbox/linux/syscall_broker/broker_file_permission.cc
// Async signal safe
bool SandboxBroker::Policy::ValidatePath(const char* path) const {
  if (!path) return false;

  const size_t len = strlen(path);
  // No empty paths
  if (len == 0) return false;
  // Paths must be absolute and not relative
  if (path[0] != '/') return false;
  // No trailing / (but "/" is valid)
  if (len > 1 && path[len - 1] == '/') return false;
  // No trailing /.
  if (len >= 2 && path[len - 2] == '/' && path[len - 1] == '.') return false;
  // No trailing /..
  if (len >= 3 && path[len - 3] == '/' && path[len - 2] == '.' &&
      path[len - 1] == '.')
    return false;
  // No /../ anywhere
  for (size_t i = 0; i < len; i++) {
    if (path[i] == '/' && (len - i) > 3) {
      if (path[i + 1] == '.' && path[i + 2] == '.' && path[i + 3] == '/') {
        return false;
      }
    }
  }
  return true;
}

void SandboxBroker::Policy::AddPath(int aPerms, const char* aPath,
                                    AddCondition aCond) {
  nsDependentCString path(aPath);
  MOZ_ASSERT(path.Length() <= kMaxPathLen);
  if (aCond == AddIfExistsNow) {
    struct stat statBuf;
    if (lstat(aPath, &statBuf) != 0) {
      return;
    }
  }
  auto& perms = mMap.LookupOrInsert(path, MAY_ACCESS);
  MOZ_ASSERT(perms & MAY_ACCESS);

  if (SandboxInfo::Get().Test(SandboxInfo::kVerbose)) {
    SANDBOX_LOG("policy for %s: %d -> %d", aPath, perms, perms | aPerms);
  }
  perms |= aPerms;
}

void SandboxBroker::Policy::AddTree(int aPerms, const char* aPath) {
  struct stat statBuf;

  if (stat(aPath, &statBuf) != 0) {
    return;
  }

  if (!S_ISDIR(statBuf.st_mode)) {
    return;
  }

  Policy::AddTreeInternal(aPerms, aPath);
}

void SandboxBroker::Policy::AddFutureDir(int aPerms, const char* aPath) {
  Policy::AddTreeInternal(aPerms, aPath);
}

void SandboxBroker::Policy::AddTreeInternal(int aPerms, const char* aPath) {
  // Add a Prefix permission on things inside the dir.
  nsDependentCString path(aPath);
  MOZ_ASSERT(path.Length() <= kMaxPathLen - 1);
  // Enforce trailing / on aPath
  if (path.Last() != '/') {
    path.Append('/');
  }
  Policy::AddPrefixInternal(aPerms, path);

  // Add a path permission on the dir itself so it can
  // be opened. We're guaranteed to have a trailing / now,
  // so just cut that.
  path.Truncate(path.Length() - 1);
  if (!path.IsEmpty()) {
    Policy::AddPath(aPerms, path.get(), AddAlways);
  }
}

void SandboxBroker::Policy::AddPrefix(int aPerms, const char* aPath) {
  Policy::AddPrefixInternal(aPerms, nsDependentCString(aPath));
}

void SandboxBroker::Policy::AddPrefixInternal(int aPerms,
                                              const nsACString& aPath) {
  auto& perms = mMap.LookupOrInsert(aPath, MAY_ACCESS);
  MOZ_ASSERT(perms & MAY_ACCESS);

  int newPerms = perms | aPerms | RECURSIVE;
  if (SandboxInfo::Get().Test(SandboxInfo::kVerbose)) {
    SANDBOX_LOG("policy for %s: %d -> %d", PromiseFlatCString(aPath).get(),
                perms, newPerms);
  }
  perms = newPerms;
}

void SandboxBroker::Policy::AddFilePrefix(int aPerms, const char* aDir,
                                          const char* aPrefix) {
  size_t prefixLen = strlen(aPrefix);
  DIR* dirp = opendir(aDir);
  struct dirent* de;
  if (!dirp) {
    return;
  }
  while ((de = readdir(dirp))) {
    if (strcmp(de->d_name, ".") != 0 && strcmp(de->d_name, "..") != 0 &&
        strncmp(de->d_name, aPrefix, prefixLen) == 0) {
      nsAutoCString subPath;
      subPath.Assign(aDir);
      subPath.Append('/');
      subPath.Append(de->d_name);
      AddPath(aPerms, subPath.get(), AddAlways);
    }
  }
  closedir(dirp);
}

void SandboxBroker::Policy::AddDynamic(int aPerms, const char* aPath) {
  struct stat statBuf;
  bool exists = (stat(aPath, &statBuf) == 0);

  if (!exists) {
    AddPrefix(aPerms, aPath);
  } else {
    size_t len = strlen(aPath);
    if (!len) return;
    if (aPath[len - 1] == '/') {
      AddTree(aPerms, aPath);
    } else {
      AddPath(aPerms, aPath);
    }
  }
}

void SandboxBroker::Policy::AddAncestors(const char* aPath, int aPerms) {
  nsAutoCString path(aPath);

  while (true) {
    const auto lastSlash = path.RFindCharInSet("/");
    if (lastSlash <= 0) {
      MOZ_ASSERT(lastSlash == 0);
      return;
    }
    path.Truncate(lastSlash);
    AddPath(aPerms, path.get());
  }
}

void SandboxBroker::Policy::FixRecursivePermissions() {
  // This builds an entirely new hashtable in order to avoid iterator
  // invalidation problems.
  PathPermissionMap oldMap;
  mMap.SwapElements(oldMap);

  if (SandboxInfo::Get().Test(SandboxInfo::kVerbose)) {
    SANDBOX_LOG("fixing recursive policy entries");
  }

  for (const auto& entry : oldMap) {
    const nsACString& path = entry.GetKey();
    const int& localPerms = entry.GetData();
    int inheritedPerms = 0;

    nsAutoCString ancestor(path);
    // This is slightly different from the loop in AddAncestors: it
    // leaves the trailing slashes attached so they'll match AddTree
    // entries.
    while (true) {
      // Last() release-asserts that the string is not empty.  We
      // should never have empty keys in the map, and the Truncate()
      // below will always give us a non-empty string.
      if (ancestor.Last() == '/') {
        ancestor.Truncate(ancestor.Length() - 1);
      }
      const auto lastSlash = ancestor.RFindCharInSet("/");
      if (lastSlash < 0) {
        MOZ_ASSERT(ancestor.IsEmpty());
        break;
      }
      ancestor.Truncate(lastSlash + 1);
      const int ancestorPerms = oldMap.Get(ancestor);
      if (ancestorPerms & RECURSIVE) {
        // if a child is set with FORCE_DENY, do not compute inheritedPerms
        if ((localPerms & FORCE_DENY) == FORCE_DENY) {
          if (SandboxInfo::Get().Test(SandboxInfo::kVerbose)) {
            SANDBOX_LOG("skip inheritence policy for %s: %d",
                        PromiseFlatCString(path).get(), localPerms);
          }
        } else {
          inheritedPerms |= ancestorPerms & ~RECURSIVE;
        }
      }
    }

    const int newPerms = localPerms | inheritedPerms;
    if ((newPerms & ~RECURSIVE) == inheritedPerms) {
      if (SandboxInfo::Get().Test(SandboxInfo::kVerbose)) {
        SANDBOX_LOG("removing redundant %s: %d -> %d",
                    PromiseFlatCString(path).get(), localPerms, newPerms);
      }
      // Skip adding this entry to the new map.
      continue;
    }
    if (SandboxInfo::Get().Test(SandboxInfo::kVerbose)) {
      SANDBOX_LOG("new policy for %s: %d -> %d", PromiseFlatCString(path).get(),
                  localPerms, newPerms);
    }
    mMap.InsertOrUpdate(path, newPerms);
  }
}

int SandboxBroker::Policy::Lookup(const nsACString& aPath) const {
  // Early exit for paths explicitly found in the
  // whitelist.
  // This means they will not gain extra permissions
  // from recursive paths.
  int perms = mMap.Get(aPath);
  if (perms) {
    return perms;
  }

  // Not a legally constructed path
  if (!ValidatePath(PromiseFlatCString(aPath).get())) return 0;

  // Now it's either an illegal access, or a recursive
  // directory permission. We'll have to check the entire
  // whitelist for the best match (slower).
  int allPerms = 0;
  for (const auto& entry : mMap) {
    const nsACString& whiteListPath = entry.GetKey();
    const int& perms = entry.GetData();

    if (!(perms & RECURSIVE)) continue;

    // passed part starts with something on the whitelist
    if (StringBeginsWith(aPath, whiteListPath)) {
      allPerms |= perms;
    }
  }

  // Strip away the RECURSIVE flag as it doesn't
  // necessarily apply to aPath.
  return allPerms & ~RECURSIVE;
}

static bool AllowOperation(int aReqFlags, int aPerms) {
  int needed = 0;
  if (aReqFlags & R_OK) {
    needed |= SandboxBroker::MAY_READ;
  }
  if (aReqFlags & W_OK) {
    needed |= SandboxBroker::MAY_WRITE;
  }
  // We don't really allow executing anything,
  // so in true unix tradition we hijack this
  // for directory access (creation).
  if (aReqFlags & X_OK) {
    needed |= SandboxBroker::MAY_CREATE;
  }
  return (aPerms & needed) == needed;
}

static bool AllowAccess(int aReqFlags, int aPerms) {
  if (aReqFlags & ~(R_OK | W_OK | X_OK | F_OK)) {
    return false;
  }
  int needed = 0;
  if (aReqFlags & R_OK) {
    needed |= SandboxBroker::MAY_READ;
  }
  if (aReqFlags & W_OK) {
    needed |= SandboxBroker::MAY_WRITE;
  }
  return (aPerms & needed) == needed;
}

// These flags are added to all opens to prevent possible side-effects
// on this process.  These shouldn't be relevant to the child process
// in any case due to the sandboxing restrictions on it.  (See also
// the use of MSG_CMSG_CLOEXEC in SandboxBrokerCommon.cpp).
static const int kRequiredOpenFlags = O_CLOEXEC | O_NOCTTY;

// Linux originally assigned a flag bit to O_SYNC but implemented the
// semantics standardized as O_DSYNC; later, that bit was renamed and
// a new bit was assigned to the full O_SYNC, and O_SYNC was redefined
// to be both bits.  As a result, this #define is needed to compensate
// for outdated kernel headers like Android's.
#define O_SYNC_NEW 04010000
static const int kAllowedOpenFlags =
    O_APPEND | O_DIRECT | O_DIRECTORY | O_EXCL | O_LARGEFILE | O_NOATIME |
    O_NOCTTY | O_NOFOLLOW | O_NONBLOCK | O_NDELAY | O_SYNC_NEW | O_TRUNC |
    O_CLOEXEC | O_CREAT;
#undef O_SYNC_NEW

static bool AllowOpen(int aReqFlags, int aPerms) {
  if (aReqFlags & ~O_ACCMODE & ~kAllowedOpenFlags) {
    return false;
  }
  int needed;
  switch (aReqFlags & O_ACCMODE) {
    case O_RDONLY:
      needed = SandboxBroker::MAY_READ;
      break;
    case O_WRONLY:
      needed = SandboxBroker::MAY_WRITE;
      break;
    case O_RDWR:
      needed = SandboxBroker::MAY_READ | SandboxBroker::MAY_WRITE;
      break;
    default:
      return false;
  }
  if (aReqFlags & O_CREAT) {
    needed |= SandboxBroker::MAY_CREATE;
  }
  // Linux allows O_TRUNC even with O_RDONLY
  if (aReqFlags & O_TRUNC) {
    needed |= SandboxBroker::MAY_WRITE;
  }
  return (aPerms & needed) == needed;
}

static int DoStat(const char* aPath, statstruct* aBuff, int aFlags) {
  if (aFlags & O_NOFOLLOW) {
    return lstatsyscall(aPath, aBuff);
  }
  return statsyscall(aPath, aBuff);
}

static int DoLink(const char* aPath, const char* aPath2,
                  SandboxBrokerCommon::Operation aOper) {
  if (aOper == SandboxBrokerCommon::Operation::SANDBOX_FILE_LINK) {
    return link(aPath, aPath2);
  }
  if (aOper == SandboxBrokerCommon::Operation::SANDBOX_FILE_SYMLINK) {
    return symlink(aPath, aPath2);
  }
  MOZ_CRASH("SandboxBroker: Unknown link operation");
}

static int DoConnect(const char* aPath, size_t aLen, int aType,
                     bool aIsAbstract) {
  // Deny SOCK_DGRAM for the same reason it's denied for socketpair.
  if (aType != SOCK_STREAM && aType != SOCK_SEQPACKET) {
    errno = EACCES;
    return -1;
  }
  // Ensure that the address is a pathname.  (An empty string
  // resulting from an abstract address probably shouldn't have made
  // it past the policy check, but check explicitly just in case.)
  if (aPath[0] == '\0') {
    errno = ENETUNREACH;
    return -1;
  }

  // Try to copy the name into a normal-sized sockaddr_un, with
  // null-termination. Specifically, from man page:
  //
  // When the address of an abstract socket is returned, the returned addrlen is
  // greater than sizeof(sa_family_t) (i.e., greater than 2), and the name of
  // the socket is contained in the first (addrlen - sizeof(sa_family_t)) bytes
  // of sun_path.
  //
  // As mentionned in `SandboxBrokerClient::Connect()`, `DoCall` expects a
  // null-terminated string while abstract socket are not. So we receive a copy
  // here and we have to put things back correctly as a real abstract socket to
  // perform the brokered `connect()` call.
  struct sockaddr_un sun;
  memset(&sun, 0, sizeof(sun));
  sun.sun_family = AF_UNIX;
  char* sunPath = sun.sun_path;
  size_t sunLen = sizeof(sun.sun_path);
  size_t addrLen = sizeof(sun);
  if (aIsAbstract) {
    *sunPath++ = '\0';
    sunLen--;
    addrLen = offsetof(struct sockaddr_un, sun_path) + aLen + 1;
  }
  if (aLen + 1 > sunLen) {
    errno = ENAMETOOLONG;
    return -1;
  }
  memcpy(sunPath, aPath, aLen);

  // Finally, the actual socket connection.
  const int fd = socket(AF_UNIX, aType | SOCK_CLOEXEC, 0);
  if (fd < 0) {
    return -1;
  }
  if (connect(fd, reinterpret_cast<struct sockaddr*>(&sun), addrLen) < 0) {
    close(fd);
    return -1;
  }
  return fd;
}

size_t SandboxBroker::RealPath(char* aPath, size_t aBufSize, size_t aPathLen) {
  char* result = realpath(aPath, nullptr);
  if (result != nullptr) {
    base::strlcpy(aPath, result, aBufSize);
    free(result);
    // Size changed, but guaranteed to be 0 terminated
    aPathLen = strlen(aPath);
  }
  return aPathLen;
}

size_t SandboxBroker::ConvertRelativePath(char* aPath, size_t aBufSize,
                                          size_t aPathLen) {
  if (strstr(aPath, "..") != nullptr) {
    return RealPath(aPath, aBufSize, aPathLen);
  }
  return aPathLen;
}

nsCString SandboxBroker::ReverseSymlinks(const nsACString& aPath) {
  // Revert any symlinks we previously resolved.
  int32_t cutLength = aPath.Length();
  nsCString cutPath(Substring(aPath, 0, cutLength));

  for (;;) {
    nsCString orig;
    bool found = mSymlinkMap.Get(cutPath, &orig);
    if (found) {
      orig.Append(Substring(aPath, cutLength, aPath.Length() - cutLength));
      return orig;
    }
    // Not found? Remove a path component and try again.
    int32_t pos = cutPath.RFindChar('/');
    if (pos == kNotFound || pos <= 0) {
      // will be empty
      return orig;
    } else {
      // Cut until just before the /
      cutLength = pos;
      cutPath.Assign(Substring(cutPath, 0, cutLength));
    }
  }
}

int SandboxBroker::SymlinkPermissions(const char* aPath,
                                      const size_t aPathLen) {
  // Work on a temporary copy, so we can reverse it.
  // Because we bail on a writable dir, SymlinkPath
  // might not restore the callers' path exactly.
  char pathBufSymlink[kMaxPathLen + 1];
  strcpy(pathBufSymlink, aPath);

  nsCString orig =
      ReverseSymlinks(nsDependentCString(pathBufSymlink, aPathLen));
  if (!orig.IsEmpty()) {
    if (SandboxInfo::Get().Test(SandboxInfo::kVerbose)) {
      SANDBOX_LOG("Reversing %s -> %s", aPath, orig.get());
    }
    base::strlcpy(pathBufSymlink, orig.get(), sizeof(pathBufSymlink));
  }

  int perms = 0;
  // Resolve relative paths, propagate permissions and
  // fail if a symlink is in a writable path. The output is in perms.
  char* result =
      SandboxBroker::SymlinkPath(mPolicy.get(), pathBufSymlink, NULL, &perms);
  if (result != NULL) {
    free(result);
    // We finished the translation, so we have a usable return in "perms".
    return perms;
  } else {
    // Empty path means we got a writable dir in the chain or tried
    // to back out of a link target.
    return 0;
  }
}

void SandboxBroker::ThreadMain(void) {
  // Create a nsThread wrapper for the current platform thread, and register it
  // with the thread manager.
  (void)NS_GetCurrentThread();

  char threadName[kThreadNameMaxSize];
  // mChildPid can be max 7 digits because of the previous string size,
  // and 'FSBroker' is 8 bytes. The maximum thread size is 16 with the null byte
  // included. That leaves us 7 digits.
  SprintfLiteral(threadName, "FSBroker%d", mChildPid);
  PlatformThread::SetName(threadName);

  AUTO_PROFILER_REGISTER_THREAD(threadName);

  // The gtest SandboxBrokerMisc.* will loop and create / destroy SandboxBroker
  // taking a RefPtr<SandboxBroker> that gets destructed when getting out of
  // scope.
  //
  // Because Create() will start this thread we get into a situation where:
  //  - SandboxBrokerMisc creates SandboxBroker
  //    RefPtr = 1
  //  - SandboxBrokerMisc gtest is leaving the scope so destructor got called
  //    RefPtr = 0
  //  - this thread starts and add a new reference via that RefPtr
  //    RefPtr = 1
  //  - destructor does its job and closes the FD which triggers EOF and ends
  //    the while {} here
  //  - thread ends and gets out of scope so destructor gets called
  //    RefPtr = 0
  //
  // NS_ADDREF_THIS() / dont_AddRef(this) avoid this.
  RefPtr<SandboxBroker> deathGrip = dont_AddRef(this);

  // Permissive mode can only be enabled through an environment variable,
  // therefore it is sufficient to fetch the value once
  // before the main thread loop starts
  bool permissive = SandboxInfo::Get().Test(SandboxInfo::kPermissive);

  while (true) {
    struct iovec ios[2];
    // We will receive the path strings in 1 buffer and split them back up.
    char recvBuf[2 * (kMaxPathLen + 1)];
    char pathBuf[kMaxPathLen + 1];
    char pathBuf2[kMaxPathLen + 1];
    size_t pathLen = 0;
    size_t pathLen2 = 0;
    char respBuf[kMaxPathLen + 1];  // Also serves as struct stat
    Request req;
    Response resp;
    int respfd;

    // Make sure stat responses fit in the response buffer
    MOZ_ASSERT((kMaxPathLen + 1) > sizeof(struct stat));

    // This makes our string handling below a bit less error prone.
    memset(recvBuf, 0, sizeof(recvBuf));

    ios[0].iov_base = &req;
    ios[0].iov_len = sizeof(req);
    ios[1].iov_base = recvBuf;
    ios[1].iov_len = sizeof(recvBuf);

    const ssize_t recvd = RecvWithFd(mFileDesc, ios, 2, &respfd);
    if (recvd == 0) {
      if (SandboxInfo::Get().Test(SandboxInfo::kVerbose)) {
        SANDBOX_LOG("EOF from pid %d", mChildPid);
      }
      break;
    }
    // It could be possible to continue after errors and short reads,
    // at least in some cases, but protocol violation indicates a
    // hostile client, so terminate the broker instead.
    if (recvd < 0) {
      SANDBOX_LOG_ERRNO("bad read from pid %d", mChildPid);
      shutdown(mFileDesc, SHUT_RD);
      break;
    }
    if (recvd < static_cast<ssize_t>(sizeof(req))) {
      SANDBOX_LOG("bad read from pid %d (%d < %d)", mChildPid, recvd,
                  sizeof(req));
      shutdown(mFileDesc, SHUT_RD);
      break;
    }
    if (respfd == -1) {
      SANDBOX_LOG("no response fd from pid %d", mChildPid);
      shutdown(mFileDesc, SHUT_RD);
      break;
    }

    // Initialize the response with the default failure.
    memset(&resp, 0, sizeof(resp));
    memset(&respBuf, 0, sizeof(respBuf));
    resp.mError = -EACCES;
    ios[0].iov_base = &resp;
    ios[0].iov_len = sizeof(resp);
    ios[1].iov_base = nullptr;
    ios[1].iov_len = 0;
    int openedFd = -1;

    // Clear permissions
    int perms;

    // Find end of first string, make sure the buffer is still
    // 0 terminated.
    size_t recvBufLen = static_cast<size_t>(recvd) - sizeof(req);
    if (recvBufLen > 0 && recvBuf[recvBufLen - 1] != 0) {
      SANDBOX_LOG("corrupted path buffer from pid %d", mChildPid);
      shutdown(mFileDesc, SHUT_RD);
      break;
    }

    // First path should fit in maximum path length buffer.
    size_t first_len = strlen(recvBuf);
    if (first_len <= kMaxPathLen) {
      strcpy(pathBuf, recvBuf);
      // Skip right over the terminating 0, and try to copy in the
      // second path, if any. If there's no path, this will hit a
      // 0 immediately (we nulled the buffer before receiving).
      // We do not assume the second path is 0-terminated, this is
      // enforced below.
      strncpy(pathBuf2, recvBuf + first_len + 1, kMaxPathLen);

      // First string is guaranteed to be 0-terminated.
      pathLen = first_len;

      // Look up the first pathname but first translate relative paths.
      pathLen = ConvertRelativePath(pathBuf, sizeof(pathBuf), pathLen);
      perms = mPolicy->Lookup(nsDependentCString(pathBuf, pathLen));

      if (!perms) {
        // Did we arrive from a symlink in a path that is not writable?
        // Then try to figure out the original path and see if that is
        // readable. Work on the original path, this reverses
        // ConvertRelative above.
        int symlinkPerms = SymlinkPermissions(recvBuf, first_len);
        if (symlinkPerms > 0) {
          perms = symlinkPerms;
        }
      }
      if (!perms) {
        // Now try the opposite case: translate symlinks to their
        // actual destination file. Firefox always resolves symlinks,
        // and in most cases we have whitelisted fixed paths that
        // libraries will rely on and try to open. So this codepath
        // is mostly useful for Mesa which had its kernel interface
        // moved around.
        pathLen = RealPath(pathBuf, sizeof(pathBuf), pathLen);
        perms = mPolicy->Lookup(nsDependentCString(pathBuf, pathLen));
      }

      // Same for the second path.
      pathLen2 = strnlen(pathBuf2, kMaxPathLen);
      if (pathLen2 > 0) {
        // Force 0 termination.
        pathBuf2[pathLen2] = '\0';
        pathLen2 = ConvertRelativePath(pathBuf2, sizeof(pathBuf2), pathLen2);
        int perms2 = mPolicy->Lookup(nsDependentCString(pathBuf2, pathLen2));

        // Take the intersection of the permissions for both paths.
        perms &= perms2;
      }
    } else {
      // Failed to receive intelligible paths.
      perms = 0;
    }

    // And now perform the operation if allowed.
    if (perms & CRASH_INSTEAD) {
      // This is somewhat nonmodular, but it works.
      resp.mError = -ENOSYS;
    } else if ((perms & FORCE_DENY) == FORCE_DENY) {
      resp.mError = -EACCES;
    } else if (permissive || perms & MAY_ACCESS) {
      // If the operation was only allowed because of permissive mode, log it.
      if (permissive && !(perms & MAY_ACCESS)) {
        AuditPermissive(req.mOp, req.mFlags, req.mId, perms, pathBuf);
      }

      switch (req.mOp) {
        case SANDBOX_FILE_OPEN:
          if (permissive || AllowOpen(req.mFlags, perms)) {
            // Permissions for O_CREAT hardwired to 0600; if that's
            // ever a problem we can change the protocol (but really we
            // should be trying to remove uses of MAY_CREATE, not add
            // new ones).
            openedFd = open(pathBuf, req.mFlags | kRequiredOpenFlags, 0600);
            if (openedFd >= 0) {
              resp.mError = 0;
            } else {
              resp.mError = -errno;
            }
          } else {
            AuditDenial(req.mOp, req.mFlags, req.mId, perms, pathBuf);
          }
          break;

        case SANDBOX_FILE_ACCESS:
          if (permissive || AllowAccess(req.mFlags, perms)) {
            if (access(pathBuf, req.mFlags) == 0) {
              resp.mError = 0;
            } else {
              resp.mError = -errno;
            }
          } else {
            AuditDenial(req.mOp, req.mFlags, req.mId, perms, pathBuf);
          }
          break;

        case SANDBOX_FILE_STAT:
          MOZ_ASSERT(req.mBufSize == sizeof(statstruct));
          if (DoStat(pathBuf, (statstruct*)&respBuf, req.mFlags) == 0) {
            resp.mError = 0;
            ios[1].iov_base = &respBuf;
            ios[1].iov_len = sizeof(statstruct);
          } else {
            resp.mError = -errno;
          }
          break;

        case SANDBOX_FILE_CHMOD:
          if (permissive || AllowOperation(W_OK, perms)) {
            if (chmod(pathBuf, req.mFlags) == 0) {
              resp.mError = 0;
            } else {
              resp.mError = -errno;
            }
          } else {
            AuditDenial(req.mOp, req.mFlags, req.mId, perms, pathBuf);
          }
          break;

        case SANDBOX_FILE_LINK:
        case SANDBOX_FILE_SYMLINK:
          if (permissive || AllowOperation(W_OK | X_OK, perms)) {
            if (DoLink(pathBuf, pathBuf2, req.mOp) == 0) {
              resp.mError = 0;
            } else {
              resp.mError = -errno;
            }
          } else {
            AuditDenial(req.mOp, req.mFlags, req.mId, perms, pathBuf);
          }
          break;

        case SANDBOX_FILE_RENAME:
          if (permissive || AllowOperation(W_OK | X_OK, perms)) {
            if (rename(pathBuf, pathBuf2) == 0) {
              resp.mError = 0;
            } else {
              resp.mError = -errno;
            }
          } else {
            AuditDenial(req.mOp, req.mFlags, req.mId, perms, pathBuf);
          }
          break;

        case SANDBOX_FILE_MKDIR:
          if (permissive || AllowOperation(W_OK | X_OK, perms)) {
            if (mkdir(pathBuf, req.mFlags) == 0) {
              resp.mError = 0;
            } else {
              resp.mError = -errno;
            }
          } else {
            struct stat sb;
            // This doesn't need an additional policy check because
            // MAY_ACCESS is required to even enter this switch statement.
            if (lstat(pathBuf, &sb) == 0) {
              resp.mError = -EEXIST;
            } else {
              AuditDenial(req.mOp, req.mFlags, req.mId, perms, pathBuf);
            }
          }
          break;

        case SANDBOX_FILE_UNLINK:
          if (permissive || AllowOperation(W_OK | X_OK, perms)) {
            if (unlink(pathBuf) == 0) {
              resp.mError = 0;
            } else {
              resp.mError = -errno;
            }
          } else {
            AuditDenial(req.mOp, req.mFlags, req.mId, perms, pathBuf);
          }
          break;

        case SANDBOX_FILE_RMDIR:
          if (permissive || AllowOperation(W_OK | X_OK, perms)) {
            if (rmdir(pathBuf) == 0) {
              resp.mError = 0;
            } else {
              resp.mError = -errno;
            }
          } else {
            AuditDenial(req.mOp, req.mFlags, req.mId, perms, pathBuf);
          }
          break;

        case SANDBOX_FILE_READLINK:
          if (permissive || AllowOperation(R_OK, perms)) {
            ssize_t respSize =
                readlink(pathBuf, (char*)&respBuf, sizeof(respBuf));
            if (respSize >= 0) {
              if (respSize > 0) {
                // Record the mapping so we can invert the file to the original
                // symlink.
                nsDependentCString orig(pathBuf, pathLen);
                nsDependentCString xlat(respBuf, respSize);
                if (!orig.Equals(xlat) && xlat[0] == '/') {
                  if (SandboxInfo::Get().Test(SandboxInfo::kVerbose)) {
                    SANDBOX_LOG("Recording mapping %s -> %s", xlat.get(),
                                orig.get());
                  }
                  mSymlinkMap.InsertOrUpdate(xlat, orig);
                }
                // Make sure we can invert a fully resolved mapping too. If our
                // caller is realpath, and there's a relative path involved, the
                // client side will try to open this one.
                char* resolvedBuf = realpath(pathBuf, nullptr);
                if (resolvedBuf) {
                  nsDependentCString resolvedXlat(resolvedBuf);
                  if (!orig.Equals(resolvedXlat) &&
                      !xlat.Equals(resolvedXlat)) {
                    if (SandboxInfo::Get().Test(SandboxInfo::kVerbose)) {
                      SANDBOX_LOG("Recording mapping %s -> %s",
                                  resolvedXlat.get(), orig.get());
                    }
                    mSymlinkMap.InsertOrUpdate(resolvedXlat, orig);
                  }
                  free(resolvedBuf);
                }
              }
              // Truncate the reply to the size of the client's
              // buffer, matching the real readlink()'s behavior in
              // that case, and being careful with the input data.
              ssize_t callerSize =
                  std::max(AssertedCast<ssize_t>(req.mBufSize), ssize_t(0));
              respSize = std::min(respSize, callerSize);
              resp.mError = AssertedCast<int>(respSize);
              ios[1].iov_base = &respBuf;
              ios[1].iov_len = ReleaseAssertedCast<size_t>(respSize);
              MOZ_RELEASE_ASSERT(ios[1].iov_len <= sizeof(respBuf));
            } else {
              resp.mError = -errno;
            }
          } else {
            AuditDenial(req.mOp, req.mFlags, req.mId, perms, pathBuf);
          }
          break;

        case SANDBOX_SOCKET_CONNECT:
        case SANDBOX_SOCKET_CONNECT_ABSTRACT:
          if (permissive || (perms & MAY_CONNECT) != 0) {
            openedFd = DoConnect(pathBuf, pathLen, req.mFlags,
                                 req.mOp == SANDBOX_SOCKET_CONNECT_ABSTRACT);
            if (openedFd >= 0) {
              resp.mError = 0;
            } else {
              resp.mError = -errno;
            }
          } else {
            AuditDenial(req.mOp, req.mFlags, req.mId, perms, pathBuf);
          }
          break;
      }
    } else {
      MOZ_ASSERT(perms == 0);
      AuditDenial(req.mOp, req.mFlags, req.mId, perms, pathBuf);
    }

    const size_t numIO = ios[1].iov_len > 0 ? 2 : 1;
    const ssize_t sent = SendWithFd(respfd, ios, numIO, openedFd);
    if (sent < 0) {
      SANDBOX_LOG_ERRNO("failed to send broker response to pid %d", mChildPid);
    } else {
      MOZ_ASSERT(static_cast<size_t>(sent) == ios[0].iov_len + ios[1].iov_len);
    }

    // Work around Linux kernel bug: recvmsg checks for pending data
    // and then checks for EOF or shutdown, without synchronization;
    // if the sendmsg and last close occur between those points, it
    // will see no pending data (before) and a closed socket (after),
    // and incorrectly return EOF even though there is a message to be
    // read.  To avoid this, we send an extra message with a reference
    // to respfd, so the last close can't happen until after the real
    // response is read.
    //
    // See also: https://bugzil.la/1243108#c48
    const struct Response fakeResp = {-4095};
    const struct iovec fakeIO = {const_cast<Response*>(&fakeResp),
                                 sizeof(fakeResp)};
    // If the client has already read the real response and closed its
    // socket then this will fail, but that's fine.
    if (SendWithFd(respfd, &fakeIO, 1, respfd) < 0) {
      MOZ_ASSERT(errno == EPIPE || errno == ECONNREFUSED || errno == ENOTCONN);
    }

    close(respfd);

    if (openedFd >= 0) {
      close(openedFd);
    }
  }
}

void SandboxBroker::AuditPermissive(int aOp, int aFlags, uint64_t aId,
                                    int aPerms, const char* aPath) {
  MOZ_RELEASE_ASSERT(SandboxInfo::Get().Test(SandboxInfo::kPermissive));

  struct stat statBuf;

  if (lstat(aPath, &statBuf) == 0) {
    // Path exists, set errno to 0 to indicate "success".
    errno = 0;
  }

  SANDBOX_LOG_ERRNO(
      "SandboxBroker: would have denied op=%s rflags=%o perms=%d path=%s for "
      "pid=%d permissive=1; real status",
      OperationDescription[aOp], aFlags, aPerms, aPath, mChildPid);
  SandboxProfiler::ReportAudit("SandboxBroker::AuditPermissive",
                               OperationDescription[aOp], aFlags, aId, aPerms,
                               aPath, mChildPid);
}

void SandboxBroker::AuditDenial(int aOp, int aFlags, uint64_t aId, int aPerms,
                                const char* aPath) {
  if (SandboxInfo::Get().Test(SandboxInfo::kVerbose)) {
    SANDBOX_LOG(
        "SandboxBroker: denied op=%s rflags=%o perms=%d path=%s for pid=%d",
        OperationDescription[aOp], aFlags, aPerms, aPath, mChildPid);
  }
  SandboxProfiler::ReportAudit("SandboxBroker::AuditDenial",
                               OperationDescription[aOp], aFlags, aId, aPerms,
                               aPath, mChildPid);
}

}  // namespace mozilla
