#include "git/transfer.hpp"

#include <git2.h>
// git_error_set_str lives here rather than in git2.h: setting an error from a
// callback is a thing only a transport or a callback does, so libgit2 files it
// under sys/.
#include <git2/sys/errors.h>

#include <algorithm>
#include <cstddef>
// For back_inserter, which libstdc++ hands over through <vector> and MSVC's STL
// does not. Neither is wrong — a standard header is allowed to include another —
// which is why a missing include of this kind builds on one compiler and fails
// on the next, and why the Windows CI job is the thing that finds them.
#include <iterator>
#include <string>
#include <utility>
#include <vector>

namespace gittop::git {
namespace {

std::string LastError() {
  const git_error* e = git_error_last();
  if (e != nullptr && e->message != nullptr) {
    return e->message;
  }
  return "unknown libgit2 error";
}

// Small owning handles in the same spirit as IndexHandle in repository.cpp:
// every libgit2 object here has at least one early-return path between its
// allocation and its free.
template <typename T, void (*Free)(T*)>
class Owned {
 public:
  Owned() = default;
  ~Owned() { reset(); }
  Owned(const Owned&) = delete;
  Owned& operator=(const Owned&) = delete;

  T** put() {
    reset();
    return &ptr_;
  }
  T* get() const { return ptr_; }
  explicit operator bool() const { return ptr_ != nullptr; }
  void reset() {
    if (ptr_ != nullptr) {
      Free(ptr_);
      ptr_ = nullptr;
    }
  }

 private:
  T* ptr_ = nullptr;
};

using RepoHandle = Owned<git_repository, git_repository_free>;
using RemoteHandle = Owned<git_remote, git_remote_free>;
using RefHandle = Owned<git_reference, git_reference_free>;
using ObjectHandle = Owned<git_object, git_object_free>;
using AnnotatedHandle = Owned<git_annotated_commit, git_annotated_commit_free>;

// Everything the libgit2 callbacks need. One struct behind the void* payload,
// because a C callback gets exactly one pointer and there are five of them.
struct Context {
  ProgressSink* sink = nullptr;
  const std::atomic<bool>* cancel = nullptr;
  const Credentials* credentials = nullptr;

  TransferProgress progress;

  // libgit2 will keep asking for credentials until something authenticates or
  // the callback errors. Handing back the same rejected token forever is an
  // infinite loop, so the second ask is treated as "that did not work".
  int credential_attempts = 0;

