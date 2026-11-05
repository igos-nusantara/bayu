// SPDX-License-Identifier: BSD-3-Clause

#include <gtest/gtest.h>

#include <chrono>
#include <future>
#include <memory>
#include <thread>

#include "base/event_loop.h"
#include "bayu/packagekit/packagekit.h"

using namespace bayu;
using namespace bayu::base;
using namespace bayu::base::dbus;
using namespace bayu::packagekit;
using namespace std::chrono_literals;

namespace {

constexpr const char* kService = "org.freedesktop.PackageKit";

// PackageKit on the session bus, on a thread of its own: one transaction at
// /1, which lists two updates or downloads them.
class FakePackageKit
{
public:
    FakePackageKit()
    {
        std::promise<bool> ready;
        m_thread = std::thread([this, &ready] { run(ready); });
        m_ready = ready.get_future().get();
    }
    ~FakePackageKit()
    {
        m_handle.post([this] { m_loop->quit(); });
        m_thread.join();
    }

    bool isReady() const { return m_ready; }

    // What the client asked, read on the service's thread after the fact.
    std::vector<std::string> hints;
    uint64_t flags = 0;
    std::vector<std::string> downloaded;
    bool fail = false;
    // A package from a repository without signatures.
    bool unsigned_ = false;
    int updateCalls = 0;
    // RefreshCache's force, -1 before any.
    int forced = -1;

private:
    void run(std::promise<bool>& ready)
    {
        EventLoop loop;
        m_loop = &loop;
        m_handle = loop.handle();
        std::unique_ptr<Connection> connection = Connection::open(Connection::Bus::Session, &loop);
        if (!connection || !connection->requestName(kService)) {
            ready.set_value(false);
            return;
        }
        auto daemon = std::make_unique<Object>(connection.get(), "/org/freedesktop/PackageKit", kService);
        daemon->addMethod("CreateTransaction", [] { return ObjectPath {"/1"}; });
        auto transaction = std::make_unique<Object>(connection.get(), "/1", "org.freedesktop.PackageKit.Transaction");
        Object* t = transaction.get();
        transaction->addSignal("Package", "uss");
        transaction->addSignal("ItemProgress", "suu");
        transaction->addSignal("ErrorCode", "us");
        transaction->addSignal("Finished", "uu");
        transaction->addProperty("Percentage", [this] { return m_percentage; });
        transaction->addMethod("SetHints", [this](const std::vector<std::string>& given) { hints = given; });
        transaction->addMethod("RefreshCache", [this, t](bool force) {
            forced = force;
            t->emitSignal("Finished", uint32_t(1), uint32_t(10));
        });
        transaction->addMethod("GetUpdates", [this, t](uint64_t) {
            t->emitSignal("Package", uint32_t(8), std::string("openssh;9.9p1-29;x86_64;igos-baseos"),
                          std::string("An open source implementation of SSH"));
            t->emitSignal("Package", uint32_t(5), std::string("bayu-desktop;0.3.8-1.el10;x86_64;bayu"),
                          std::string("The Bayu desktop"));
            t->emitSignal("Finished", uint32_t(1), uint32_t(10));
        });
        transaction->addMethod("UpdatePackages", [this, t](uint64_t given, const std::vector<std::string>& ids) {
            flags = given;
            downloaded = ids;
            ++updateCalls;
            if (unsigned_ && (given & (uint64_t(1) << 1))) {
                t->emitSignal("Finished", uint32_t(8), uint32_t(10));
                return;
            }
            m_percentage = 50;
            t->emitPropertiesChanged({"Percentage"});
            t->emitSignal("Package", uint32_t(10), ids.front(), std::string());
            if (fail) {
                t->emitSignal("ErrorCode", uint32_t(4), std::string("No network connection available"));
                t->emitSignal("Finished", uint32_t(2), uint32_t(10));
                return;
            }
            t->emitSignal("Finished", uint32_t(1), uint32_t(10));
        });
        ready.set_value(daemon->publish() && transaction->publish());
        loop.run();
    }

    std::thread m_thread;
    EventLoop* m_loop = nullptr;
    LoopHandle m_handle;
    bool m_ready = false;
    uint32_t m_percentage = 101;
};

class PackageKitTest : public testing::Test
{
protected:
    void SetUp() override
    {
        if (!Connection::session(&m_loop))
            GTEST_SKIP() << "No session bus; run the tests in dbus-run-session";
        m_daemon = std::make_unique<FakePackageKit>();
        ASSERT_TRUE(m_daemon->isReady());
    }

    bool waitFor(const std::function<bool()>& condition)
    {
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (!condition()) {
            if (std::chrono::steady_clock::now() > deadline)
                return false;
            const int timer = m_loop.startTimer(5ms, false, [this] { m_loop.quit(); });
            m_loop.run();
            m_loop.stopTimer(timer);
        }
        return true;
    }

    Connection* bus() { return Connection::session(&m_loop); }

