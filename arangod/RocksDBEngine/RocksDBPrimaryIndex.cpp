////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2017 ArangoDB GmbH, Cologne, Germany
/// Copyright 2004-2014 triAGENS GmbH, Cologne, Germany
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is ArangoDB GmbH, Cologne, Germany
///
/// @author Jan Steemann
////////////////////////////////////////////////////////////////////////////////

#include "RocksDBPrimaryIndex.h"
#include "Aql/AstNode.h"
#include "Basics/Exceptions.h"
#include "Basics/StaticStrings.h"
#include "Basics/VelocyPackHelper.h"
#include "Cache/CachedValue.h"
#include "Cache/TransactionalCache.h"
#include "Cluster/ServerState.h"
#include "Indexes/SkiplistIndexAttributeMatcher.h"
#include "Logger/Logger.h"
#include "RocksDBEngine/RocksDBCollection.h"
#include "RocksDBEngine/RocksDBCommon.h"
#include "RocksDBEngine/RocksDBComparator.h"
#include "RocksDBEngine/RocksDBEngine.h"
#include "RocksDBEngine/RocksDBKey.h"
#include "RocksDBEngine/RocksDBKeyBounds.h"
#include "RocksDBEngine/RocksDBMethods.h"
#include "RocksDBEngine/RocksDBTransactionState.h"
#include "RocksDBEngine/RocksDBTypes.h"
#include "RocksDBEngine/RocksDBValue.h"
#include "StorageEngine/EngineSelectorFeature.h"
#include "Transaction/Context.h"
#include "Transaction/Helpers.h"
#include "Transaction/Methods.h"
#include "VocBase/LogicalCollection.h"

#include "RocksDBEngine/RocksDBPrefixExtractor.h"

#ifdef USE_ENTERPRISE
#include "Enterprise/VocBase/VirtualCollection.h"
#endif

#include <rocksdb/iterator.h>
#include <rocksdb/utilities/transaction.h>

#include <velocypack/Builder.h>
#include <velocypack/Collection.h>
#include <velocypack/Slice.h>
#include <velocypack/velocypack-aliases.h>

using namespace arangodb;

namespace {
  std::string const lowest;               // smallest possible key
  std::string const highest = "\xFF";    // greatest possible key
}

RocksDBPrimaryIndexRangeIterator::RocksDBPrimaryIndexRangeIterator(
    LogicalCollection* collection, transaction::Methods* trx,
    arangodb::RocksDBPrimaryIndex const* index, bool reverse, RocksDBKeyBounds&& bounds)
    : IndexIterator(collection, trx),
      _index(index),
      _cmp(index->comparator()),
      _reverse(reverse),
      _bounds(std::move(bounds)) {
  TRI_ASSERT(index->columnFamily() == RocksDBColumnFamily::primary());

  RocksDBMethods* mthds = RocksDBTransactionState::toMethods(trx);
  rocksdb::ReadOptions options = mthds->iteratorReadOptions();
  // we need to have a pointer to a slice for the upper bound
  // so we need to assign the slice to an instance variable here
  if (reverse) {
    _rangeBound = _bounds.start();
    options.iterate_lower_bound = &_rangeBound;
  } else {
    _rangeBound = _bounds.end();
    options.iterate_upper_bound = &_rangeBound;
  }

  TRI_ASSERT(options.prefix_same_as_start);
  _iterator = mthds->NewIterator(options, index->columnFamily());
  if (reverse) {
    _iterator->SeekForPrev(_bounds.end());
  } else {
    _iterator->Seek(_bounds.start());
  }
}

/// @brief Reset the cursor
void RocksDBPrimaryIndexRangeIterator::reset() {
  TRI_ASSERT(_trx->state()->isRunning());

  if (_reverse) {
    _iterator->SeekForPrev(_bounds.end());
  } else {
    _iterator->Seek(_bounds.start());
  }
}

