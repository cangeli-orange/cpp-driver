/*
  Copyright 2014 DataStax

  Licensed under the Apache License, Version 2.0 (the "License");
  you may not use this file except in compliance with the License.
  You may obtain a copy of the License at

  http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.
*/

#ifndef __CASS_SESSION_HPP_INCLUDED__
#define __CASS_SESSION_HPP_INCLUDED__

#include <uv.h>

#include <atomic>
#include <list>
#include <string>
#include <vector>
#include <memory>
#include <set>

#include "mpmc_queue.hpp"
#include "spsc_queue.hpp"
#include "io_worker.hpp"
#include "load_balancing_policy.hpp"
#include "round_robin_policy.hpp"
#include "resolver.hpp"
#include "config.hpp"
#include "session_future.hpp"
#include "request_handler.hpp"

namespace cass {

struct Session {
  typedef std::shared_ptr<IOWorker> IOWorkerPtr;
  typedef std::vector<IOWorkerPtr> IOWorkerCollection;
  enum SessionState {
    SESSION_STATE_NEW,
    SESSION_STATE_CONNECTING,
    SESSION_STATE_READY,
    SESSION_STATE_DISCONNECTING,
    SESSION_STATE_DISCONNECTED
  };

  struct Payload {
    enum Type {
      ON_CONNECTED,
      ON_SHUTDOWN,
    };
    Type type;
    Host host;
  };

  std::atomic<SessionState> state_;
  uv_thread_t                  thread_;
  uv_loop_t*                   loop_;
  SSLContext*                  ssl_context_;
  uv_async_t                   async_connect_;
  IOWorkerCollection           io_workers_;
  std::string                  keyspace_;
  SessionFuture*               connect_future_;
  SessionFuture*               shutdown_future_;
  std::set<Host>               hosts_;
  Config                       config_;
  std::unique_ptr<AsyncQueue<MPMCQueue<RequestHandler*>>> request_queue_;
  std::unique_ptr<AsyncQueue<MPMCQueue<Payload>>> event_queue_;
  std::unique_ptr<LoadBalancingPolicy> load_balancing_policy_;
  int pending_resolve_count_;
  int pending_connections_count_;
  int pending_workers_count_;
  int current_io_worker_;

  Session() 
    : state_(SESSION_STATE_NEW)
    , loop_(uv_loop_new())
    , ssl_context_(nullptr)
    , connect_future_(nullptr)
    , shutdown_future_(nullptr)
    , load_balancing_policy_(new RoundRobinPolicy())
    , pending_resolve_count_(0)
    , pending_connections_count_(0)
    , pending_workers_count_(0)
    , current_io_worker_(0) { }

  Session(const Session* session)
    : state_(SESSION_STATE_NEW)
    , loop_(uv_loop_new())
    , ssl_context_(nullptr)
    , connect_future_(nullptr)
    , shutdown_future_(nullptr)
    , config_(session->config_)
    , load_balancing_policy_(new RoundRobinPolicy())
    , pending_resolve_count_(0)
    , pending_connections_count_(0)
    , pending_workers_count_(0)
    , current_io_worker_(0) { }

  ~Session() {
    uv_loop_delete(loop_);
  }

  int init() {
    async_connect_.data = this;
    uv_async_init( loop_, &async_connect_, &Session::on_connect);

    request_queue_.reset(new AsyncQueue<MPMCQueue<RequestHandler*>>(config_.queue_size_io()));
    int rc = request_queue_->init(loop_, this, &Session::on_execute);
    if(rc != 0) return rc;
    event_queue_.reset(new AsyncQueue<MPMCQueue<Payload>>(config_.queue_size_event()));
    rc = event_queue_->init(loop_, this, &Session::on_event);
    if(rc != 0) return rc;
    for (size_t i = 0; i < config_.thread_count_io(); ++i) {
      IOWorkerPtr io_worker(new IOWorker(this, config_));
      int rc = io_worker->init();
      if(rc != 0) return rc;
      io_workers_.push_back(io_worker);
    }
    return rc;
  }

  void join() {
    uv_thread_join(&thread_);
  }

  void notify_connect_q(const Host& host) {
    Payload payload;
    payload.type = Payload::ON_CONNECTED;
    payload.host = host;
    event_queue_->enqueue(payload);
  }

  void notify_shutdown_q() {
    Payload payload;
    payload.type = Payload::ON_SHUTDOWN;
    event_queue_->enqueue(payload);
  }

  static void
  on_run(
      void* data) {
    Session* session = reinterpret_cast<Session*>(data);

    for(auto io_worker : session->io_workers_) {
      io_worker->run();
    }

    uv_run(session->loop_, UV_RUN_DEFAULT);
  }

  Future*
  connect(
      const char* ks) {
    return connect(std::string(ks));
  }

  Future*
  connect(
      const std::string& ks) {
    connect_future_ = new SessionFuture();
    connect_future_->session = this;

    SessionState expected_state = SESSION_STATE_NEW;
    if (state_.compare_exchange_strong(
            expected_state,
            SESSION_STATE_CONNECTING)) {

      init();

      keyspace_ = ks;
      uv_thread_create(
          &thread_,
          &Session::on_run,
          this);

      uv_async_send(&async_connect_);
    } else {
     connect_future_->set_error(CASS_ERROR_LIB_ALREADY_CONNECTED, "Connect has already been called");
    }
    return connect_future_;
  }

