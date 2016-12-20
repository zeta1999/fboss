/*
 *  Copyright (c) 2004-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#include "fboss/agent/AddressUtil.h"
#include "fboss/agent/SwSwitch.h"
#include "fboss/agent/ThriftHandler.h"
#include "fboss/agent/test/TestUtils.h"
#include "fboss/agent/state/RouteUpdater.h"
#include "fboss/agent/hw/mock/MockPlatform.h"
#include "fboss/agent/state/SwitchState.h"
#include "fboss/agent/ApplyThriftConfig.h"
#include "fboss/agent/state/Route.h"

#include <folly/IPAddress.h>
#include <gtest/gtest.h>

using namespace facebook::fboss;
using folly::IPAddress;
using folly::IPAddressV4;
using folly::IPAddressV6;
using folly::StringPiece;
using std::unique_ptr;
using std::shared_ptr;
using testing::UnorderedElementsAreArray;
using facebook::network::toBinaryAddress;
using cfg::PortSpeed;

namespace {

unique_ptr<SwSwitch> setupSwitch() {
  auto state = testStateA();
  auto sw = createMockSw(state);
  sw->initialConfigApplied(std::chrono::steady_clock::now());
  return sw;
}

IpPrefix ipPrefix(StringPiece ip, int length) {
  IpPrefix result;
  result.ip = toBinaryAddress(IPAddress(ip));
  result.prefixLength = length;
  return result;
}

} // unnamed namespace

TEST(ThriftTest, getInterfaceDetail) {
  auto sw = setupSwitch();
  ThriftHandler handler(sw.get());

  // Query the two interfaces configured by testStateA()
  InterfaceDetail info;
  handler.getInterfaceDetail(info, 1);
  EXPECT_EQ("interface1", info.interfaceName);
  EXPECT_EQ(1, info.interfaceId);
  EXPECT_EQ(1, info.vlanId);
  EXPECT_EQ(0, info.routerId);
  EXPECT_EQ("00:02:00:00:00:01", info.mac);
  std::vector<IpPrefix> expectedAddrs = {
    ipPrefix("10.0.0.1", 24),
    ipPrefix("192.168.0.1", 24),
    ipPrefix("2401:db00:2110:3001::0001", 64),
  };
  EXPECT_THAT(info.address, UnorderedElementsAreArray(expectedAddrs));

  handler.getInterfaceDetail(info, 55);
  EXPECT_EQ("interface55", info.interfaceName);
  EXPECT_EQ(55, info.interfaceId);
  EXPECT_EQ(55, info.vlanId);
  EXPECT_EQ(0, info.routerId);
  EXPECT_EQ("00:02:00:00:00:55", info.mac);
  expectedAddrs = {
    ipPrefix("10.0.55.1", 24),
    ipPrefix("192.168.55.1", 24),
    ipPrefix("2401:db00:2110:3055::0001", 64),
  };
  EXPECT_THAT(info.address, UnorderedElementsAreArray(expectedAddrs));

  // Calling getInterfaceDetail() on an unknown
  // interface should throw an FbossError.
  EXPECT_THROW(handler.getInterfaceDetail(info, 123), FbossError);
}


TEST(ThriftTest, assertPortSpeeds) {
  // We rely on the exact value of the port speeds for some
  // logic, so we want to ensure that these values don't change.
  EXPECT_EQ(static_cast<int>(PortSpeed::GIGE), 1000);
  EXPECT_EQ(static_cast<int>(PortSpeed::XG), 10000);
  EXPECT_EQ(static_cast<int>(PortSpeed::TWENTYG), 20000);
  EXPECT_EQ(static_cast<int>(PortSpeed::TWENTYFIVEG), 25000);
  EXPECT_EQ(static_cast<int>(PortSpeed::FORTYG), 40000);
  EXPECT_EQ(static_cast<int>(PortSpeed::FIFTYG), 50000);
  EXPECT_EQ(static_cast<int>(PortSpeed::HUNDREDG), 100000);
}

TEST(ThriftTest, LinkLocalRoutes) {
  MockPlatform platform;
  auto stateV0 = testStateB();
  // Remove all linklocalroutes from stateV0 in order to clear all
  // linklocalroutes
  RouteUpdater updater(stateV0->getRouteTables());
  updater.delLinkLocalRoutes(RouterID(0));
  auto newRt = updater.updateDone();
  stateV0->resetRouteTables(newRt);
  cfg::SwitchConfig config;
  config.vlans.resize(1);
  config.vlans[0].id = 1;
  config.interfaces.resize(1);
  config.interfaces[0].intfID = 1;
  config.interfaces[0].vlanID = 1;
  config.interfaces[0].routerID = 0;
  config.interfaces[0].__isset.mac = true;
  config.interfaces[0].mac = "00:02:00:00:00:01";
  config.interfaces[0].ipAddresses.resize(3);
  config.interfaces[0].ipAddresses[0] = "10.0.0.1/24";
  config.interfaces[0].ipAddresses[1] = "192.168.0.1/24";
  config.interfaces[0].ipAddresses[2] = "2401:db00:2110:3001::0001/64";
  // Call applyThriftConfig
  auto stateV1 = publishAndApplyConfig(stateV0, &config, &platform);
  stateV1->publish();
  // Verify that stateV1 contains the link local route
  shared_ptr<RouteTable> rt = stateV1->getRouteTables()->
                              getRouteTableIf(RouterID(0));
  ASSERT_NE(nullptr, rt);
  // Link local addr.
  auto ip = IPAddressV6("fe80::");
  // Find longest match to link local addr.
  auto longestMatchRoute = rt->getRibV6()->longestMatch(ip);
  // Verify that a route is found
  ASSERT_NE(nullptr, longestMatchRoute);
  // Verify that the route is to link local addr.
  ASSERT_EQ(longestMatchRoute->prefix().network, ip);
}

std::unique_ptr<UnicastRoute>
makeUnicastRoute(std::string ip, uint8_t len, std::string nxtHop) {
  auto nr = std::make_unique<UnicastRoute>();
  nr->dest.ip = toBinaryAddress(IPAddress(ip));
  nr->dest.prefixLength = len;
  nr->nextHopAddrs.push_back(toBinaryAddress(IPAddress(nxtHop)));
  return nr;
}

// Test for the ThriftHandler::syncFib method
TEST(ThriftTest, syncFib) {
  RouterID rid = RouterID(0);

  // Create a config
  cfg::SwitchConfig config;
  config.vlans.resize(1);
  config.vlans[0].id = 1;
  config.interfaces.resize(1);
  config.interfaces[0].intfID = 1;
  config.interfaces[0].vlanID = 1;
  config.interfaces[0].routerID = 0;
  config.interfaces[0].__isset.mac = true;
  config.interfaces[0].mac = "00:02:00:00:00:01";
  config.interfaces[0].ipAddresses.resize(3);
  config.interfaces[0].ipAddresses[0] = "10.0.0.1/24";
  config.interfaces[0].ipAddresses[1] = "192.168.0.19/24";
  config.interfaces[0].ipAddresses[2] = "2401:db00:2110:3001::0001/64";

  // Create a mock SwSwitch using the config, and wrap it in a ThriftHandler
  auto mockSw = createMockSw(&config);
  mockSw->initialConfigApplied(std::chrono::steady_clock::now());
  mockSw->fibSynced();
  ThriftHandler handler(mockSw.get());

  // Add a few BGP routes
  handler.addUnicastRoute(0, makeUnicastRoute("7.7.7.7", 16, "99.99.99.99"));
  handler.addUnicastRoute(0, makeUnicastRoute("8.8.8.8", 16, "99.99.99.99"));
  handler.addUnicastRoute(0, makeUnicastRoute("aaaa::0", 64, "bbbb::0"));

  // Make sure all the static and link-local routes are there
  auto tables2 = handler.getSw()->getState()->getRouteTables();
  GET_ROUTE_V4(tables2, rid, "10.0.0.0/24");
  GET_ROUTE_V4(tables2, rid, "192.168.0.0/24");
  GET_ROUTE_V6(tables2, rid, "2401:db00:2110:3001::/64");
  GET_ROUTE_V6(tables2, rid, "fe80::/64");
  // Make sure the BGP routes are there.
  GET_ROUTE_V4(tables2, rid, "7.7.0.0/16");
  GET_ROUTE_V4(tables2, rid, "8.8.0.0/16");
  GET_ROUTE_V6(tables2, rid, "aaaa::0/64");
  // Make sure there are no more routes than the ones we just tested
  EXPECT_EQ(4, tables2->getRouteTable(rid)->getRibV4()->size());
  EXPECT_EQ(3, tables2->getRouteTable(rid)->getRibV6()->size());

  // Now use syncFib to replace all the BGP routes.
  // Statics and link-locals should remain unchanged.
  auto newRoutes = folly::make_unique<std::vector<UnicastRoute>>();
  UnicastRoute nr1 = *makeUnicastRoute("5.5.5.5", 8, "10.0.0.0").get();
  UnicastRoute nr2 = *makeUnicastRoute("6666::0", 128, "10.0.0.0").get();
  UnicastRoute nr3 = *makeUnicastRoute("7777::0", 128, "10.0.0.0").get();
  newRoutes->push_back(nr1);
  newRoutes->push_back(nr2);
  newRoutes->push_back(nr3);
  handler.syncFib(0, std::move(newRoutes));

  // Make sure all the static and link-local routes are still there
  auto tables3 = handler.getSw()->getState()->getRouteTables();
  GET_ROUTE_V4(tables3, rid, "10.0.0.0/24");
  GET_ROUTE_V4(tables3, rid, "192.168.0.0/24");
  GET_ROUTE_V6(tables3, rid, "2401:db00:2110:3001::/64");
  GET_ROUTE_V6(tables3, rid, "fe80::/64");
  // Make sure the new BGPd routes are there.
  GET_ROUTE_V4(tables3, rid, "5.0.0.0/8");
  GET_ROUTE_V6(tables3, rid, "6666::0/128");
  GET_ROUTE_V6(tables3, rid, "7777::0/128");
  // Make sure there are no more routes (ie. old ones were deleted)
  EXPECT_EQ(3, tables3->getRouteTable(rid)->getRibV4()->size());
  EXPECT_EQ(4, tables3->getRouteTable(rid)->getRibV6()->size());
}