  // Filled by push_update_reference. A push can be refused by the server while
  // git_remote_push still returns zero, so this is the only place the answer
  // actually arrives.
  std::vector<std::string> rejections;
  bool any_ref_updated = false;
};

void Publish(Context* ctx) {
  if (ctx->sink != nullptr) {
    ctx->sink->Publish(ctx->progress);
  }
}

bool Cancelled(const Context* ctx) {
  return ctx->cancel != nullptr && ctx->cancel->load();
}

int TransferProgressCb(const git_indexer_progress* stats, void* payload) {
  auto* ctx = static_cast<Context*>(payload);
  ctx->progress.received_objects = static_cast<int>(stats->received_objects);
  ctx->progress.indexed_objects = static_cast<int>(stats->indexed_objects);
  ctx->progress.total_objects = static_cast<int>(stats->total_objects);
  ctx->progress.received_bytes = static_cast<std::int64_t>(stats->received_bytes);

  // Resolving deltas is local work and can take longer than the download on a
  // big repository. Calling it "receiving" makes a stalled bar look like a
  // stalled network.
  ctx->progress.phase = stats->received_objects < stats->total_objects
                            ? TransferPhase::Receiving
                            : TransferPhase::Resolving;
  Publish(ctx);
  return Cancelled(ctx) ? -1 : 0;
}

int PushTransferProgressCb(unsigned int current, unsigned int total, std::size_t bytes,
                           void* payload) {
  auto* ctx = static_cast<Context*>(payload);
  ctx->progress.phase = TransferPhase::Sending;
  ctx->progress.pushed_objects = static_cast<int>(current);
  ctx->progress.total_push_objects = static_cast<int>(total);
  ctx->progress.received_bytes = static_cast<std::int64_t>(bytes);
  Publish(ctx);
  return Cancelled(ctx) ? -1 : 0;
}

// The server's own chatter. Trimmed of the carriage returns it uses to redraw
// its progress line in place, which a terminal that is not echoing it would
// otherwise turn into a wall.
int SidebandCb(const char* str, int len, void* payload) {
  auto* ctx = static_cast<Context*>(payload);
  if (str == nullptr || len <= 0) {
    return 0;
  }
  std::string message(str, static_cast<std::size_t>(len));
  const std::size_t last = message.find_last_of("\r\n");
  if (last != std::string::npos) {
    const std::size_t start = message.find_last_of("\r\n", last == 0 ? 0 : last - 1);
    message = message.substr(start == std::string::npos ? 0 : start + 1);
  }
  while (!message.empty() && (message.back() == '\r' || message.back() == '\n')) {
    message.pop_back();
  }
  if (!message.empty()) {
    ctx->progress.remote_message = std::move(message);
    Publish(ctx);
  }
  return Cancelled(ctx) ? -1 : 0;
}

int PushUpdateReferenceCb(const char* refname, const char* status, void* data) {
  auto* ctx = static_cast<Context*>(data);
  if (status == nullptr) {
    ctx->any_ref_updated = true;
    return 0;
  }
  ctx->rejections.push_back(std::string(refname == nullptr ? "?" : refname) + ": " + status);
  return 0;
}

int CredentialCb(git_credential** out, const char* url, const char* username_from_url,
                 unsigned int allowed_types, void* payload) {
  auto* ctx = static_cast<Context*>(payload);
  (void)url;

  if (++ctx->credential_attempts > 2) {
    git_error_set_str(GIT_ERROR_NET, "the remote rejected these credentials");
    return GIT_EAUTH;
  }

  // Asked for before the key itself on an ssh remote that had no user in its
  // URL. "git" is what every hosted provider expects.
  if ((allowed_types & GIT_CREDENTIAL_USERNAME) != 0U) {
    return git_credential_username_new(out, username_from_url != nullptr ? username_from_url
                                                                         : "git");
  }

  if ((allowed_types & GIT_CREDENTIAL_SSH_KEY) != 0U) {
    // Only reachable on a libssh2 build. With the exec transport the system ssh
    // does its own authentication against the user's agent, config and keys,
    // and libgit2 never asks.
    return git_credential_ssh_key_from_agent(
        out, username_from_url != nullptr ? username_from_url : "git");
  }

  if ((allowed_types & GIT_CREDENTIAL_USERPASS_PLAINTEXT) != 0U) {
    if (ctx->credentials == nullptr || ctx->credentials->password.empty()) {
      git_error_set_str(GIT_ERROR_NET,
                        "this remote wants a token and gittop has not found one");
      return GIT_EAUTH;
    }
    return git_credential_userpass_plaintext_new(out, ctx->credentials->username.c_str(),
                                                 ctx->credentials->password.c_str());
  }

  git_error_set_str(GIT_ERROR_NET, "no authentication method gittop supports");
  return GIT_EAUTH;
}

void InstallCallbacks(git_remote_callbacks* callbacks, Context* ctx) {
  callbacks->payload = ctx;
  callbacks->credentials = CredentialCb;
  callbacks->transfer_progress = TransferProgressCb;
  callbacks->push_transfer_progress = PushTransferProgressCb;
  callbacks->push_update_reference = PushUpdateReferenceCb;
  callbacks->sideband_progress = SidebandCb;
}

TransferResult Failure(std::string summary, std::string detail, std::string hint = {}) {
  TransferResult result;
  result.ok = false;
  result.summary = std::move(summary);
  result.detail = std::move(detail);
  result.hint = std::move(hint);
  return result;
}

// Opens the worker's own handle. Sharing App's would mean two threads inside
// one git_repository, which libgit2 does not allow.
bool Open(const std::string& path, RepoHandle* repo, TransferResult* failure) {
  if (git_repository_open(repo->put(), path.c_str()) != 0) {
    *failure = Failure("could not open the repository", LastError());
    return false;
  }
  return true;
}

bool LookupRemote(git_repository* repo, const std::string& name, RemoteHandle* remote,
                  TransferResult* failure) {
  if (git_remote_lookup(remote->put(), repo, name.c_str()) != 0) {
    *failure = Failure("no remote called " + name, LastError());
    return false;
  }
  const char* url = git_remote_url(remote->get());
  std::string why_not;
  if (url != nullptr && !TransportAvailable(url, &why_not)) {
    *failure = Failure("cannot reach this remote", why_not,
                       "gittop reads the same remote over https without one");
    return false;
  }
  return true;
}

// The checked-out branch, or an empty string with `failure` filled for the two
// states that have no branch to act on.
bool CurrentBranch(git_repository* repo, RefHandle* head, std::string* name,
                   TransferResult* failure) {
  const int rc = git_repository_head(head->put(), repo);
  if (rc == GIT_EUNBORNBRANCH) {
    *failure = Failure("this branch has no commits yet", "there is nothing to send or receive");
    return false;
  }
  if (rc != 0) {
    *failure = Failure("could not read HEAD", LastError());
    return false;
  }
  if (git_repository_head_detached(repo) == 1) {
    *failure = Failure("HEAD is detached", "check out a branch first",
                       "fetch still works and updates every tracking ref");
    return false;
  }
  const char* shorthand = git_reference_shorthand(head->get());
  *name = shorthand == nullptr ? "" : shorthand;
  return true;
}

std::string Plural(std::size_t n, const char* one, const char* many) {
  return std::to_string(n) + " " + (n == 1 ? one : many);
}

// "3 behind", "2 ahead and 1 behind", or empty when the branch is level. Read
// after a fetch, which is the moment the numbers become news.
std::string DivergenceNote(git_repository* repo, git_reference* head) {
  RefHandle upstream;
  if (git_branch_upstream(upstream.put(), head) != 0) {
    return {};
  }
  const git_oid* local = git_reference_target(head);
  const git_oid* remote = git_reference_target(upstream.get());
  if (local == nullptr || remote == nullptr) {
    return {};
  }
  std::size_t ahead = 0;
  std::size_t behind = 0;
  if (git_graph_ahead_behind(&ahead, &behind, repo, local, remote) != 0) {
    return {};
  }
  if (ahead == 0 && behind == 0) {
    return {};
  }
  if (ahead > 0 && behind > 0) {
    return Plural(ahead, "commit", "commits") + " ahead, " + std::to_string(behind) + " behind";
  }
  return ahead > 0 ? Plural(ahead, "commit", "commits") + " ahead"
                   : Plural(behind, "commit", "commits") + " behind";
}

// A fetch summary that says what actually came down rather than "done".
std::string ReceivedNote(const git_indexer_progress* stats) {
  if (stats == nullptr || stats->received_objects == 0) {
    return "everything was already here";
  }
  const double mib = static_cast<double>(stats->received_bytes) / (1024.0 * 1024.0);
  std::string note = Plural(stats->received_objects, "object", "objects");
  if (mib >= 0.1) {
    std::string size = std::to_string(mib);
    size = size.substr(0, size.find('.') + 2);
    note += ", " + size + " MiB";
  }
  return note;
}

// Every remote-tracking ref for one remote, short form ("origin/topic"),
// sorted. Read before and after a pruning fetch: the difference between the two
// is exactly what was pruned, which is a better thing to report than a count
// derived from sizes — a fetch creates tracking refs as well as removing them,
// so the two lists are not nested and subtracting their sizes would undercount.
std::vector<std::string> TrackingRefs(git_repository* repo, const std::string& remote_name) {
  const std::string root = "refs/remotes/";
  const std::string prefix = root + remote_name + "/";

  std::vector<std::string> names;
  git_strarray list = {};
  if (git_reference_list(&list, repo) != 0) {
    return names;
  }
  for (std::size_t i = 0; i < list.count; ++i) {
    const std::string ref = list.strings[i];
    if (ref.rfind(prefix, 0) != 0) {
      continue;
    }
    // refs/remotes/<name>/HEAD is the remote's default branch as a symbolic
    // ref, not a branch anybody tracks, and prune leaves it alone. Counting it
    // would report a prune that never happened.
    if (ref.compare(prefix.size(), std::string::npos, "HEAD") == 0) {
      continue;
    }
    names.push_back(ref.substr(root.size()));
  }
  git_strarray_dispose(&list);

  std::sort(names.begin(), names.end());
  return names;
}

TransferResult RunFetch(git_repository* repo, git_remote* remote, Context* ctx, bool prune) {
  git_fetch_options options;
  git_fetch_options_init(&options, GIT_FETCH_OPTIONS_VERSION);
  InstallCallbacks(&options.callbacks, ctx);
  // Unless the user asked for a prune by name, whatever the repository's own
  // remote.<name>.prune says. Deciding on its own to delete someone's tracking
  // refs is not a dashboard's call to make; doing it when asked is.
  options.prune = prune ? GIT_FETCH_PRUNE : GIT_FETCH_PRUNE_UNSPECIFIED;
  options.download_tags = GIT_REMOTE_DOWNLOAD_TAGS_AUTO;

  ctx->progress.phase = TransferPhase::Connecting;
  Publish(ctx);

  if (git_remote_fetch(remote, nullptr, &options, "gittop: fetch") != 0) {
    if (Cancelled(ctx)) {
      return Failure("cancelled", {});
    }
    return Failure("fetch failed", LastError());
  }

  TransferResult result;
  result.ok = true;
  result.detail = ReceivedNote(git_remote_stats(remote));

  RefHandle head;
  std::string branch;
  TransferResult ignored;
  if (CurrentBranch(repo, &head, &branch, &ignored)) {
    const std::string note = DivergenceNote(repo, head.get());
    result.summary = note.empty() ? "up to date with " + branch : branch + " is " + note;
    result.no_op = note.empty();
  } else {
    result.summary = "fetched";
  }
  return result;
}

}  // namespace

void ProgressSink::Publish(const TransferProgress& progress) {
  const std::lock_guard<std::mutex> lock(mutex_);
  progress_ = progress;
}

TransferProgress ProgressSink::Read() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  return progress_;
}