bool RocksDBPrimaryIndexRangeIterator::outOfRange() const {
  TRI_ASSERT(_trx->state()->isRunning());
  if (_reverse) {
    return (_cmp->Compare(_iterator->key(), _bounds.start()) < 0);
  } else {
    return (_cmp->Compare(_iterator->key(), _bounds.end()) > 0);
  }
}

bool RocksDBPrimaryIndexRangeIterator::next(LocalDocumentIdCallback const& cb, size_t limit) {
  TRI_ASSERT(_trx->state()->isRunning());

  if (limit == 0 || !_iterator->Valid() || outOfRange()) {
    // No limit no data, or we are actually done. The last call should have
    // returned false
    TRI_ASSERT(limit > 0);  // Someone called with limit == 0. Api broken
    return false;
  }

  while (limit > 0) {
    TRI_ASSERT(_index->objectId() == RocksDBKey::objectId(_iterator->key()));

    cb(RocksDBValue::documentId(_iterator->value()));

    --limit;
    if (_reverse) {
      _iterator->Prev();
    } else {
      _iterator->Next();
    }

    if (!_iterator->Valid() || outOfRange()) {
      return false;
    }
  }

  return true;
}

void RocksDBPrimaryIndexRangeIterator::skip(uint64_t count, uint64_t& skipped) {
  TRI_ASSERT(_trx->state()->isRunning());

  if (!_iterator->Valid() || outOfRange()) {
    return;
  }

  while (count > 0) {
    TRI_ASSERT(_index->objectId() == RocksDBKey::objectId(_iterator->key()));

    --count;
    ++skipped;
    if (_reverse) {
      _iterator->Prev();
    } else {
      _iterator->Next();
    }

    if (!_iterator->Valid() || outOfRange()) {
      return;
    }
  }
}

// ================ Primary Index Iterator ================

/// @brief hard-coded vector of the index attributes
/// note that the attribute names must be hard-coded here to avoid an init-order
/// fiasco with StaticStrings::FromString etc.
static std::vector<std::vector<arangodb::basics::AttributeName>> const IndexAttributes{
    {arangodb::basics::AttributeName("_id", false)},
    {arangodb::basics::AttributeName("_key", false)}};

RocksDBPrimaryIndexEqIterator::RocksDBPrimaryIndexEqIterator(
    LogicalCollection* collection, transaction::Methods* trx, RocksDBPrimaryIndex* index,
    std::unique_ptr<VPackBuilder> key, bool allowCoveringIndexOptimization)
    : IndexIterator(collection, trx),
      _index(index),
      _key(std::move(key)),
      _done(false),
      _allowCoveringIndexOptimization(allowCoveringIndexOptimization) {
  TRI_ASSERT(_key->slice().isString());
}

RocksDBPrimaryIndexEqIterator::~RocksDBPrimaryIndexEqIterator() {
  if (_key != nullptr) {
    // return the VPackBuilder to the transaction context
    _trx->transactionContextPtr()->returnBuilder(_key.release());
  }
}

bool RocksDBPrimaryIndexEqIterator::next(LocalDocumentIdCallback const& cb, size_t limit) {
  if (limit == 0 || _done) {
    // No limit no data, or we are actually done. The last call should have
    // returned false
    TRI_ASSERT(limit > 0);  // Someone called with limit == 0. Api broken
    return false;
  }

  _done = true;
  LocalDocumentId documentId = _index->lookupKey(_trx, StringRef(_key->slice()));
  if (documentId.isSet()) {
    cb(documentId);
  }
  return false;
}

bool RocksDBPrimaryIndexEqIterator::nextCovering(DocumentCallback const& cb, size_t limit) {
  TRI_ASSERT(_allowCoveringIndexOptimization);
  if (limit == 0 || _done) {
    // No limit no data, or we are actually done. The last call should have
    // returned false
    TRI_ASSERT(limit > 0);  // Someone called with limit == 0. Api broken
    return false;
  }

  _done = true;
  LocalDocumentId documentId = _index->lookupKey(_trx, StringRef(_key->slice()));
  if (documentId.isSet()) {
    cb(documentId, _key->slice());
  }
  return false;
}

