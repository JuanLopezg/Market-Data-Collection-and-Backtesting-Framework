#include <gtest/gtest.h>

#include "test_support.h"

// The test harness itself: the environment meson test provides and the temp folder helper.
TEST(TestSupport, MesonProvidesSourceRootAndServiceExecutables)
{
    EXPECT_TRUE(std::filesystem::exists(test_support::sourcePath("lib/README.md")));
    EXPECT_TRUE(std::filesystem::exists(test_support::builtExecutable("ALGOTRADING_BIN_STRATEGY_SERVICE")));
}

TEST(TestSupport, TempDirIsRemovedAfterUse)
{
    std::filesystem::path kept;
    {
        test_support::TempDir dir;
        kept = dir.path();
        test_support::writeFile(dir / "a/b.txt", "content");
        EXPECT_EQ(test_support::readFile(dir / "a/b.txt"), "content");
    }
    EXPECT_FALSE(std::filesystem::exists(kept));
}

TEST(TestSupport, DockerInfrastructureStartsAndRestarts)
{
    if (!test_support::dockerAvailable())
        GTEST_SKIP() << "Docker is not available";
    auto nats = test_support::DockerContainer::startNats();
    auto postgres = test_support::DockerContainer::startPostgres();
    EXPECT_NE(nats->url().find("nats://127.0.0.1:"), std::string::npos);
    postgres->restart();
    EXPECT_NE(postgres->url().find(std::to_string(postgres->hostPort())), std::string::npos);
}
