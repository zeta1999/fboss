/*
 *  Copyright (c) 2004-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#pragma once

#include "fboss/agent/hw/sai/api/LoggingUtil.h"
#include "fboss/agent/hw/sai/api/SaiApiError.h"
#include "fboss/agent/hw/sai/api/SaiApiLock.h"
#include "fboss/agent/hw/sai/api/SaiAttribute.h"
#include "fboss/agent/hw/sai/api/SaiAttributeDataTypes.h"
#include "fboss/agent/hw/sai/api/Traits.h"
#include "fboss/lib/TupleUtils.h"

#include <folly/Format.h>
#include <folly/logging/xlog.h>

#include <boost/variant.hpp>

#include <exception>
#include <mutex>
#include <stdexcept>
#include <type_traits>
#include <vector>

extern "C" {
#include <sai.h>
}

namespace facebook::fboss {

template <typename ApiT>
class SaiApi {
 public:
  virtual ~SaiApi() = default;
  SaiApi() = default;
  SaiApi(const SaiApi& other) = delete;
  SaiApi& operator=(const SaiApi& other) = delete;

  // Currently, create is not clever enough to have totally deducible
  // template parameters. It can be done, but I think it would reduce
  // the value of the CreateAttributes pattern. That is something that
  // may change in the future.
  //
  // It is also split up into two implementations:
  // one for objects whose AdapterKey is a SAI object id, which needs to
  // return the adapter key, and another for those objects whose AdapterKey
  // is an entry struct, which must take an AdapterKey but don't return one.
  // The distinction is drawn with traits from Traits.h and SFINAE

  // sai_object_id_t case
  template <typename SaiObjectTraits>
  std::enable_if_t<
      AdapterKeyIsObjectId<SaiObjectTraits>::value,
      typename SaiObjectTraits::AdapterKey>
  create(
      const typename SaiObjectTraits::CreateAttributes& createAttributes,
      sai_object_id_t switch_id) {
    static_assert(
        std::is_same_v<typename SaiObjectTraits::SaiApiT, ApiT>,
        "invalid traits for the api");
    typename SaiObjectTraits::AdapterKey key;
    std::vector<sai_attribute_t> saiAttributeTs = saiAttrs(createAttributes);
    std::lock_guard<std::mutex> g{SaiApiLock::getInstance()->lock};
    sai_status_t status = impl()._create(
        &key, switch_id, saiAttributeTs.size(), saiAttributeTs.data());
    saiApiCheckError(status, ApiT::ApiType, "Failed to create sai entity");
    return key;
  }

  // entry struct case
  template <typename SaiObjectTraits>
  std::enable_if_t<AdapterKeyIsEntryStruct<SaiObjectTraits>::value, void>
  create(
      const typename SaiObjectTraits::AdapterKey& entry,
      const typename SaiObjectTraits::CreateAttributes& createAttributes) {
    static_assert(
        std::is_same_v<typename SaiObjectTraits::SaiApiT, ApiT>,
        "invalid traits for the api");
    std::vector<sai_attribute_t> saiAttributeTs = saiAttrs(createAttributes);
    std::lock_guard<std::mutex> g{SaiApiLock::getInstance()->lock};
    sai_status_t status =
        impl()._create(entry, saiAttributeTs.size(), saiAttributeTs.data());
    saiApiCheckError(status, ApiT::ApiType, "Failed to create sai entity");
    XLOG(DBG5) << "created sai object [" << saiApiTypeToString(ApiT::ApiType)
               << "]:" << folly::logging::objectToString(entry);
  }

  template <typename AdapterKeyT>
  void remove(const AdapterKeyT& key) {
    std::lock_guard<std::mutex> g{SaiApiLock::getInstance()->lock};
    sai_status_t status = impl()._remove(key);
    saiApiCheckError(status, ApiT::ApiType, "Failed to remove sai object");
    XLOG(DBG5) << "removed sai object [" << saiApiTypeToString(ApiT::ApiType)
               << "]:" << folly::logging::objectToString(key);
  }

  /*
   * We can do getAttribute on top of more complicated types than just
   * attributes. For example, if we overload on tuples and optionals, we
   * can recursively call getAttribute on their internals to do more
   * interesting gets. This is particularly useful when we want to load
   * complicated aggregations of SaiAttributes for something like warm boot.
   */

  // Default "real attr". This is the base case of the recursion
  template <
      typename AdapterKeyT,
      typename AttrT,
      typename = std::enable_if_t<
          IsSaiAttribute<std::remove_reference_t<AttrT>>::value>>
  typename std::remove_reference_t<AttrT>::ValueType getAttribute(
      const AdapterKeyT& key,
      AttrT&& attr) {
    static_assert(
        IsSaiAttribute<typename std::remove_reference<AttrT>::type>::value,
        "getAttribute must be called on a SaiAttribute or supported "
        "collection of SaiAttributes");

    std::lock_guard<std::mutex> g{SaiApiLock::getInstance()->lock};
    sai_status_t status;
    status = impl()._getAttribute(key, attr.saiAttr());
    /*
     * If this is a list attribute and we have not allocated enough
     * memory for the data coming from SAI, the Adapter will return
     * SAI_STATUS_BUFFER_OVERFLOW and fill in `count` in the list object.
     * We can take advantage of that to allocate a proper buffer and
     * try the get again.
     */
    if (status == SAI_STATUS_BUFFER_OVERFLOW) {
      attr.realloc();
      status = impl()._getAttribute(key, attr.saiAttr());
    }
    saiApiCheckError(status, ApiT::ApiType, "Failed to get sai attribute");
    return attr.value();
  }

  // std::tuple of attributes
  template <
      typename AdapterKeyT,
      typename TupleT,
      typename =
          std::enable_if_t<IsTuple<std::remove_reference_t<TupleT>>::value>>
  const std::remove_reference_t<TupleT> getAttribute(
      const AdapterKeyT& key,
      TupleT&& attrTuple) {
    // TODO: assert on All<IsSaiAttribute>
    auto recurse = [&key, this](auto&& attr) {
      return getAttribute(key, std::forward<decltype(attr)>(attr));
    };
    return tupleMap(recurse, std::forward<TupleT>(attrTuple));
  }

  // std::optional of attribute
  template <
      typename AdapterKeyT,
      typename AttrT,
      typename = std::enable_if_t<
          IsSaiAttribute<std::remove_reference_t<AttrT>>::value>>
  auto getAttribute(
      const AdapterKeyT& key,
      std::optional<AttrT>& attrOptional) {
    AttrT attr = attrOptional.value_or(AttrT{});
    auto res =
        std::optional<typename AttrT::ValueType>{getAttribute(key, attr)};
    return res;
  }

  template <typename AdapterKeyT, typename AttrT>
  void setAttribute(const AdapterKeyT& key, const AttrT& attr) {
    std::lock_guard<std::mutex> g{SaiApiLock::getInstance()->lock};
    auto status = impl()._setAttribute(key, saiAttr(attr));
    saiApiCheckError(status, ApiT::ApiType, "Failed to set attribute");
  }

  template <typename SaiObjectTraits>
  std::vector<uint64_t> getStats(
      const typename SaiObjectTraits::AdapterKey& key,
      const std::vector<sai_stat_id_t>& counterIds) {
    static_assert(
        SaiObjectHasStats<SaiObjectTraits>::value,
        "getStats only supported for Sai objects with stats");
    std::lock_guard<std::mutex> g{SaiApiLock::getInstance()->lock};
    return getStatsImpl<SaiObjectTraits>(
        key, counterIds.data(), counterIds.size());
  }
  template <typename SaiObjectTraits>
  std::vector<uint64_t> getStats(
      const typename SaiObjectTraits::AdapterKey& key) {
    static_assert(
        SaiObjectHasStats<SaiObjectTraits>::value,
        "getStats only supported for Sai objects with stats");
    std::lock_guard<std::mutex> g{SaiApiLock::getInstance()->lock};
    return getStatsImpl<SaiObjectTraits>(
        key,
        SaiObjectTraits::CounterIds.data(),
        SaiObjectTraits::CounterIds.size());
  }

 private:
  template <typename SaiObjectTraits>
  std::vector<uint64_t> getStatsImpl(
      const typename SaiObjectTraits::AdapterKey& key,
      const sai_stat_id_t* counterIds,
      size_t numCounters) {
    std::vector<uint64_t> counters;
    counters.resize(numCounters);
    sai_status_t status = impl()._getStats(
        key,
        counters.size(),
        counterIds,
        SaiObjectTraits::CounterMode,
        counters.data());
    saiApiCheckError(status, ApiT::ApiType, "Failed to get stats");
    return counters;
  }
  ApiT& impl() {
    return static_cast<ApiT&>(*this);
  }
};

} // namespace facebook::fboss