void RocksDBPrimaryIndexEqIterator::reset() { _done = false; }

RocksDBPrimaryIndexInIterator::RocksDBPrimaryIndexInIterator(
    LogicalCollection* collection, transaction::Methods* trx, RocksDBPrimaryIndex* index,
    std::unique_ptr<VPackBuilder> keys, bool allowCoveringIndexOptimization)
    : IndexIterator(collection, trx),
      _index(index),
      _keys(std::move(keys)),
      _iterator(_keys->slice()),
      _allowCoveringIndexOptimization(allowCoveringIndexOptimization) {
  TRI_ASSERT(_keys->slice().isArray());
}

RocksDBPrimaryIndexInIterator::~RocksDBPrimaryIndexInIterator() {
  if (_keys != nullptr) {
    // return the VPackBuilder to the transaction context
    _trx->transactionContextPtr()->returnBuilder(_keys.release());
  }
}

bool RocksDBPrimaryIndexInIterator::next(LocalDocumentIdCallback const& cb, size_t limit) {
  if (limit == 0 || !_iterator.valid()) {
    // No limit no data, or we are actually done. The last call should have
    // returned false
    TRI_ASSERT(limit > 0);  // Someone called with limit == 0. Api broken
    return false;
  }

  while (limit > 0) {
    LocalDocumentId documentId = _index->lookupKey(_trx, StringRef(*_iterator));
    if (documentId.isSet()) {
      cb(documentId);
      --limit;
    }

    _iterator.next();
    if (!_iterator.valid()) {
      return false;
    }
  }
  return true;
}

bool RocksDBPrimaryIndexInIterator::nextCovering(DocumentCallback const& cb, size_t limit) {
  TRI_ASSERT(_allowCoveringIndexOptimization);
  if (limit == 0 || !_iterator.valid()) {
    // No limit no data, or we are actually done. The last call should have
    // returned false
    TRI_ASSERT(limit > 0);  // Someone called with limit == 0. Api broken
    return false;
  }

  while (limit > 0) {
    // TODO: prevent copying of the value into result, as we don't need it here!
    LocalDocumentId documentId = _index->lookupKey(_trx, StringRef(*_iterator));
    if (documentId.isSet()) {
      cb(documentId, *_iterator);
      --limit;
    }

    _iterator.next();
    if (!_iterator.valid()) {
      return false;
    }
  }
  return true;
}

void RocksDBPrimaryIndexInIterator::reset() { _iterator.reset(); }

// ================ PrimaryIndex ================

RocksDBPrimaryIndex::RocksDBPrimaryIndex(arangodb::LogicalCollection& collection,
                                         arangodb::velocypack::Slice const& info)
    : RocksDBIndex(0, collection,
                   std::vector<std::vector<arangodb::basics::AttributeName>>(
                       {{arangodb::basics::AttributeName(StaticStrings::KeyString, false)}}),
                   true, false, RocksDBColumnFamily::primary(),
                   basics::VelocyPackHelper::stringUInt64(info, "objectId"),
                   static_cast<RocksDBCollection*>(collection.getPhysical())->cacheEnabled()),
      _isRunningInCluster(ServerState::instance()->isRunningInCluster()) {
  TRI_ASSERT(_cf == RocksDBColumnFamily::primary());
  TRI_ASSERT(_objectId != 0);
}

RocksDBPrimaryIndex::~RocksDBPrimaryIndex() {}

void RocksDBPrimaryIndex::load() {
  RocksDBIndex::load();
  if (useCache()) {
    // FIXME: make the factor configurable
    RocksDBCollection* rdb = static_cast<RocksDBCollection*>(_collection.getPhysical());
    uint64_t numDocs = rdb->numberDocuments();

    if (numDocs > 0) {
      _cache->sizeHint(static_cast<uint64_t>(0.3 * numDocs));
    }
  }
}