bool TransportAvailable(const std::string& url, std::string* why_not) {
  const int features = git_libgit2_features();
  const auto starts_with = [&url](const char* prefix) {
    const std::string p = prefix;
    return url.size() >= p.size() && url.compare(0, p.size(), p) == 0;
  };

  // scp-style host:path is ssh, and is how git writes a remote by default.
  const bool scp_style = !starts_with("http://") && !starts_with("https://") &&
                         !starts_with("file://") && !starts_with("/") &&
                         url.find(':') != std::string::npos && url.find("://") == std::string::npos;

  if (starts_with("ssh://") || starts_with("git+ssh://") || scp_style) {
    if ((features & GIT_FEATURE_SSH) == 0) {
      *why_not = "this build of gittop has no ssh transport";
      return false;
    }
    return true;
  }
  if (starts_with("https://")) {
    if ((features & GIT_FEATURE_HTTPS) == 0) {
      *why_not = "this build of gittop has no https transport";
      return false;
    }
    return true;
  }
  // http://, file://, git://, and a bare local path all need nothing special.
  return true;
}

std::string TransportSummary() {
  const int features = git_libgit2_features();
  std::string out;
  if ((features & GIT_FEATURE_HTTPS) != 0) {
    const char* backend = git_libgit2_feature_backend(GIT_FEATURE_HTTPS);
    out += std::string("https (") + (backend == nullptr ? "?" : backend) + ")";
  }
  if ((features & GIT_FEATURE_SSH) != 0) {
    const char* backend = git_libgit2_feature_backend(GIT_FEATURE_SSH);
    out += out.empty() ? "" : ", ";
    out += std::string("ssh (") + (backend == nullptr ? "?" : backend) + ")";
  }
  return out.empty() ? "none — this build cannot reach a network remote" : out;
}