  void init_pools() {
    int num_pools =  hosts_.size() * io_workers_.size();
    pending_connections_count_ = num_pools * config_.core_connections_per_host();
    for(auto& host : hosts_) {
      for(auto io_worker : io_workers_) {
        io_worker->add_pool_q(host);
      }
    }
  }

  Future* shutdown() {
    shutdown_future_ = new ShutdownSessionFuture();
    shutdown_future_->session = this;

    SessionState expected_state_ready = SESSION_STATE_READY;
    SessionState expected_state_connecting = SESSION_STATE_CONNECTING;
    if (state_.compare_exchange_strong(expected_state_ready, SESSION_STATE_DISCONNECTING) ||
        state_.compare_exchange_strong(expected_state_connecting, SESSION_STATE_DISCONNECTING)) {
      pending_workers_count_ = io_workers_.size();
      for(auto io_worker : io_workers_) {
        io_worker->shutdown_q();
      }
    } else {
      shutdown_future_->set_error(CASS_ERROR_LIB_NOT_CONNECTED, "The session is not connected");
    }

    return shutdown_future_;
  }

  static void
  on_connect(
      uv_async_t* data,
      int         status) {
    Session* session = reinterpret_cast<Session*>(data->data);

    int port = session->config_.port();
    for(std::string seed : session->config_.contact_points()) {
      Address address;
      if(Address::from_string(seed, port, &address)) {
        Host host(address);
        session->hosts_.insert(host);
      } else {
        session->pending_resolve_count_++;
        Resolver::resolve(session->loop_, seed, port, session, on_resolve);
      }
    }
    if(session->pending_resolve_count_ == 0) {
      session->init_pools();
    }
  }

  static void on_resolve(Resolver* resolver) {
    if(resolver->is_success()) {
      Session* session = reinterpret_cast<Session*>(resolver->data());
      Host host(resolver->address());
      session->hosts_.insert(host);
      if(--session->pending_resolve_count_ == 0) {
        session->init_pools();
      }
    } else {
      // TODO:(mpenick) log or handle
      fprintf(stderr, "Unable to resolve %s:%d\n", resolver->host().c_str(), resolver->port());
    }
  }

  static void on_event(uv_async_t* async, int status) {
    Session* session  = reinterpret_cast<Session*>(async->data);

    Payload payload;
    while(session->event_queue_->dequeue(payload)) {
      if(payload.type == Payload::ON_CONNECTED) {
        if(--session->pending_connections_count_ == 0) {
          session->load_balancing_policy_->init(session->hosts_);
          session->state_ = SESSION_STATE_READY;
          session->connect_future_->set_result();
          session->connect_future_ = nullptr;
        }
      } else if(payload.type == Payload::ON_SHUTDOWN) {
        for(auto io_worker : session->io_workers_) {
          if(io_worker->is_shutdown_done_) {
            io_worker->join();
          }
        }
        if(--session->pending_workers_count_ == 0) {
          session->shutdown_future_->set_result();
          session->shutdown_future_ = nullptr;
          session->state_ = SESSION_STATE_DISCONNECTED;
          uv_stop(session->loop_);
        }
      }
    }
  }

  SSLSession*
  ssl_session_new() {
    if (ssl_context_) {
      return ssl_context_->session_new();
    }
    return NULL;
  }

  Future* prepare(const char* statement, size_t length) {
    Message* request = new Message();
    request->opcode = CQL_OPCODE_PREPARE;
    PrepareRequest* prepare = new PrepareRequest();
    prepare->prepare_string(statement, length);
    request->body.reset(prepare);
    RequestHandler* request_handler
        = new RequestHandler(request);
    request_handler->future()->statement.assign(statement, length);
    execute(request_handler);
    return request_handler->future();
  }

  Future* execute(Statement* statement) {
    Message* request = new Message();
    request->opcode = statement->opcode();
    request->body.reset(statement);
    RequestHandler* request_handler = new RequestHandler(request);
    execute(request_handler);
    return request_handler->future();
  }

  inline void execute(RequestHandler* request_handler) {
    if (!request_queue_->enqueue(request_handler)) {
      request_handler->on_error(CASS_ERROR_LIB_REQUEST_QUEUE_FULL, "The request queue has reached capacity");
      delete request_handler;
    }
  }

  static void
  on_execute(
      uv_async_t* data,
      int         status) {
    Session* session = reinterpret_cast<Session*>(data->data);

    RequestHandler* request_handler = nullptr;
    while (session->request_queue_->dequeue(request_handler)) {
      session->load_balancing_policy_->new_query_plan(&request_handler->hosts);

      size_t start = session->current_io_worker_;
      size_t remaining = session->io_workers_.size();
      while(remaining != 0) {
        // TODO(mpenick): Make this something better than RR
        auto io_worker = session->io_workers_[start];
        if(io_worker->execute(request_handler)) {
          session->current_io_worker_ = (start + 1) % session->io_workers_.size();
          break;
        }
        start++;
        remaining--;
      }

      if(remaining == 0) {
        request_handler->on_error(CASS_ERROR_LIB_NO_AVAILABLE_IO_THREAD, "All workers are busy");
      }
    }
  }

  void
  set_keyspace() {
    // TODO(mstump)
  }

  void
  set_load_balancing_policy(LoadBalancingPolicy* policy) {
    load_balancing_policy_.reset(policy);
  }
};

} // namespace cass

#endif