/// @brief return a VelocyPack representation of the index
void RocksDBPrimaryIndex::toVelocyPack(VPackBuilder& builder,
                                       std::underlying_type<Serialize>::type flags) const {
  builder.openObject();
  RocksDBIndex::toVelocyPack(builder, flags);
  // hard-coded
  builder.add(arangodb::StaticStrings::IndexUnique, arangodb::velocypack::Value(true));
  builder.add(arangodb::StaticStrings::IndexSparse, arangodb::velocypack::Value(false));
  builder.close();
}

LocalDocumentId RocksDBPrimaryIndex::lookupKey(transaction::Methods* trx,
                                               arangodb::StringRef keyRef) const {
  RocksDBKeyLeaser key(trx);
  key->constructPrimaryIndexValue(_objectId, keyRef);

  bool lockTimeout = false;
  if (useCache()) {
    TRI_ASSERT(_cache != nullptr);
    // check cache first for fast path
    auto f = _cache->find(key->string().data(),
                          static_cast<uint32_t>(key->string().size()));
    if (f.found()) {
      rocksdb::Slice s(reinterpret_cast<char const*>(f.value()->value()),
                       f.value()->valueSize());
      return RocksDBValue::documentId(s);
    } else if (f.result().errorNumber() == TRI_ERROR_LOCK_TIMEOUT) {
      // assuming someone is currently holding a write lock, which
      // is why we cannot access the TransactionalBucket.
      lockTimeout = true;  // we skip the insert in this case
    }
  }

  RocksDBMethods* mthds = RocksDBTransactionState::toMethods(trx);
  rocksdb::PinnableSlice val;
  rocksdb::Status s = mthds->Get(_cf, key->string(), &val);
  if (!s.ok()) {
    return LocalDocumentId();
  }

  if (useCache() && !lockTimeout) {
    TRI_ASSERT(_cache != nullptr);

    // write entry back to cache
    auto entry =
        cache::CachedValue::construct(key->string().data(),
                                      static_cast<uint32_t>(key->string().size()),
                                      val.data(), static_cast<uint64_t>(val.size()));
    if (entry) {
      Result status = _cache->insert(entry);
      if (status.errorNumber() == TRI_ERROR_LOCK_TIMEOUT) {
        // the writeLock uses cpu_relax internally, so we can try yield
        std::this_thread::yield();
        status = _cache->insert(entry);
      }
      if (status.fail()) {
        delete entry;
      }
    }
  }

  return RocksDBValue::documentId(val);
}

/// @brief reads a revision id from the primary index
/// if the document does not exist, this function will return false
/// if the document exists, the function will return true
/// the revision id will only be non-zero if the primary index
/// value contains the document's revision id. note that this is not
/// the case for older collections
/// in this case the caller must fetch the revision id from the actual
/// document
bool RocksDBPrimaryIndex::lookupRevision(transaction::Methods* trx, arangodb::StringRef keyRef,
                                         LocalDocumentId& documentId,
                                         TRI_voc_rid_t& revisionId) const {
  documentId.clear();
  revisionId = 0;

  RocksDBKeyLeaser key(trx);
  key->constructPrimaryIndexValue(_objectId, keyRef);

  // acquire rocksdb transaction
  RocksDBMethods* mthds = RocksDBTransactionState::toMethods(trx);
  rocksdb::PinnableSlice val;
  rocksdb::Status s = mthds->Get(_cf, key->string(), &val);
  if (!s.ok()) {
    return false;
  }

  documentId = RocksDBValue::documentId(val);

  // this call will populate revisionId if the revision id value is
  // stored in the primary index
  revisionId = RocksDBValue::revisionId(val);
  return true;
}

