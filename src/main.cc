#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

#include "src/backend.h"
#include "src/common.h"
#include "src/redis_transport.h"
#include "src/store.h"
#include "src/transfer.h"

namespace vpp_json {
namespace {
volatile std::sig_atomic_t stop_requested = 0;
void Stop(int) { stop_requested = 1; }
[[noreturn]] void Fail(const std::string& message) {
  std::cerr << message << '\n';
  std::exit(1);
}
struct Options {
  std::string host = "127.0.0.1";
  int port = 6379;
  std::string prefix = "vpp";
  std::string state = "state";
  std::string clients;
  std::string backend = "vapi";
  std::string socket = "/run/vpp/api.sock";
  int mock_count = 100;
};
int ParseInt(const std::string& text, int minimum, int maximum) {
  char* end = nullptr;
  int64_t value = std::strtoll(text.c_str(), &end, 10);
  if (text.empty() || *end || value < minimum || value > maximum) {
    Fail("Invalid numeric option");
  }
  return value;
}
Options ParseOptions(int argc, char** argv) {
  Options options;
  for (int i = 1; i < argc; ++i) {
    std::string name = argv[i];
    if (name == "--help") {
      std::cout
          << "vpp-json-gateway --clients clients.json [--backend vapi|mock] "
             "[--state directory] [--redis-host host] [--redis-port port] "
             "[--namespace name] [--vpp-socket path] [--mock-count count]\n";
      std::exit(0);
    }
    if (++i == argc) {
      Fail("Missing option value");
    }
    std::string value = argv[i];
    if (name == "--clients") {
      options.clients = value;
    } else if (name == "--state") {
      options.state = value;
    } else if (name == "--backend") {
      options.backend = value;
    } else if (name == "--redis-host") {
      options.host = value;
    } else if (name == "--redis-port") {
      options.port = ParseInt(value, 1, 65535);
    } else if (name == "--namespace") {
      options.prefix = value;
    } else if (name == "--vpp-socket") {
      options.socket = value;
    } else if (name == "--mock-count") {
      options.mock_count = ParseInt(value, 0, 1000000);
    } else {
      Fail("Unknown option: " + name);
    }
  }
  if (options.clients.empty() || !IsId(options.prefix) ||
      (options.backend != "mock" && options.backend != "vapi")) {
    Fail("Invalid options; use --help");
  }
#ifndef WITH_VAPI
  if (options.backend == "vapi") {
    Fail(
        "This build has no VAPI support; rebuild with -DWITH_VAPI=ON or "
        "explicitly select --backend mock");
  }
#endif
  return options;
}
std::string SessionKey(const Options& options, const std::string& client,
                       const std::string& sid, const std::string& direction) {
  return options.prefix + ":session:{" + client + ":" + sid + "}:" + direction;
}
Json Capabilities(const Options& options) {
  return {{"protocol", 1},
          {"backend", options.backend},
          {"methods",
           {"api.describe", "api.replay", "session.renew", "interface.list",
            "interface.set_state", "transfer.begin", "transfer.chunk",
            "transfer.commit", "transfer.status"}},
          {"frame_bytes", Limits::kFrameBytes},
          {"response_window", Limits::kWindow},
          {"max_upload_bytes", Limits::kUploadBytes},
          {"max_response_bytes", Limits::kSpoolBytes},
          {"max_params_bytes", Limits::kFrameBytes},
          {"lease_seconds", Limits::kLeaseSeconds},
          {"request_retention", "session_lifetime"},
          {"max_requests_per_session", Limits::kJobsPerSession},
          {"generated_methods",
           options.backend == "vapi" ? Json("api.methods") : Json(nullptr)},
          {"events", false},
          {"snapshot", false}};
}
// Only this thread touches Backend, including construction and destruction.
class Worker {
 public:
  explicit Worker(const Options& options)
      : thread_([this, options] {
          std::unique_ptr<Backend> backend;
          if (options.backend == "mock") {
            backend = MakeMockBackend(options.mock_count);
          }
#ifdef WITH_VAPI
          else
            backend = MakeVapiBackend(options.socket);
#endif
          for (;;) {
            std::packaged_task<Status(Backend*)> task;
            {
              std::unique_lock<std::mutex> lock(mutex_);
              changed_.wait_for(lock, std::chrono::milliseconds(100),
                                [this] { return stopping_ || task_.valid(); });
              if (!stopping_ && !task_.valid()) {
                lock.unlock();
                backend->Poll();
                continue;
              }
              if (stopping_ && !task_.valid()) return;
              task = std::move(task_);
            }
            task(backend.get());
          }
        }) {
  }
  ~Worker() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stopping_ = true;
    }
    changed_.notify_one();
    thread_.join();
  }
  std::future<Status> Submit(std::function<Status(Backend*)> function) {
    std::lock_guard<std::mutex> lock(mutex_);
    task_ = std::packaged_task<Status(Backend*)>(std::move(function));
    auto future = task_.get_future();
    changed_.notify_one();
    return future;
  }

