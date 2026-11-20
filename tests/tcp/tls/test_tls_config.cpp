#include <gtest/gtest.h>
#include "tcp/TlsConfig.hpp"

class TlsConfigTest : public ::testing::Test {};

TEST_F(TlsConfigTest, DefaultConstructionHasTlsDisabled) {
    tlcfi::tcp::TlsConfig config;
    EXPECT_FALSE(config.enabled);
    EXPECT_TRUE(config.certFile.empty());
    EXPECT_TRUE(config.keyFile.empty());
    EXPECT_TRUE(config.caFile.empty());
    EXPECT_TRUE(config.verifyPeer);  // Default is true for security
}

TEST_F(TlsConfigTest, ConstructionWithTlsEnabled) {
    tlcfi::tcp::TlsConfig config(true, "server.crt", "server.key", "ca.crt", false);
    EXPECT_TRUE(config.enabled);
    EXPECT_EQ(config.certFile, "server.crt");
    EXPECT_EQ(config.keyFile, "server.key");
    EXPECT_EQ(config.caFile, "ca.crt");
    EXPECT_FALSE(config.verifyPeer);
}

TEST_F(TlsConfigTest, ConstructionWithDefaultVerifyPeer) {
    tlcfi::tcp::TlsConfig config(true, "server.crt", "server.key");
    EXPECT_TRUE(config.enabled);
    EXPECT_EQ(config.certFile, "server.crt");
    EXPECT_EQ(config.keyFile, "server.key");
    EXPECT_TRUE(config.caFile.empty());
    EXPECT_TRUE(config.verifyPeer);  // Default is true
}

TEST_F(TlsConfigTest, ConstructionWithEmptyCaFile) {
    tlcfi::tcp::TlsConfig config(true, "server.crt", "server.key", "", true);
    EXPECT_TRUE(config.enabled);
    EXPECT_TRUE(config.caFile.empty());
}

TEST_F(TlsConfigTest, VerifyPeerCanBeDisabled) {
    tlcfi::tcp::TlsConfig config(true, "server.crt", "server.key", "ca.crt", false);
    EXPECT_FALSE(config.verifyPeer);
}

TEST_F(TlsConfigTest, VerifyPeerCanBeEnabled) {
    tlcfi::tcp::TlsConfig config(true, "server.crt", "server.key", "ca.crt", true);
    EXPECT_TRUE(config.verifyPeer);
}



