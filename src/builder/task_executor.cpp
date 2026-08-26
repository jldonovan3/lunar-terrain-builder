#include "builder/task_executor.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <lunar/terrain/error.hpp>

namespace lunar::terrain::builder {
namespace {

[[nodiscard]] Result<void> cancelled() {
    return Result<void>::failure(Error{
        ErrorCode::cancelled, "terrain build was cancelled"});
}

}  // namespace

Result<void> run_bounded_tasks(
    const std::span<const BuilderTask> tasks,
    const std::uint32_t max_parallelism,
    const std::stop_token cancellation) {
    if (max_parallelism == 0) {
        return Result<void>::failure(Error{
            ErrorCode::invalid_argument, "bounded task parallelism must be positive"});
    }
    if (cancellation.stop_requested()) {
        return cancelled();
    }
    if (tasks.empty()) {
        return Result<void>::success();
    }

    std::vector<std::optional<Error>> failures(tasks.size());
    std::atomic<std::size_t> next_task{0};
    std::stop_source failure_stop;
    const std::size_t worker_count = std::min<std::size_t>(max_parallelism, tasks.size());
    std::vector<std::jthread> workers;
    workers.reserve(worker_count);
    for (std::size_t worker_index = 0; worker_index < worker_count; ++worker_index) {
        static_cast<void>(worker_index);
        workers.emplace_back([&]() {
            while (!failure_stop.stop_requested()) {
                const std::size_t index = next_task.fetch_add(1, std::memory_order_relaxed);
                if (index >= tasks.size()) {
                    return;
                }
                if (cancellation.stop_requested()) {
                    failures[index] = cancelled().error();
                    failure_stop.request_stop();
                    return;
                }
                try {
                    auto result = tasks[index]();
                    if (!result) {
                        failures[index] = std::move(result).error();
                        failure_stop.request_stop();
                        return;
                    }
                } catch (const std::exception& exception) {
                    failures[index] = Error{
                        ErrorCode::internal_error,
                        std::string{"builder tile task threw an exception: "} + exception.what()};
                    failure_stop.request_stop();
                    return;
                } catch (...) {
                    failures[index] = Error{
                        ErrorCode::internal_error, "builder tile task threw a non-standard exception"};
                    failure_stop.request_stop();
                    return;
                }
            }
        });
    }
    workers.clear();

    for (std::optional<Error>& failure : failures) {
        if (failure) {
            return Result<void>::failure(std::move(*failure));
        }
    }
    if (cancellation.stop_requested()) {
        return cancelled();
    }
    return Result<void>::success();
}

}  // namespace lunar::terrain::builder