 private:
  std::mutex mutex_;
  std::condition_variable changed_;
  bool stopping_ = false;
  std::packaged_task<Status(Backend*)> task_;
  std::thread thread_;
};
struct Entry {
  std::string id;
  std::string body;
};
bool ReadFirst(RedisTransport* redis, const std::string& key, Entry* entry) {
  Reply reply = redis->Command({"XRANGE", key, "-", "+", "COUNT", "1"});
  if (!reply || reply->type != REDIS_REPLY_ARRAY || reply->elements == 0) {
    return false;
  }
  auto* row = reply->element[0];
  if (row->type != REDIS_REPLY_ARRAY || row->elements != 2 ||
      !row->element[0]->str) {
    return false;
  }
  entry->id.assign(row->element[0]->str, row->element[0]->len);
  entry->body.clear();
  auto* fields = row->element[1];
  if (fields->type != REDIS_REPLY_ARRAY) {
    return true;
  }
  for (std::size_t i = 0; i + 1 < fields->elements; i += 2) {
    auto* key_field = fields->element[i];
    auto* value = fields->element[i + 1];
    if (key_field->str &&
        std::string(key_field->str, key_field->len) == "json" && value->str) {
      entry->body.assign(value->str, value->len);
    }
  }
  return true;
}
void Remove(RedisTransport* redis, const std::string& key, const Entry& entry) {
  // If deletion is ambiguous the same entry is reread; the durable journal
  // prevents a second execution.
  redis->Command({"XDEL", key, entry.id});
}
void SyncDirectory(const std::string& path) {
  int fd = open(path.c_str(), O_RDONLY);
  if (fd < 0 || fsync(fd) != 0) {
    Fail("Cannot persist spool directory");
  }
  close(fd);
}
class ApiServer {
 public:
  ApiServer(Options options, Json clients, std::string owner)
      : options_(std::move(options)),
        clients_(std::move(clients)),
        owner_(std::move(owner)),
        store_(options_.state + "/journal.sqlite"),
        transfers_(options_.state + "/uploads"),
        redis_(options_.host, options_.port),
        worker_(options_) {
    Recover();
  }
  void Run() {
    std::cerr << "Gateway ready; backend=" << options_.backend << '\n';
    while (!stop_requested) {
      CompleteWorker();
      if (redis_.Connect() && OwnNamespace()) {
        if (Now() != last_cleanup_) {
          Cleanup();
          last_cleanup_ = Now();
        }
        Register();
        auto sessions = store_.Query(
            "SELECT sid,client FROM sessions WHERE expires>? ORDER BY rowid",
            {std::to_string(Now())});
        // One entry per session per turn bounds each client's admission share.
        for (const auto& session : sessions) {
          Receive(session[0], session[1]);
        }
        Deliver();
        StartJob();
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    // The worker has a bounded VAPI deadline. Retain its result on shutdown.
    if (future_.valid()) {
      future_.wait();
      CompleteWorker();
    }
  }

 private:
  bool OwnNamespace() {
    // No lease expiry: another state directory must not take ownership while
    // this process might still have a VPP operation in flight. The same durable
    // identity can reconnect after a crash. Administrative takeover is
    // explicit.
    static const std::string kScript =
        "local owner=redis.call('GET',KEYS[1]); "
        "if not owner then redis.call('SET',KEYS[1],ARGV[1]); return 1 end; "
        "if owner==ARGV[1] then return 1 else return 0 end";
    auto reply = redis_.Command(
        {"EVAL", kScript, "1", options_.prefix + ":owner", owner_});
    if (!reply) {
      return false;
    }
    if (reply->type != REDIS_REPLY_INTEGER || reply->integer != 1) {
      Fail(
          "Redis namespace is owned by another journal, or ownership check "
          "failed");
    }
    return true;
  }
  void MarkDone(const std::string& sid, const std::string& id,
                const std::string& path) {
    SyncDirectory(options_.state + "/spool");
    int64_t size = FileSize(path);
    if (size < 0) {
      Fail("Missing completed response spool");
    }
    store_.Query(
        "UPDATE jobs SET state='done',size=?,offset=0 WHERE sid=? AND id=?",
        {std::to_string(size), sid, id});
  }
  void Recover() {
    for (const auto& row : store_.Query(
             "SELECT sid,id,path,state FROM jobs WHERE state!='queued'")) {
      if (row[3] == "running" || FileSize(row[2]) < 0) {
        Spool spool(row[2], row[1]);
        Status status = spool.Single(Error(
            row[1],
            {"outcome_unknown",
             "Gateway interrupted; do not automatically repeat mutations"}));
        if (!status.ok()) {
          Fail(status.message);
        }
        MarkDone(row[0], row[1], row[2]);
      }
    }
  }
  void Register() {
    std::string key = options_.prefix + ":register";
    Entry entry;
    if (!ReadFirst(&redis_, key, &entry)) {
      return;
    }
    Json request;
    Status status = ParseRequest(entry.body, &request);
    if (!status.ok() || request["method"] != "session.register") {
      Remove(&redis_, key, entry);
      return;
    }
    Json params = request.value("params", Json::object());
    if (!params.contains("client") || !IsId(params["client"]) ||
        !params.contains("token") || !params["token"].is_string()) {
      Remove(&redis_, key, entry);
      return;
    }
    std::string client = params["client"];
    if (!clients_.contains(client) ||
        !EqualSecret(clients_[client], params["token"])) {
      Remove(&redis_, key, entry);
      return;
    }
    std::string reply_key = options_.prefix + ":bootstrap:" + client;
    std::string id = request["id"];
    Json reply;
    if (params.size() != 3 || !params.contains("protocol") ||
        params["protocol"] != 1) {
      reply = Error(id, {"unsupported_protocol",
                         "Registration requires client, token and protocol 1"});
    } else {
      auto existing = store_.Query(
          "SELECT sid,expires FROM sessions WHERE client=? AND registration=?",
          {client, id});
      if (existing.empty() &&
          store_.Number("SELECT COUNT(*) FROM sessions") >= Limits::kSessions) {
        reply = Error(id, {"quota_exceeded", "Session limit reached"});
      } else {
        std::string sid = existing.empty() ? RandomId() : existing[0][0];
        std::string expires =
            existing.empty() ? std::to_string(Now() + Limits::kLeaseSeconds)
                             : existing[0][1];
        if (existing.empty()) {
          store_.Query("INSERT INTO sessions VALUES(?,?,?,?)",
                       {sid, client, id, expires});
        }
        Json result = Capabilities(options_);
        result["session"] = sid;
        result["expires_at"] = std::strtoll(expires.c_str(), nullptr, 10);
        result["requests"] = SessionKey(options_, client, sid, "requests");
        result["responses"] = SessionKey(options_, client, sid, "responses");
        reply = Result(id, result);
      }
    }
    reply["seq"] = 0;
    if (redis_.Publish(reply_key, reply.dump())) {
      Remove(&redis_, key, entry);
    }
  }
  void Receive(const std::string& sid, const std::string& client) {
    std::string key = SessionKey(options_, client, sid, "requests");
    std::string reply_key = SessionKey(options_, client, sid, "responses");
    Entry entry;
    if (!ReadFirst(&redis_, key, &entry)) {
      return;
    }
    redis_.Command({"EXPIRE", key, std::to_string(Limits::kLeaseSeconds)});
    Json request;
    Status status = ParseRequest(entry.body, &request);
    if (!status.ok()) {
      Json error = Error("invalid", status);
      error["seq"] = 0;
      if (redis_.Publish(reply_key, error.dump())) {
        Remove(&redis_, key, entry);
      }
      return;
    }
    std::string id = request["id"];
    // Upload chunks have their own durable (transfer, sequence, checksum)
    // identity, so their bulk data does not also occupy the request journal.
    if (request["method"] == "transfer.chunk" &&
        !request.contains("params_ref")) {
      Json result;
      status =
          transfers_.Handle(sid, "transfer.chunk",
                            request.value("params", Json::object()), &result);
      Json response = status.ok() ? Result(id, result) : Error(id, status);
      response["seq"] = 0;
      if (redis_.Publish(reply_key, response.dump())) {
        Remove(&redis_, key, entry);
      }
      return;
    }
    std::string body = request.dump();
    auto existing = store_.Query(
        "SELECT body,state FROM jobs WHERE sid=? AND id=?", {sid, id});
    if (!existing.empty()) {
      if (existing[0][0] != body) {
        status = {"id_conflict",
                  "Request ID already identifies a different request"};
      }
      // Duplicate delivery caused by ambiguous XDEL must not rewind output.
      // Explicit replay uses api.replay, below, and never invokes the backend.
    } else if (request["method"] == "api.replay") {
      Json params = request.value("params", Json::object());
      if (params.size() != 1 || !params.contains("request") ||
          !IsId(params["request"])) {
        status = {"invalid_params", "Replay requires the original request ID"};
      } else {
        auto found = store_.Query("SELECT state FROM jobs WHERE sid=? AND id=?",
                                  {sid, params["request"]});
        if (found.empty()) {
          status = {"resource_not_found",
                    "Request is not retained in this session"};
        } else if (found[0][0] == "done") {
          store_.Query("UPDATE jobs SET offset=0 WHERE sid=? AND id=?",
                       {sid, params["request"]});
        }
      }
    } else if (store_.Number("SELECT COUNT(*) FROM jobs WHERE sid=?", {sid}) >=
                   Limits::kJobsPerSession ||
               store_.Number("SELECT COALESCE(SUM(length(body)),0) FROM jobs") +
                       static_cast<int64_t>(body.size()) >
                   16 * 1024 * 1024) {
      status = {"quota_exceeded",
                "Retained request quota reached; use a new session after "
                "draining results"};
    } else {
      std::string path =
          options_.state + "/spool/" + Sha256(sid + ":" + id) + ".jsonl";
      store_.Query(
          "INSERT INTO jobs(sid,id,body,state,path,expires) "
          "VALUES(?,?,?,'queued',?,?)",
          {sid, id, body, path, std::to_string(Now() + Limits::kLeaseSeconds)});
    }
    if (!status.ok()) {
      Json error = Error(id, status);
      error["seq"] = 0;
      if (!redis_.Publish(reply_key, error.dump())) {
        return;
      }
    }
    Remove(&redis_, key, entry);
  }
  void StartJob() {
    if (future_.valid()) {
      return;
    }
    auto rows = store_.Query(
        "SELECT sid,id,body,path FROM jobs WHERE state='queued' ORDER BY rowid "
        "LIMIT 1");
    if (rows.empty()) {
      return;
    }
    auto row = rows[0];
    store_.Query("UPDATE jobs SET state='running' WHERE sid=? AND id=?",
                 {row[0], row[1]});
    Json request = Json::parse(row[2], nullptr, false);
    Json params = request.value("params", Json::object());
    Status status;
    if (request.contains("params_ref")) {
      status = transfers_.Resolve(row[0], request["params_ref"], &params);
      request.erase("params_ref");
      request["params"] = params;
    }
    std::string method = request["method"];
    Json result;
    bool immediate = true;
    if (!status.ok()) { /* Produce the resolution error below. */
    } else if (method == "api.describe" && params.empty()) {
      if (!params.empty()) {
        status = {"invalid_params", "api.describe takes no parameters"};
      } else {
        result = Capabilities(options_);
      }
    } else if (method == "session.renew") {
      if (!params.empty()) {
        status = {"invalid_params", "session.renew takes no parameters"};
      } else {
        int64_t expires = Now() + Limits::kLeaseSeconds;
        store_.Query("UPDATE sessions SET expires=? WHERE sid=?",
                     {std::to_string(expires), row[0]});
        result = {{"expires_at", expires}};
      }
    } else if (method.rfind("transfer.", 0) == 0) {
      status = transfers_.Handle(row[0], method, params, &result);
    } else {
      immediate = false;
    }
    if (immediate) {
      Spool spool(row[3], row[1]);
      Status written = spool.Single(status.ok() ? Result(row[1], result)
                                                : Error(row[1], status));
      if (!written.ok()) {
        Fail(written.message);
      }
      MarkDone(row[0], row[1], row[3]);
      return;
    }
    int64_t used = store_.Number("SELECT COALESCE(SUM(size),0) FROM jobs");
    int64_t session_used = store_.Number(
        "SELECT COALESCE(SUM(size),0) FROM jobs WHERE sid=?", {row[0]});
    int64_t available = std::min<int64_t>(256 * 1024 * 1024 - used,
                                          64 * 1024 * 1024 - session_used);
    active_sid_ = row[0];
    active_id_ = row[1];
    active_path_ = row[3];
    future_ =
        worker_.Submit([request, path = row[3], available](Backend* backend) {
          Spool spool(path, request["id"], std::max<int64_t>(1024, available));
          if (available < 4096) {
            return spool.Single(
                Error(request["id"],
                      {"quota_exceeded", "Response storage quota exhausted"}));
          }
          return RunBackend(backend, request, &spool);
        });
  }
  void CompleteWorker() {
    if (!future_.valid() || future_.wait_for(std::chrono::seconds(0)) !=
                                std::future_status::ready) {
      return;
    }
    Status status = future_.get();
    if (!status.ok()) {
      Fail(status.message);
    }
    MarkDone(active_sid_, active_id_, active_path_);
    active_sid_.clear();
    active_id_.clear();
    active_path_.clear();
  }
  void Deliver() {
    // At most one frame per session each turn; responses never block VAPI.
    for (const auto& session :
         store_.Query("SELECT sid,client FROM sessions ORDER BY rowid")) {
      auto rows = store_.Query(
          "SELECT id,path,offset FROM jobs WHERE sid=? AND state='done' AND "
          "offset<size ORDER BY rowid LIMIT 1",
          {session[0]});
      if (rows.empty()) {
        continue;
      }
      auto row = rows[0];
      std::ifstream file(row[1], std::ios::binary);
      auto offset = std::strtoll(row[2].c_str(), nullptr, 10);
      file.seekg(offset);
      std::string line;
      if (!std::getline(file, line)) {
        Fail("Cannot read retained response");
      }
      if (redis_.Publish(
              SessionKey(options_, session[1], session[0], "responses"),
              line)) {
        store_.Query(
            "UPDATE jobs SET offset=? WHERE sid=? AND id=?",
            {std::to_string(offset + line.size() + 1), session[0], row[0]});
      }
    }
  }
  void Cleanup() {
    transfers_.Expire();
    for (const auto& session :
         store_.Query("SELECT sid,client FROM sessions WHERE expires<=?",
                      {std::to_string(Now())})) {
      if (session[0] == active_sid_) {
        continue;
      }
      for (const auto& row :
           store_.Query("SELECT path FROM jobs WHERE sid=?", {session[0]})) {
        if (unlink(row[0].c_str()) != 0 && errno != ENOENT) {
          Fail("Cannot expire response spool");
        }
      }
      redis_.Command(
          {"DEL", SessionKey(options_, session[1], session[0], "requests"),
           SessionKey(options_, session[1], session[0], "responses")});
      store_.Query("DELETE FROM sessions WHERE sid=?", {session[0]});
    }
  }
  Options options_;
  Json clients_;
  std::string owner_;
  Store store_;
  TransferManager transfers_;
  RedisTransport redis_;
  Worker worker_;
  std::future<Status> future_;
  std::string active_sid_;
  std::string active_id_;
  std::string active_path_;
  int64_t last_cleanup_ = 0;
};
}  // namespace
int RunMain(int argc, char** argv) {
  umask(0077);
  std::signal(SIGINT, Stop);
  std::signal(SIGTERM, Stop);
  std::signal(SIGPIPE, SIG_IGN);
  Options options = ParseOptions(argc, argv);
  std::ifstream clients_file(options.clients);
  Json clients = Json::parse(clients_file, nullptr, false);
  if (!clients.is_object() || clients.empty()) {
    Fail("Expected clients JSON mapping client names to secrets");
  }
  for (auto it = clients.begin(); it != clients.end(); ++it) {
    if (!IsId(it.key()) || !it.value().is_string() ||
        it.value().get_ref<const std::string&>().size() < 32) {
      Fail("Client names must be IDs and secrets at least 32 characters");
    }
  }
  if (!MakeDirectories(options.state + "/spool")) {
    Fail("Cannot create state directory");
  }
  int lock =
      open((options.state + "/server.lock").c_str(), O_CREAT | O_RDWR, 0600);
  if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB) != 0) {
    Fail("Another gateway owns this state directory");
  }
  std::string identity_path = options.state + "/identity.json";
  std::ifstream identity_file(identity_path);
  Json identity = Json::parse(identity_file, nullptr, false);
  if (!identity.is_object()) {
    if (FileSize(identity_path) >= 0) {
      Fail("Invalid journal identity");
    }
    identity = {{"owner", RandomId()},
                {"namespace", options.prefix},
                {"host", options.host},
                {"port", options.port}};
    Status written = WriteAtomic(identity_path, identity.dump());
    if (!written.ok()) {
      Fail(written.message);
    }
  }
  if (identity.value("namespace", "") != options.prefix ||
      identity.value("host", "") != options.host ||
      identity["port"] != options.port || !identity.contains("owner") ||
      !IsId(identity["owner"])) {
    Fail("Journal belongs to a different Redis endpoint or namespace");
  }
  ApiServer server(options, clients, identity["owner"]);
  server.Run();
  close(lock);
  return 0;
}
}  // namespace vpp_json

int main(int argc, char** argv) { return vpp_json::RunMain(argc, argv); }