TransferResult Fetch(const std::string& repo_path, const std::string& remote_name,
                     const Credentials& credentials, std::shared_ptr<ProgressSink> sink,
                     const std::atomic<bool>* cancel) {
  RepoHandle repo;
  TransferResult failure;
  if (!Open(repo_path, &repo, &failure)) {
    return failure;
  }
  RemoteHandle remote;
  if (!LookupRemote(repo.get(), remote_name, &remote, &failure)) {
    return failure;
  }

  Context ctx;
  ctx.sink = sink.get();
  ctx.cancel = cancel;
  ctx.credentials = &credentials;

  TransferResult result = RunFetch(repo.get(), remote.get(), &ctx, /*prune=*/false);
  ctx.progress.phase = result.ok ? TransferPhase::Done : TransferPhase::Failed;
  Publish(&ctx);
  return result;
}

TransferResult Prune(const std::string& repo_path, const std::string& remote_name,
                     const Credentials& credentials, std::shared_ptr<ProgressSink> sink,
                     const std::atomic<bool>* cancel) {
  RepoHandle repo;
  TransferResult failure;
  if (!Open(repo_path, &repo, &failure)) {
    return failure;
  }
  RemoteHandle remote;
  if (!LookupRemote(repo.get(), remote_name, &remote, &failure)) {
    return failure;
  }

  Context ctx;
  ctx.sink = sink.get();
  ctx.cancel = cancel;
  ctx.credentials = &credentials;

  // Read across the fetch rather than asking the server twice. git_remote_ls
  // would name the branches that still exist, but only while the connection is
  // open, and git_remote_fetch closes it — so this would mean a second connect
  // to learn something the prune itself already decides.
  const std::vector<std::string> before = TrackingRefs(repo.get(), remote_name);
  TransferResult result = RunFetch(repo.get(), remote.get(), &ctx, /*prune=*/true);
  ctx.progress.phase = result.ok ? TransferPhase::Done : TransferPhase::Failed;
  Publish(&ctx);
  if (!result.ok) {
    return result;
  }
  const std::vector<std::string> after = TrackingRefs(repo.get(), remote_name);

  std::vector<std::string> pruned;
  std::set_difference(before.begin(), before.end(), after.begin(), after.end(),
                      std::back_inserter(pruned));

  if (pruned.empty()) {
    // Not a failure and not a fetch summary either: the question asked was
    // "is anything here stale", and "no" is a real answer worth printing.
    result.summary = "nothing to prune";
    result.detail = "every remote-tracking ref still has a branch behind it";
    result.no_op = true;
    return result;
  }

  result.summary = Plural(pruned.size(), "stale ref", "stale refs") + " pruned";
  result.no_op = false;

  // Named, up to three. Which branches went is the whole content of the answer,
  // and a bare count leaves the user to diff two things they cannot see.
  constexpr std::size_t kNamed = 3;
  std::string detail;
  for (std::size_t i = 0; i < pruned.size() && i < kNamed; ++i) {
    detail += (i == 0 ? "" : ", ") + pruned[i];
  }
  if (pruned.size() > kNamed) {
    detail += " and " + std::to_string(pruned.size() - kNamed) + " more";
  }
  result.detail = std::move(detail);
  return result;
}

