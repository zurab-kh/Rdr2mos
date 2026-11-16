// Example of a test in its own file: include the harness and register with COOP_TEST.
#include "harness.h"

COOP_TEST(testRegistrySmoke, "harness: tests in separate files are registered") {
    Cluster c;
    Player& a = c.join("Solo", true);
    c.run(800);
    CHECK(a.session->status().welcomed && a.session->status().worldOwner);
}