Result RocksDBPrimaryIndex::insertInternal(transaction::Methods& trx, RocksDBMethods* mthd,
                                           LocalDocumentId const& documentId,
                                           velocypack::Slice const& slice,
                                           Index::OperationMode mode) {
  Result res;
  VPackSlice keySlice = transaction::helpers::extractKeyFromDocument(slice);
  TRI_ASSERT(keySlice.isString());
  RocksDBKeyLeaser key(&trx);

  key->constructPrimaryIndexValue(_objectId, StringRef(keySlice));

  rocksdb::PinnableSlice val;
  rocksdb::Status s = mthd->Get(_cf, key->string(), &val);

  if (s.ok()) {  // detected conflicting primary key
    std::string existingId = keySlice.copyString();

    if (mode == OperationMode::internal) {
      return res.reset(TRI_ERROR_ARANGO_UNIQUE_CONSTRAINT_VIOLATED, std::move(existingId));
    }

    res.reset(TRI_ERROR_ARANGO_UNIQUE_CONSTRAINT_VIOLATED);

    return addErrorMsg(res, existingId);
  }
  val.Reset();  // clear used memory

  blackListKey(key->string().data(), static_cast<uint32_t>(key->string().size()));

  TRI_voc_rid_t revision = transaction::helpers::extractRevFromDocument(slice);
  auto value = RocksDBValue::PrimaryIndexValue(documentId, revision);

  s = mthd->Put(_cf, key.ref(), value.string());
  if (!s.ok()) {
    res.reset(rocksutils::convertStatus(s, rocksutils::index));
    addErrorMsg(res);
  }
  return res;
}

Result RocksDBPrimaryIndex::updateInternal(transaction::Methods& trx, RocksDBMethods* mthd,
                                           LocalDocumentId const& oldDocumentId,
                                           velocypack::Slice const& oldDoc,
                                           LocalDocumentId const& newDocumentId,
                                           velocypack::Slice const& newDoc,
                                           Index::OperationMode mode) {
  Result res;
  VPackSlice keySlice = transaction::helpers::extractKeyFromDocument(oldDoc);
  TRI_ASSERT(keySlice == oldDoc.get(StaticStrings::KeyString));
  RocksDBKeyLeaser key(&trx);

  key->constructPrimaryIndexValue(_objectId, StringRef(keySlice));

  TRI_voc_rid_t revision = transaction::helpers::extractRevFromDocument(newDoc);
  auto value = RocksDBValue::PrimaryIndexValue(newDocumentId, revision);

  blackListKey(key->string().data(), static_cast<uint32_t>(key->string().size()));

  rocksdb::Status s = mthd->Put(_cf, key.ref(), value.string());
  if (!s.ok()) {
    res.reset(rocksutils::convertStatus(s, rocksutils::index));
    addErrorMsg(res);
  }
  return res;
}

Result RocksDBPrimaryIndex::removeInternal(transaction::Methods& trx, RocksDBMethods* mthd,
                                           LocalDocumentId const& documentId,
                                           velocypack::Slice const& slice,
                                           Index::OperationMode mode) {
  Result res;

  // TODO: deal with matching revisions?
  VPackSlice keySlice = transaction::helpers::extractKeyFromDocument(slice);
  TRI_ASSERT(keySlice.isString());
  RocksDBKeyLeaser key(&trx);
  key->constructPrimaryIndexValue(_objectId, StringRef(keySlice));

  blackListKey(key->string().data(), static_cast<uint32_t>(key->string().size()));

  // acquire rocksdb transaction
  auto* mthds = RocksDBTransactionState::toMethods(&trx);
  rocksdb::Status s = mthds->Delete(_cf, key.ref());
  if (!s.ok()) {
    res.reset(rocksutils::convertStatus(s, rocksutils::index));
    addErrorMsg(res);
  }
  return res;
}

/// @brief checks whether the index supports the condition
bool RocksDBPrimaryIndex::supportsFilterCondition(
    std::vector<std::shared_ptr<arangodb::Index>> const& allIndexes,
    arangodb::aql::AstNode const* node, arangodb::aql::Variable const* reference,
    size_t itemsInIndex, size_t& estimatedItems, double& estimatedCost) const {
  std::unordered_map<size_t, std::vector<arangodb::aql::AstNode const*>> found;
  std::unordered_set<std::string> nonNullAttributes;

  std::size_t values = 0;
  SkiplistIndexAttributeMatcher::matchAttributes(this, node, reference, found,
                                                 values, nonNullAttributes,
                                                 /*skip evaluation (during execution)*/ false);
  estimatedItems = values;
  return !found.empty();
}