TransferResult Pull(const std::string& repo_path, const std::string& remote_name,
                    const Credentials& credentials, std::shared_ptr<ProgressSink> sink,
                    const std::atomic<bool>* cancel) {
  RepoHandle repo;
  TransferResult failure;
  if (!Open(repo_path, &repo, &failure)) {
    return failure;
  }
  RefHandle head;
  std::string branch;
  if (!CurrentBranch(repo.get(), &head, &branch, &failure)) {
    return failure;
  }
  RemoteHandle remote;
  if (!LookupRemote(repo.get(), remote_name, &remote, &failure)) {
    return failure;
  }

  Context ctx;
  ctx.sink = sink.get();
  ctx.cancel = cancel;
  ctx.credentials = &credentials;

  TransferResult fetched = RunFetch(repo.get(), remote.get(), &ctx, /*prune=*/false);
  if (!fetched.ok) {
    ctx.progress.phase = TransferPhase::Failed;
    Publish(&ctx);
    return fetched;
  }

  RefHandle upstream;
  if (git_branch_upstream(upstream.put(), head.get()) != 0) {
    return Failure(branch + " has no upstream", "nothing to merge into it",
                   "push it once and gittop will set one");
  }

  ctx.progress.phase = TransferPhase::Updating;
  Publish(&ctx);

  AnnotatedHandle theirs;
  if (git_annotated_commit_from_ref(theirs.put(), repo.get(), upstream.get()) != 0) {
    return Failure("could not read the upstream", LastError());
  }

  git_merge_analysis_t analysis = GIT_MERGE_ANALYSIS_NONE;
  git_merge_preference_t preference = GIT_MERGE_PREFERENCE_NONE;
  const git_annotated_commit* heads[1] = {theirs.get()};
  if (git_merge_analysis(&analysis, &preference, repo.get(), heads, 1) != 0) {
    return Failure("could not work out how to merge", LastError());
  }

  if ((analysis & GIT_MERGE_ANALYSIS_UP_TO_DATE) != 0) {
    TransferResult result;
    result.ok = true;
    result.no_op = true;
    result.summary = branch + " is already up to date";
    result.detail = fetched.detail;
    ctx.progress.phase = TransferPhase::Done;
    Publish(&ctx);
    return result;
  }

  // Anything that is not a fast-forward needs a merge commit or a rebase, and
  // gittop does neither yet. Doing one implicitly behind a key called "pull" is
  // how a tool loses work that was never committed anywhere else.
  if ((analysis & GIT_MERGE_ANALYSIS_FASTFORWARD) == 0) {
    return Failure(branch + " and its upstream have diverged",
                   "gittop only fast-forwards; this needs a merge or a rebase",
                   "the fetch already landed, so `git rebase` or `git merge` will work now");
  }

  ObjectHandle target;
  if (git_object_lookup(target.put(), repo.get(), git_annotated_commit_id(theirs.get()),
                        GIT_OBJECT_COMMIT) != 0) {
    return Failure("could not read the commit to move to", LastError());
  }

  git_checkout_options checkout;
  git_checkout_options_init(&checkout, GIT_CHECKOUT_OPTIONS_VERSION);
  // SAFE, not FORCE: a fast-forward that would write over an uncommitted change
  // stops here and says so rather than winning.
  checkout.checkout_strategy = GIT_CHECKOUT_SAFE;

  if (git_checkout_tree(repo.get(), target.get(), &checkout) != 0) {
    return Failure("could not fast-forward " + branch, LastError(),
                   "commit or stash the local changes in the way, then pull again");
  }

  RefHandle moved;
  if (git_reference_set_target(moved.put(), head.get(),
                               git_annotated_commit_id(theirs.get()),
                               "gittop: fast-forward") != 0) {
    return Failure("could not move " + branch, LastError());
  }
  if (git_repository_set_head(repo.get(), git_reference_name(moved.get())) != 0) {
    return Failure("could not update HEAD", LastError());
  }

  TransferResult result;
  result.ok = true;
  result.summary = "fast-forwarded " + branch;
  result.detail = fetched.detail;
  ctx.progress.phase = TransferPhase::Done;
  Publish(&ctx);
  return result;
}

