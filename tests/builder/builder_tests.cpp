#include <catch2/catch_test_macros.hpp>

#include "builder/builder.hpp"

TEST_CASE("builder library exposes its deterministic version") {
    CHECK(lunar::terrain::builder::version_string() == "0.1.0");
}