/// @brief creates an IndexIterator for the given Condition
IndexIterator* RocksDBPrimaryIndex::iteratorForCondition(
    transaction::Methods* trx, ManagedDocumentResult*, arangodb::aql::AstNode const* node,
    arangodb::aql::Variable const* reference, IndexIteratorOptions const& opts) {
  TRI_ASSERT(!isSorted() || opts.sorted);
  TRI_ASSERT(node->type == aql::NODE_TYPE_OPERATOR_NARY_AND);
  TRI_ASSERT(node->numMembers() >= 1);

  static const auto removeCollectionFromString =
      [this, &trx](bool isId, std::string& value) -> bool {
    if (isId) {
      char const* key = nullptr;
      size_t outLength = 0;
      std::shared_ptr<LogicalCollection> collection;
      Result res = trx->resolveId(value.data(), value.length(), collection, key, outLength);

      if (!res.ok()) {
        return false;
      }

      TRI_ASSERT(key);
      TRI_ASSERT(collection);

      if (!_isRunningInCluster && collection->id() != _collection.id()) {
        // only continue lookup if the id value is syntactically correct and
        // refers to "our" collection, using local collection id
        return false;
      }

      if (_isRunningInCluster) {
        if (collection->planId() != _collection.planId()) {
          // only continue lookup if the id value is syntactically correct and
          // refers to "our" collection, using cluster collection id
          return false;
        }
      }

      value = std::string(key, outLength);
    }
    return true;
  };

  if (node->numMembers() == 1) {
    auto comp = node->getMember(0);
    // assume a.b == value
    auto attrNode = comp->getMember(0);
    auto valNode = comp->getMember(1);

    if (attrNode->type != aql::NODE_TYPE_ATTRIBUTE_ACCESS) {
      // value == a.b  ->  flip the two sides
      attrNode = comp->getMember(1);
      valNode = comp->getMember(0);
    }

    TRI_ASSERT(attrNode->type == aql::NODE_TYPE_ATTRIBUTE_ACCESS);

    if (comp->type == aql::NODE_TYPE_OPERATOR_BINARY_EQ) {
      // a.b == value
      return createEqIterator(trx, attrNode, valNode);
    }

    if (comp->type == aql::NODE_TYPE_OPERATOR_BINARY_IN) {
      // a.b IN values
      if (valNode->isArray()) {
        // a.b IN array
        return createInIterator(trx, attrNode, valNode);
      }
    }
  }

  std::string lower;
  std::string upper;
  bool lower_found = false;
  bool upper_found = false;

  // add nodes that may create a range
  std::vector<aql::AstNode const*> nodes;
  for (size_t i = 0; i < node->numMembers(); i++) {
    nodes.push_back(node->getMember(i));
  }

  for (auto comp : nodes) {
    if (comp == nullptr) {
      continue;
    }

    auto type = comp->type;

    if (!(type == aql::NODE_TYPE_OPERATOR_BINARY_LE ||
          type == aql::NODE_TYPE_OPERATOR_BINARY_LT || type == aql::NODE_TYPE_OPERATOR_BINARY_GE ||
          type == aql::NODE_TYPE_OPERATOR_BINARY_GT ||
          type == aql::NODE_TYPE_OPERATOR_BINARY_EQ
        )) {
      return new EmptyIndexIterator(&_collection, trx);
    }

    auto attrNode = comp->getMember(0);
    auto valNode = comp->getMember(1);
    bool flip = false;

    if (attrNode->type != aql::NODE_TYPE_ATTRIBUTE_ACCESS) {
      // value == a.b  ->  flip the two sides
      attrNode = comp->getMember(1);
      valNode = comp->getMember(0);
      flip = true;
    }

    TRI_ASSERT(attrNode->type == aql::NODE_TYPE_ATTRIBUTE_ACCESS);
    bool const isId = (attrNode->stringEquals(StaticStrings::IdString));

    std::string value = lowest;
    if (valNode->isStringValue()) {
      value = valNode->getString();
    } else if (valNode->isObject() || valNode->isArray()) {
      value = highest;
    } else if (valNode->isNullValue() || valNode->isBoolValue()) {
      // keep lowest
    } else {
      TRI_ASSERT(false);
    }

    if (flip) {
      switch (type) {
        case aql::NODE_TYPE_OPERATOR_BINARY_LE: {
          type = aql::NODE_TYPE_OPERATOR_BINARY_GE;
          break;
        }
        case aql::NODE_TYPE_OPERATOR_BINARY_LT: {
          type = aql::NODE_TYPE_OPERATOR_BINARY_GT;
          break;
        }
        case aql::NODE_TYPE_OPERATOR_BINARY_GE: {
          type = aql::NODE_TYPE_OPERATOR_BINARY_LE;
          break;
        }
        case aql::NODE_TYPE_OPERATOR_BINARY_GT: {
          type = aql::NODE_TYPE_OPERATOR_BINARY_LT;
          break;
        }
        case aql::NODE_TYPE_OPERATOR_BINARY_EQ: {
          break;
        }
        default: {
          continue;
        }
      }
    }

    if (!removeCollectionFromString(isId, value)) {
      continue;
    }

    if (type == aql::NODE_TYPE_OPERATOR_BINARY_EQ) {
      if (!upper_found || value < upper) {
        upper = value;
        upper_found = true;
      }
      if (!lower_found || value < lower) {
        lower = std::move(value);
        lower_found = true;
      }
    }

    if (type == aql::NODE_TYPE_OPERATOR_BINARY_LE || type == aql::NODE_TYPE_OPERATOR_BINARY_LT) {
      // a.b < value
      if (type == aql::NODE_TYPE_OPERATOR_BINARY_LT && value != lowest) {
        value.back() -= 0x01U;  // modify upper bound so that it is not included
      }
      if (!upper_found || value < upper) {
        upper = std::move(value);
        upper_found = true;
      }
    }

    if (type == aql::NODE_TYPE_OPERATOR_BINARY_GE || type == aql::NODE_TYPE_OPERATOR_BINARY_GT) {
      if (type == aql::NODE_TYPE_OPERATOR_BINARY_GE && value != lowest) {
        value.back() -= 0x01U;  // modify lower bound so it is included
      }
      lower = std::move(value);
      lower_found = true;
    }

  }  // for nodes

  // if only one bound is given select the other (lowest or highest) accordingly
  if (upper_found && !lower_found) {
    lower = lowest;
    lower_found = true;
  } else if (lower_found && !upper_found) {
    upper = highest;
    upper_found = true;
  }

  if (lower_found && upper_found) {
    return new RocksDBPrimaryIndexRangeIterator(
        &_collection /*logical collection*/, trx, this, opts.ascending /*reverse*/,
        RocksDBKeyBounds::PrimaryIndex(_objectId, lower, upper));
  }

  // operator type unsupported or IN used on non-array
  return new EmptyIndexIterator(&_collection, trx);
};

