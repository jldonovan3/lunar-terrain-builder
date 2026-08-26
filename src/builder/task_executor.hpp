#pragma once

#include <cstdint>
#include <functional>
#include <span>
#include <stop_token>

#include <lunar/terrain/result.hpp>

namespace lunar::terrain::builder {

using BuilderTask = std::function<Result<void>()>;

// Executes at most max_parallelism tasks concurrently. Results are observed in
// input order so worker scheduling cannot change the reported failure.
[[nodiscard]] Result<void> run_bounded_tasks(
    std::span<const BuilderTask> tasks,
    std::uint32_t max_parallelism,
    std::stop_token cancellation = {});

}  // namespace lunar::terrain::builder