    EventLoop m_loop;
    std::unique_ptr<FakePackageKit> m_daemon;
};

} // namespace

TEST(PackageTest, ReadsItsId)
{
    const Package package {"openssh;9.9p1-29;x86_64;igos-baseos", "", 8};
    EXPECT_EQ(package.name(), "openssh");
    EXPECT_EQ(package.version(), "9.9p1-29");
    EXPECT_EQ(package.repository(), "igos-baseos");
    EXPECT_TRUE(package.isSecurity());
}

TEST_F(PackageKitTest, ListsTheUpdates)
{
    Transaction transaction(bus());
    bool done = false;
    bool success = false;
    Subscription finished = transaction.finished.connect([&](bool ok, const std::string&) {
        done = true;
        success = ok;
    });
    transaction.getUpdates(true);
    ASSERT_TRUE(waitFor([&] { return done; }));
    EXPECT_TRUE(success);
    ASSERT_EQ(transaction.packages().size(), 2u);
    EXPECT_TRUE(transaction.packages()[0].isSecurity());
    EXPECT_EQ(transaction.packages()[1].name(), "bayu-desktop");
    EXPECT_FALSE(transaction.isRunning());
}

// The check a user asks for reads the repositories again (issue #123); the
// one in the background lets metadata that has not expired stand.
TEST_F(PackageKitTest, RefreshesTheCacheWhenAsked)
{
    for (const bool force : {true, false}) {
        Transaction transaction(bus());
        bool done = false;
        Subscription finished = transaction.finished.connect([&](bool, const std::string&) { done = true; });
        transaction.refreshCache(!force, force);
        ASSERT_TRUE(waitFor([&] { return done; }));
        EXPECT_EQ(m_daemon->forced, force ? 1 : 0);
    }
}

TEST_F(PackageKitTest, DownloadsOnlyAndTellsItsProgress)
{
    Transaction transaction(bus());
    bool done = false;
    bool success = false;
    int percentage = -1;
    Subscription progress = transaction.progressChanged.connect([&] {
        if (transaction.percentage() >= 0)
            percentage = transaction.percentage();
    });
    Subscription finished = transaction.finished.connect([&](bool ok, const std::string&) {
        done = true;
        success = ok;
    });
    transaction.downloadUpdates({"openssh;9.9p1-29;x86_64;igos-baseos"});
    ASSERT_TRUE(waitFor([&] { return done; }));
    EXPECT_TRUE(success);
    EXPECT_EQ(percentage, 50);
    EXPECT_EQ(transaction.currentPackage(), "openssh;9.9p1-29;x86_64;igos-baseos");
    // Trusted packages, downloaded only: the offline update installs them.
    EXPECT_EQ(m_daemon->flags, (uint64_t(1) << 1) | (uint64_t(1) << 3));
    EXPECT_EQ(m_daemon->downloaded, (std::vector<std::string> {"openssh;9.9p1-29;x86_64;igos-baseos"}));
    EXPECT_EQ(m_daemon->hints, (std::vector<std::string> {"interactive=true"}));
}

TEST_F(PackageKitTest, TellsWhyItFailed)
{
    m_daemon->fail = true;
    Transaction transaction(bus());
    std::string error;
    bool done = false;
    Subscription finished = transaction.finished.connect([&](bool ok, const std::string& why) {
        done = !ok;
        error = why;
    });
    transaction.downloadUpdates({"openssh;9.9p1-29;x86_64;igos-baseos"});
    ASSERT_TRUE(waitFor([&] { return done; }));
    EXPECT_EQ(error, "No network connection available");
}

TEST_F(PackageKitTest, AsksAgainForUnsignedPackages)
{
    m_daemon->unsigned_ = true;
    Transaction transaction(bus());
    bool done = false;
    bool success = false;
    Subscription finished = transaction.finished.connect([&](bool ok, const std::string&) {
        done = true;
        success = ok;
    });
    transaction.downloadUpdates({"ign-repos;2026.1-6;x86_64;igos-ign"});
    ASSERT_TRUE(waitFor([&] { return done; }));
    EXPECT_TRUE(success);
    EXPECT_EQ(m_daemon->updateCalls, 2);
    EXPECT_EQ(m_daemon->flags, uint64_t(1) << 3);
}

TEST_F(PackageKitTest, LeavesUnsignedPackagesInTheBackground)
{
    m_daemon->unsigned_ = true;
    Transaction transaction(bus());
    bool done = false;
    std::string error;
    Subscription finished = transaction.finished.connect([&](bool ok, const std::string& why) {
        done = !ok;
        error = why;
    });
    transaction.downloadUpdates({"ign-repos;2026.1-6;x86_64;igos-ign"}, true);
    ASSERT_TRUE(waitFor([&] { return done; }));
    EXPECT_EQ(m_daemon->updateCalls, 1);
    EXPECT_FALSE(error.empty());
}