/// @brief specializes the condition for use with the index
arangodb::aql::AstNode* RocksDBPrimaryIndex::specializeCondition(
    arangodb::aql::AstNode* node, arangodb::aql::Variable const* reference) const {
  return SkiplistIndexAttributeMatcher::specializeCondition(this, node, reference);
}

/// @brief create the iterator, for a single attribute, IN operator
IndexIterator* RocksDBPrimaryIndex::createInIterator(transaction::Methods* trx,
                                                     arangodb::aql::AstNode const* attrNode,
                                                     arangodb::aql::AstNode const* valNode) {
  // _key or _id?
  bool const isId = (attrNode->stringEquals(StaticStrings::IdString));

  TRI_ASSERT(valNode->isArray());

  // lease builder, but immediately pass it to the unique_ptr so we don't leak
  transaction::BuilderLeaser builder(trx);
  std::unique_ptr<VPackBuilder> keys(builder.steal());
  keys->openArray();

  size_t const n = valNode->numMembers();

  // only leave the valid elements
  for (size_t i = 0; i < n; ++i) {
    handleValNode(trx, keys.get(), valNode->getMemberUnchecked(i), isId);
    TRI_IF_FAILURE("PrimaryIndex::iteratorValNodes") {
      THROW_ARANGO_EXCEPTION(TRI_ERROR_DEBUG);
    }
  }

  TRI_IF_FAILURE("PrimaryIndex::noIterator") {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_DEBUG);
  }

  keys->close();

  return new RocksDBPrimaryIndexInIterator(&_collection, trx, this, std::move(keys), !isId);
}