TransferResult Push(const std::string& repo_path, const std::string& remote_name,
                    bool set_upstream, const Credentials& credentials,
                    std::shared_ptr<ProgressSink> sink, const std::atomic<bool>* cancel) {
  RepoHandle repo;
  TransferResult failure;
  if (!Open(repo_path, &repo, &failure)) {
    return failure;
  }
  RefHandle head;
  std::string branch;
  if (!CurrentBranch(repo.get(), &head, &branch, &failure)) {
    return failure;
  }
  RemoteHandle remote;
  if (!LookupRemote(repo.get(), remote_name, &remote, &failure)) {
    return failure;
  }

  // Where it lands on the other side. An upstream of refs/remotes/origin/trunk
  // means this branch is called trunk there, whatever it is called here.
  std::string remote_branch = branch;
  RefHandle upstream;
  const bool has_upstream = git_branch_upstream(upstream.put(), head.get()) == 0;
  if (has_upstream) {
    const char* shorthand = git_reference_shorthand(upstream.get());
    if (shorthand != nullptr) {
      const std::string full = shorthand;
      const std::size_t slash = full.find('/');
      if (slash != std::string::npos && slash + 1 < full.size()) {
        remote_branch = full.substr(slash + 1);
      }
    }
  }

  // No leading '+'. A force push can discard commits that exist nowhere else,
  // and the guardrail rule says nothing destructive gets a single keystroke —
  // so gittop does not offer one at all yet.
  const std::string refspec = "refs/heads/" + branch + ":refs/heads/" + remote_branch;
  std::vector<char*> specs;
  std::string spec_storage = refspec;
  specs.push_back(spec_storage.data());
  git_strarray refspecs = {specs.data(), specs.size()};

  Context ctx;
  ctx.sink = sink.get();
  ctx.cancel = cancel;
  ctx.credentials = &credentials;
  ctx.progress.phase = TransferPhase::Connecting;
  Publish(&ctx);

  git_push_options options;
  git_push_options_init(&options, GIT_PUSH_OPTIONS_VERSION);
  InstallCallbacks(&options.callbacks, &ctx);

  if (git_remote_push(remote.get(), &refspecs, &options) != 0) {
    ctx.progress.phase = TransferPhase::Failed;
    Publish(&ctx);
    if (Cancelled(&ctx)) {
      return Failure("cancelled", {});
    }
    return Failure("push failed", LastError());
  }

  // git_remote_push returns zero for a push the server refused. The refusal
  // arrives through push_update_reference and nowhere else, so a push that is
  // not checked here reports success for a rejected non-fast-forward.
  if (!ctx.rejections.empty()) {
    std::string detail;
    for (const std::string& rejection : ctx.rejections) {
      detail += detail.empty() ? rejection : "; " + rejection;
    }
    ctx.progress.phase = TransferPhase::Failed;
    Publish(&ctx);
    return Failure(remote_name + " refused the push", detail,
                   "pull first — gittop never force-pushes");
  }

  TransferResult result;
  result.ok = true;
  result.summary = "pushed " + branch + " to " + remote_name;
  result.detail = ctx.progress.remote_message;
  result.no_op = !ctx.any_ref_updated;
  if (result.no_op) {
    result.summary = remote_name + "/" + remote_branch + " was already up to date";
  }

  // Same as `git push -u`, and only for a branch that had none: an existing
  // upstream is a decision somebody already made.
  if (set_upstream && !has_upstream && !result.no_op) {
    const std::string tracking = remote_name + "/" + remote_branch;
    if (git_branch_set_upstream(head.get(), tracking.c_str()) == 0) {
      result.detail = result.detail.empty() ? "tracking " + tracking
                                            : result.detail + " · tracking " + tracking;
    }
  }

  ctx.progress.phase = TransferPhase::Done;
  Publish(&ctx);
  return result;
}

}  // namespace gittop::git
