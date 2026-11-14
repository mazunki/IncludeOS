#pragma once
#include <coroutine>
#include <optional>
#include <exception>
#include <utility>
#include <deque>
#include <cstddef>

struct Task {
  struct promise_type {
    Task get_return_object() {
      return Task{std::coroutine_handle<promise_type>::from_promise(*this)};
    }
    std::suspend_always initial_suspend() noexcept { return {}; }
    std::suspend_always final_suspend() noexcept { return {}; }
    void return_void() noexcept {}
    void unhandled_exception() { std::terminate(); }
  };

  std::coroutine_handle<promise_type> handle;

  explicit Task(std::coroutine_handle<promise_type> h) : handle(h) {}
  ~Task() { if (handle) handle.destroy(); }
  Task(const Task&) = delete;

  void start() {
    handle.resume();
  }
};

template <typename T>
class Channel {
public:
  explicit Channel(std::size_t capacity) : capacity_(capacity) {}

  struct Sender {
    Channel& ch;
    T value;
    bool await_ready() noexcept {
      return false;
    }

    std::coroutine_handle<> await_suspend(std::coroutine_handle<> h) noexcept {
      ch.buffer_.push_back(std::move(value));

      if (ch.buffer_.size() <= ch.capacity_) {
        if (ch.waiting_consumer_) {
          auto c = std::exchange(ch.waiting_consumer_, {});
          c.resume();
        }

        return h;
      }
      ch.waiting_producer_ = h;
      return std::noop_coroutine(); // block until receive() makes room
    }
    void await_resume() noexcept {}
  };

  struct Receiver {
    Channel& ch;
    std::optional<T> value;
    bool await_ready() noexcept {
      return false;
    }

    std::coroutine_handle<> await_suspend(std::coroutine_handle<> h) noexcept {
      if (!ch.buffer_.empty()) {
        value = std::move(ch.buffer_.front());
        ch.buffer_.pop_front();

        if (ch.waiting_producer_) {
          auto p = std::exchange(ch.waiting_producer_, {});
          p.resume();
        }
        return h;
      }

      if (ch.closed_) return h;

      ch.waiting_consumer_ = h;
      return std::noop_coroutine();
    }
    std::optional<T> await_resume() noexcept { return std::move(value); }
  };

  Sender send(T v) noexcept { return Sender{*this, std::move(v)}; }
  Receiver receive() noexcept { return Receiver{*this}; }

  void close() noexcept {
    closed_ = true;
    if (waiting_consumer_) {
      auto c = std::exchange(waiting_consumer_, {});
      c.resume();
    }
  }

private:
  std::size_t capacity_;
  std::deque<T> buffer_;
  std::coroutine_handle<> waiting_producer_{};
  std::coroutine_handle<> waiting_consumer_{};
  bool closed_{false};
};