/// @brief create the iterator, for a single attribute, EQ operator
IndexIterator* RocksDBPrimaryIndex::createEqIterator(transaction::Methods* trx,
                                                     arangodb::aql::AstNode const* attrNode,
                                                     arangodb::aql::AstNode const* valNode) {
  // _key or _id?
  bool const isId = (attrNode->stringEquals(StaticStrings::IdString));

  // lease builder, but immediately pass it to the unique_ptr so we don't leak
  transaction::BuilderLeaser builder(trx);
  std::unique_ptr<VPackBuilder> key(builder.steal());

  // handle the sole element
  handleValNode(trx, key.get(), valNode, isId);

  TRI_IF_FAILURE("PrimaryIndex::noIterator") {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_DEBUG);
  }

  if (!key->isEmpty()) {
    return new RocksDBPrimaryIndexEqIterator(&_collection, trx, this, std::move(key), !isId);
  }

  return new EmptyIndexIterator(&_collection, trx);
}

/// @brief add a single value node to the iterator's keys
void RocksDBPrimaryIndex::handleValNode(transaction::Methods* trx, VPackBuilder* keys,
                                        arangodb::aql::AstNode const* valNode,
                                        bool isId) const {
  if (!valNode->isStringValue() || valNode->getStringLength() == 0) {
    return;
  }

  if (isId) {
    // lookup by _id. now validate if the lookup is performed for the
    // correct collection (i.e. _collection)
    char const* key = nullptr;
    size_t outLength = 0;
    std::shared_ptr<LogicalCollection> collection;
    Result res = trx->resolveId(valNode->getStringValue(), valNode->getStringLength(),
                                collection, key, outLength);

    if (!res.ok()) {
      return;
    }

    TRI_ASSERT(collection != nullptr);
    TRI_ASSERT(key != nullptr);

    if (!_isRunningInCluster && collection->id() != _collection.id()) {
      // only continue lookup if the id value is syntactically correct and
      // refers to "our" collection, using local collection id
      return;
    }

    if (_isRunningInCluster) {
#ifdef USE_ENTERPRISE
      if (collection->isSmart() && collection->type() == TRI_COL_TYPE_EDGE) {
        auto c = dynamic_cast<VirtualSmartEdgeCollection const*>(collection.get());
        if (c == nullptr) {
          THROW_ARANGO_EXCEPTION_MESSAGE(
              TRI_ERROR_INTERNAL, "unable to cast smart edge collection");
        }

        if (_collection.planId() != c->getLocalCid() &&
            _collection.planId() != c->getFromCid() &&
            _collection.planId() != c->getToCid()) {
          // invalid planId
          return;
        }
      } else
#endif
          if (collection->planId() != _collection.planId()) {
        // only continue lookup if the id value is syntactically correct and
        // refers to "our" collection, using cluster collection id
        return;
      }
    }

    // use _key value from _id
    keys->add(VPackValuePair(key, outLength, VPackValueType::String));
  } else {
    keys->add(VPackValuePair(valNode->getStringValue(),
                             valNode->getStringLength(), VPackValueType::String));
  }
}
