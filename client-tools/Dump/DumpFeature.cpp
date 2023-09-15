////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2023 ArangoDB GmbH, Cologne, Germany
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
/// @author Dan Larkin-York
////////////////////////////////////////////////////////////////////////////////

#include "DumpFeature.h"

#include "ApplicationFeatures/ApplicationServer.h"
#include "ApplicationFeatures/BumpFileDescriptorsFeature.h"
#include "Basics/BoundedChannel.h"
#include "Basics/EncodingUtils.h"
#include "Basics/Exceptions.h"
#include "Basics/FileUtils.h"
#include "Basics/InputProcessors.h"
#include "Basics/NumberOfCores.h"
#include "Basics/Result.h"
#include "Basics/ScopeGuard.h"
#include "Basics/StaticStrings.h"
#include "Basics/StringUtils.h"
#include "Basics/VelocyPackHelper.h"
#include "Basics/application-exit.h"
#include "Basics/files.h"
#include "Basics/system-functions.h"
#include "FeaturePhases/BasicFeaturePhaseClient.h"
#include "Logger/LogTimeFormat.h"
#include "Maskings/Maskings.h"
#include "ProgramOptions/Parameters.h"
#include "ProgramOptions/ProgramOptions.h"
#include "Random/RandomGenerator.h"
#include "Shell/ClientFeature.h"
#include "SimpleHttpClient/HttpResponseChecker.h"
#include "SimpleHttpClient/SimpleHttpClient.h"
#include "SimpleHttpClient/SimpleHttpResult.h"
#include "Ssl/SslInterface.h"
#include "Utilities/NameValidator.h"
#include "Utils/ManagedDirectory.h"

#include <chrono>
#include <thread>
#include <unordered_map>

#include <absl/strings/str_cat.h>
#include <velocypack/Collection.h>
#include <velocypack/Dumper.h>
#include <velocypack/Iterator.h>
#include <velocypack/Sink.h>

namespace {

/// @brief fake client and syncer ids we will send to the server. the server
/// keeps track of all connected clients
static std::string clientId;
static std::string syncerId;

/// @brief minimum amount of data to fetch from server in a single batch
constexpr uint64_t MinChunkSize = 1024 * 128;

/// @brief maximum amount of data to fetch from server in a single batch
// NB: larger value may cause tcp issues (check exact limits)
constexpr uint64_t MaxChunkSize = 1024 * 1024 * 96;

std::string serverLabel(std::string const& server) {
  if (server.empty()) {
    return " on server";
  }
  return absl::StrCat(" on server '", server, "'");
}

constexpr std::string_view getDatafileSuffix(bool useVPack) noexcept {
  std::string_view suffix("json");
  if (useVPack) {
    suffix = "vpack";
  }
  return suffix;
}

/// @brief generic error for if server returns bad/unexpected json
arangodb::Result const ErrorMalformedJsonResponse = {
    TRI_ERROR_INTERNAL, "got malformed JSON response from server"};

/// @brief checks that a file pointer is valid and file status is ok
bool fileOk(arangodb::ManagedDirectory::File* file) {
  return (file && file->status().ok());
}

/// @brief assuming file pointer is not ok, generate/extract proper error
arangodb::Result fileError(arangodb::ManagedDirectory::File* file,
                           bool isWritable) {
  if (!file) {
    if (isWritable) {
      return {TRI_ERROR_CANNOT_WRITE_FILE};
    } else {
      return {TRI_ERROR_CANNOT_READ_FILE};
    }
  }
  return file->status();
}

std::string escapedCollectionName(std::string const& name,
                                  VPackSlice parameters) {
  std::string escapedName = name;
  if (arangodb::CollectionNameValidator::validateName(/*isSystem*/ true, false,
                                                      name)
          .fail()) {
    // we have a collection name with special characters.
    // we should not try to save the collection under its name in the
    // filesystem. instead, we will use the collection id as part of the
    // filename. try looking up collection id in "cid"
    VPackSlice idSlice = parameters.get(arangodb::StaticStrings::DataSourceCid);
    if (idSlice.isNone() &&
        parameters.hasKey(arangodb::StaticStrings::DataSourceId)) {
      // "cid" not present, try "id" (there seems to be difference between
      // cluster and single server about which attribute is present)
      idSlice = parameters.get(arangodb::StaticStrings::DataSourceId);
    }
    if (idSlice.isString()) {
      escapedName = idSlice.copyString();
    } else if (idSlice.isNumber<uint64_t>()) {
      escapedName = std::to_string(idSlice.getNumber<uint64_t>());
    } else {
      escapedName =
          std::to_string(arangodb::RandomGenerator::interval(UINT64_MAX));
    }
  }
  return escapedName;
}

std::string escapedViewName(std::string const& name, VPackSlice parameters) {
  std::string escapedName = name;
  if (arangodb::ViewNameValidator::validateName(/*isSystem*/ true, false,
                                                escapedName)
          .fail()) {
    // we have a view name with special characters.
    // we should not try to save the view under its name in the filesystem.
    // instead, we will use the view id as part of the filename.
    VPackSlice idSlice = parameters.get(arangodb::StaticStrings::DataSourceId);
    if (idSlice.isString()) {
      escapedName = idSlice.copyString();
    } else if (idSlice.isNumber<uint64_t>()) {
      escapedName = std::to_string(idSlice.getNumber<uint64_t>());
    } else {
      escapedName =
          std::to_string(arangodb::RandomGenerator::interval(UINT64_MAX));
    }
  }
  return escapedName;
}

/// @brief get a list of available databases to dump for the current user
std::pair<arangodb::Result, std::vector<std::string>> getDatabases(
    arangodb::httpclient::SimpleHttpClient& client) {
  std::string const url = "/_api/database/user";

  std::vector<std::string> databases;

  std::unique_ptr<arangodb::httpclient::SimpleHttpResult> response(
      client.request(arangodb::rest::RequestType::GET, url, "", 0));
  auto check = arangodb::HttpResponseChecker::check(client.getErrorMessage(),
                                                    response.get());

  if (check.fail()) {
    LOG_TOPIC("47882", ERR, arangodb::Logger::DUMP)
        << "An error occurred while trying to determine list of databases: "
        << check.errorMessage();
    return {check, databases};
  }

  // extract vpack body from response
  std::shared_ptr<VPackBuilder> parsedBody;
  try {
    parsedBody = response->getBodyVelocyPack();
  } catch (...) {
    return {::ErrorMalformedJsonResponse, databases};
  }
  VPackSlice resBody = parsedBody->slice();

  if (resBody.isObject()) {
    resBody = resBody.get("result");
  }
  if (!resBody.isArray()) {
    return {{TRI_ERROR_FAILED, "expecting list of databases to be an array"},
            databases};
  }

  for (auto const& it : arangodb::velocypack::ArrayIterator(resBody)) {
    if (it.isString()) {
      databases.push_back(it.copyString());
    }
  }

  // sort by name, with _system first
  std::sort(databases.begin(), databases.end(),
            [](std::string const& lhs, std::string const& rhs) {
              if (lhs == arangodb::StaticStrings::SystemDatabase &&
                  rhs != arangodb::StaticStrings::SystemDatabase) {
                return true;
              } else if (rhs == arangodb::StaticStrings::SystemDatabase &&
                         lhs != arangodb::StaticStrings::SystemDatabase) {
                return false;
              }
              return lhs < rhs;
            });

  return {{TRI_ERROR_NO_ERROR}, databases};
}

/// @brief start a batch via the replication API
std::pair<arangodb::Result, uint64_t> startBatch(
    arangodb::httpclient::SimpleHttpClient& client,
    std::string const& DBserver) {
  using arangodb::basics::VelocyPackHelper;
  using arangodb::basics::StringUtils::uint64;
  using arangodb::basics::StringUtils::urlEncode;

  std::string url =
      "/_api/replication/batch?serverId=" + clientId + "&syncerId=" + syncerId;
  std::string const body = "{\"ttl\":600}";
  if (!DBserver.empty()) {
    url += absl::StrCat("&DBserver=", urlEncode(DBserver));
  }

  std::unique_ptr<arangodb::httpclient::SimpleHttpResult> response(
      client.request(arangodb::rest::RequestType::POST, url, body.c_str(),
                     body.size()));
  auto check = ::arangodb::HttpResponseChecker::check(client.getErrorMessage(),
                                                      response.get());
  if (check.fail()) {
    LOG_TOPIC("34dbf", ERR, arangodb::Logger::DUMP)
        << "An error occurred while creating dump context: "
        << check.errorMessage();
    return {check, 0};
  }

  // extract vpack body from response
  std::shared_ptr<VPackBuilder> parsedBody;
  try {
    parsedBody = response->getBodyVelocyPack();
  } catch (...) {
    return {::ErrorMalformedJsonResponse, 0};
  }
  VPackSlice const resBody = parsedBody->slice();

  // look up "id" value
  std::string const id = VelocyPackHelper::getStringValue(resBody, "id", "");

  return {{TRI_ERROR_NO_ERROR}, uint64(id)};
}

/// @brief prolongs a batch to ensure we can complete our dump
void extendBatch(arangodb::httpclient::SimpleHttpClient& client,
                 std::string const& DBserver, uint64_t batchId) {
  TRI_ASSERT(batchId > 0);

  std::string url =
      absl::StrCat("/_api/replication/batch/", batchId, "?serverId=", clientId,
                   "&syncerId=", syncerId);
  std::string const body = "{\"ttl\":600}";
  if (!DBserver.empty()) {
    using arangodb::basics::StringUtils::urlEncode;
    url += absl::StrCat("&DBserver=", urlEncode(DBserver));
  }

  std::unique_ptr<arangodb::httpclient::SimpleHttpResult> response(
      client.request(arangodb::rest::RequestType::PUT, url, body.c_str(),
                     body.size()));
  // ignore any return value
}

/// @brief mark our batch finished so resources can be freed on server
void endBatch(arangodb::httpclient::SimpleHttpClient& client,
              std::string DBserver, uint64_t& batchId) {
  TRI_ASSERT(batchId > 0);

  std::string url =
      absl::StrCat("/_api/replication/batch/", batchId, "?serverId=", clientId);
  if (!DBserver.empty()) {
    using arangodb::basics::StringUtils::urlEncode;
    url += absl::StrCat("&DBserver=", urlEncode(DBserver));
  }

  std::unique_ptr<arangodb::httpclient::SimpleHttpResult> response(
      client.request(arangodb::rest::RequestType::DELETE_REQ, url, nullptr, 0));
  // ignore any return value

  // overwrite the input id
  batchId = 0;
}

bool isIgnoredHiddenEnterpriseCollection(
    arangodb::DumpFeature::Options const& options, std::string const& name) {
#ifdef USE_ENTERPRISE
  if (!options.force &&
      (name.starts_with(arangodb::StaticStrings::FullLocalPrefix) ||
       name.starts_with(arangodb::StaticStrings::FullFromPrefix) ||
       name.starts_with(arangodb::StaticStrings::FullToPrefix))) {
    LOG_TOPIC("d921a", INFO, arangodb::Logger::DUMP)
        << "Dump is ignoring collection '" << name
        << "'. Will be created via SmartGraphs of a full dump. If you want "
           "to "
           "dump this collection anyway use 'arangodump --force'. "
           "However this is not recommended and you should instead dump "
           "the edge collection of the SmartGraph instead.";
    return true;
  }
#endif
  return false;
}

arangodb::Result dumpData(arangodb::DumpFeature::Stats& stats,
                          arangodb::maskings::Maskings* maskings,
                          arangodb::ManagedDirectory::File& file,
                          std::string_view body,
                          std::string const& collectionName,
                          bool useVPack) try {
  size_t length;
  if (maskings != nullptr) {
    std::unique_ptr<arangodb::InputProcessor> processor;
    if (useVPack) {
      processor = std::make_unique<arangodb::InputProcessorVPackArray>(body);
    } else {
      processor = std::make_unique<arangodb::InputProcessorJSONL>(body);
    }

    VPackBuilder out;
    out.openArray(/*unindexed*/ true);
    while (processor->valid()) {
      maskings->mask(collectionName, processor->value(), out);
    }
    out.close();
    if (useVPack) {
      length = out.slice().byteSize();
      file.write(out.slice().startAs<char const>(), length);
    } else {
      std::string temp;
      arangodb::velocypack::StringSink sink(&temp);
      arangodb::velocypack::Dumper dumper(&sink);
      for (auto it : arangodb::velocypack::ArrayIterator(out.slice())) {
        if (!temp.empty()) {
          temp.push_back('\n');
        }
        dumper.dump(it);
      }
      if (!temp.empty()) {
        // if we have data, the last line should end with a \n...
        temp.push_back('\n');
      }
      length = temp.length();
      file.write(temp.data(), length);
    }
  } else {
    length = body.length();
    file.write(body.data(), length);
  }

  if (file.status().fail()) {
    return {TRI_ERROR_CANNOT_WRITE_FILE,
            absl::StrCat("cannot write file '", file.path(),
                         "': ", file.status().errorMessage())};
  }

  stats.totalWritten += static_cast<uint64_t>(length);

  return {};
} catch (arangodb::basics::Exception const& ex) {
  return {ex.code(),
          absl::StrCat("caught exception in dumpData for collection '",
                       collectionName, "': ", ex.what())};
} catch (std::exception const& ex) {
  return {TRI_ERROR_INTERNAL,
          absl::StrCat("caught exception in dumpData for collection '",
                       collectionName, "': ", ex.what())};
}

/// @brief dump the actual data from an individual collection
arangodb::Result dumpCollection(arangodb::httpclient::SimpleHttpClient& client,
                                arangodb::DumpFeature::DumpJob& job,
                                arangodb::ManagedDirectory::File& file,
                                std::string const& name,
                                std::string const& server, uint64_t batchId) {
  using arangodb::basics::StringUtils::boolean;
  using arangodb::basics::StringUtils::uint64;
  using arangodb::basics::StringUtils::urlEncode;

  uint64_t chunkSize =
      job.options.initialChunkSize;  // will grow adaptively up to max
  std::string baseUrl =
      absl::StrCat("/_api/replication/dump?collection=", urlEncode(name),
                   "&batchId=", batchId, "&useEnvelope=false",
                   "&array=", (job.options.useVPack ? "true" : "false"));
  if (job.options.clusterMode) {
    // we are in cluster mode, must specify dbserver
    TRI_ASSERT(!server.empty());
    using arangodb::basics::StringUtils::urlEncode;
    baseUrl += absl::StrCat("&DBserver=", urlEncode(server));
  }

  std::unordered_map<std::string, std::string> headers;
  if (job.options.useVPack) {
    headers.emplace(arangodb::StaticStrings::Accept,
                    arangodb::StaticStrings::MimeTypeVPack);
  } else {
    headers.emplace(arangodb::StaticStrings::Accept,
                    arangodb::StaticStrings::MimeTypeDump);
  }

  if (job.options.useGzipForTransport) {
    headers.emplace(arangodb::StaticStrings::AcceptEncoding,
                    arangodb::StaticStrings::EncodingGzip);
  }

  while (true) {
    std::string url = absl::StrCat(baseUrl, "&chunkSize=", chunkSize);

    ++job.stats.totalBatches;  // count how many chunks we are fetching

    // make the actual request for data
    std::unique_ptr<arangodb::httpclient::SimpleHttpResult> response(
        client.request(arangodb::rest::RequestType::GET, url, nullptr, 0,
                       headers));
    auto check = ::arangodb::HttpResponseChecker::check(
        client.getErrorMessage(), response.get());
    if (check.fail()) {
      LOG_TOPIC("ac972", ERR, arangodb::Logger::DUMP)
          << "An error occurred while dumping collection '" << name
          << "' via URL " << url << ": " << check.errorMessage();
      return check;
    }

    // find out whether there are more results to fetch
    bool checkMore = false;

    bool headerExtracted;
    std::string header = response->getHeaderField(
        arangodb::StaticStrings::ReplicationHeaderCheckMore, headerExtracted);
    if (headerExtracted) {
      // first check the basic flag
      checkMore = boolean(header);
    }
    if (!headerExtracted) {  // NOT else, fallthrough from outer or inner above
      return {TRI_ERROR_REPLICATION_INVALID_RESPONSE,
              absl::StrCat("got invalid response from server: required header "
                           "is missing while dumping collection '",
                           name, "'")};
    }

    header = response->getHeaderField(
        arangodb::StaticStrings::ContentTypeHeader, headerExtracted);
    if (!headerExtracted ||
        (job.options.useVPack &&
         header != arangodb::StaticStrings::MimeTypeVPack) ||
        (!job.options.useVPack &&
         !header.starts_with(
             arangodb::StaticStrings::MimeTypeDumpNoEncoding))) {
      return {TRI_ERROR_REPLICATION_INVALID_RESPONSE,
              "got invalid response from server: content-type is invalid"};
    }

    std::string_view body = {response->getBody().c_str(),
                             response->getBody().size()};
    job.stats.totalReceived += static_cast<uint64_t>(body.size());

    LOG_TOPIC("83f66", TRACE, arangodb::Logger::DUMP)
        << "received response body of size " << response->getBody().size()
        << ", type: " << (job.options.useVPack ? "vpack" : "json");

    // transparently uncompress gzip-encoded data
    std::string uncompressed;
    header = response->getHeaderField(arangodb::StaticStrings::ContentEncoding,
                                      headerExtracted);
    if (headerExtracted && header == arangodb::StaticStrings::EncodingGzip) {
      auto res = arangodb::encoding::gzipUncompress(
          reinterpret_cast<uint8_t const*>(body.data()), body.size(),
          uncompressed);
      if (res != TRI_ERROR_NO_ERROR) {
        THROW_ARANGO_EXCEPTION(res);
      }
      body = uncompressed;
    }

    // now actually write retrieved data to dump file.
    arangodb::Result result =
        dumpData(job.stats, job.maskings, file, body, job.collectionName,
                 job.options.useVPack);

    if (result.fail()) {
      return result;
    }

    if (!checkMore) {
      // all done, return successful
      return {};
    }

    // more data to retrieve, adaptively increase chunksize
    if (chunkSize < job.options.maxChunkSize) {
      chunkSize = static_cast<uint64_t>(chunkSize * 1.5);
      if (chunkSize > job.options.maxChunkSize) {
        chunkSize = job.options.maxChunkSize;
      }
    }
  }

  // should never get here, but need to make compiler play nice
  TRI_ASSERT(false);
  return {TRI_ERROR_INTERNAL};
}

/// @brief process a single job from the queue
void processJob(arangodb::httpclient::SimpleHttpClient& client,
                arangodb::DumpFeature::DumpJob& job) {
  arangodb::Result res;
  try {
    res = job.run(client);
  } catch (arangodb::basics::Exception const& ex) {
    res.reset(ex.code(), ex.what());
  } catch (std::exception const& ex) {
    res.reset(TRI_ERROR_INTERNAL, ex.what());
  } catch (...) {
    res.reset(TRI_ERROR_INTERNAL, "unknown exception");
  }

  if (res.fail()) {
    job.feature.reportError(res);
  }
}

/// @brief return either the name of the database to be used as a folder name,
/// or its id if its name contains special characters and is not fully supported
/// in every OS
[[nodiscard]] std::string getDatabaseDirName(std::string const& databaseName,
                                             std::string const& id) {
  bool isOldStyleName =
      arangodb::DatabaseNameValidator::validateName(
          /*allowSystem*/ true, /*extendedNames*/ false, databaseName)
          .ok();
  return isOldStyleName ? databaseName : id;
}

}  // namespace

namespace arangodb {
// job base class
DumpFeature::DumpJob::DumpJob(ManagedDirectory& directory, DumpFeature& feature,
                              Options const& options,
                              maskings::Maskings* maskings, Stats& stats,
                              velocypack::Slice collectionInfo)
    : directory{directory},
      feature{feature},
      options{options},
      maskings{maskings},
      stats{stats},
      collectionInfo{collectionInfo} {
  if (collectionInfo.isNone()) {
    return;
  }
  // extract parameters about the individual collection
  TRI_ASSERT(collectionInfo.isObject());
  VPackSlice parameters = collectionInfo.get("parameters");
  TRI_ASSERT(parameters.isObject());

  // extract basic info about the collection
  collectionName = arangodb::basics::VelocyPackHelper::getStringValue(
      parameters, StaticStrings::DataSourceName, "");
  TRI_ASSERT(!collectionName.empty());
}

DumpFeature::DumpJob::~DumpJob() = default;

DumpFeature::DumpCollectionJob::DumpCollectionJob(
    ManagedDirectory& directory, DumpFeature& feature, Options const& options,
    maskings::Maskings* maskings, Stats& stats,
    velocypack::Slice collectionInfo, uint64_t batchId)
    : DumpJob(directory, feature, options, maskings, stats, collectionInfo),
      batchId(batchId) {}

DumpFeature::DumpCollectionJob::~DumpCollectionJob() = default;

Result DumpFeature::DumpCollectionJob::run(
    arangodb::httpclient::SimpleHttpClient& client) {
  Result res;

  if (options.progress) {
    LOG_TOPIC("a9ec1", INFO, arangodb::Logger::DUMP)
        << "# Dumping collection '" << collectionName << "'...";
  }

  bool dumpStructure = true;
  bool dumpData = options.dumpData;

  if (maskings != nullptr) {
    dumpStructure = maskings->shouldDumpStructure(collectionName);
  }
  if (dumpData && maskings != nullptr) {
    dumpData = maskings->shouldDumpData(collectionName);
  }

  if (!dumpStructure && !dumpData) {
    return res;
  }

  // prep hex string of collection name
  std::string const hexString(
      arangodb::rest::SslInterface::sslMD5(collectionName));

  ++stats.totalCollections;

  // problem: collection name may contain arbitrary characters
  std::string escapedName =
      escapedCollectionName(collectionName, collectionInfo.get("parameters"));

  if (dumpStructure) {
    // save meta data
    auto file = directory.writableFile(
        absl::StrCat(escapedName,
                     (options.clusterMode ? "" : ("_" + hexString)),
                     ".structure.json"),
        true /*overwrite*/, 0, false /*gzipOk*/);
    if (!::fileOk(file.get())) {
      return ::fileError(file.get(), true);
    }

    VPackBuilder excludes;
    {  // { parameters: { shadowCollections: null } }
      VPackObjectBuilder object(&excludes);
      {
        VPackObjectBuilder subObject(&excludes, "parameters");
        subObject->add(StaticStrings::ShadowCollections,
                       VPackSlice::nullSlice());
      }
    }

    VPackBuilder collectionWithExcludedParametersBuilder =
        VPackCollection::merge(collectionInfo, excludes.slice(), true, true);

    std::string const newCollectionInfo =
        collectionWithExcludedParametersBuilder.slice().toJson();

    file->write(newCollectionInfo.data(), newCollectionInfo.size());
    if (file->status().fail()) {
      // close file and bail out
      res = file->status();
    }
  }

  if (res.ok() && !options.useParalleDump) {
    // always create the file so that arangorestore does not complain
    auto file = directory.writableFile(
        absl::StrCat(escapedName, "_", hexString, ".data.",
                     ::getDatafileSuffix(options.useVPack)),
        true /*overwrite*/, 0, true /*gzipOk*/);
    if (!::fileOk(file.get())) {
      return ::fileError(file.get(), true);
    }

    if (dumpData) {
      // save the actual data
      if (options.clusterMode) {
        // multiple shards may write to the same outfile, so turn the unique_ptr
        // into a shared_ptr here
        auto sharedFile =
            std::shared_ptr<arangodb::ManagedDirectory::File>(file.release());

        VPackSlice parameters = collectionInfo.get("parameters");
        VPackSlice shards = parameters.get("shards");

        // Iterate over the Map of shardId to server list
        for (auto const it : VPackObjectIterator(shards)) {
          // extract shard name
          TRI_ASSERT(it.key.isString());
          std::string shardName = it.key.copyString();

          if (!options.shards.empty()) {
            // dump is restricted to specific shards
            if (std::find(options.shards.begin(), options.shards.end(),
                          shardName) == options.shards.end()) {
              // do not dump this shard, as it is not in the include list
              continue;
            }
          }

          // extract dbserver id
          if (!it.value.isArray() || it.value.length() == 0 ||
              !it.value[0].isString()) {
            return {TRI_ERROR_BAD_PARAMETER,
                    "unexpected value for 'shards' attribute"};
          }

          std::string server = it.value[0].copyString();

          // create one new job per shard
          auto dumpJob = std::make_unique<arangodb::DumpFeature::DumpShardJob>(
              directory, feature, options, maskings, stats, collectionInfo,
              shardName, server, sharedFile);
          feature.taskQueue().queueJob(std::move(dumpJob));
        }

        TRI_ASSERT(res.ok());
      } else {
        // keep the batch alive
        ::extendBatch(client, "", batchId);

        // do the hard work in another function...
        res =
            ::dumpCollection(client, *this, *file, collectionName, "", batchId);
      }
    }
  }

  return res;
}

DumpFeature::DumpShardJob::DumpShardJob(
    ManagedDirectory& directory, DumpFeature& feature, Options const& options,
    maskings::Maskings* maskings, Stats& stats,
    velocypack::Slice collectionInfo, std::string const& shardName,
    std::string const& server, std::shared_ptr<ManagedDirectory::File> file)
    : DumpJob(directory, feature, options, maskings, stats, collectionInfo),
      shardName(shardName),
      server(server),
      file(file) {}

Result DumpFeature::DumpShardJob::run(
    arangodb::httpclient::SimpleHttpClient& client) {
  if (options.progress) {
    LOG_TOPIC("a27be", INFO, arangodb::Logger::DUMP)
        << "# Dumping shard '" << shardName << "' of collection '"
        << collectionName << "' from DBserver '" << server << "'...";
  }

  // make sure we have a batch on this dbserver
  auto [res, batchId] = ::startBatch(client, server);
  if (res.ok()) {
    // do the hard work elsewhere
    res = ::dumpCollection(client, *this, *file, shardName, server, batchId);
    ::endBatch(client, server, batchId);
  }

  return res;
}

DumpFeature::DumpFeature(Server& server, int& exitCode)
    : ArangoDumpFeature{server, *this},
      _clientManager{server.getFeature<HttpEndpointProvider, ClientFeature>(),
                     Logger::DUMP},
      _clientTaskQueue{server, ::processJob},
      _exitCode{exitCode} {
  setOptional(false);
  startsAfter<application_features::BasicFeaturePhaseClient>();
  if constexpr (Server::contains<BumpFileDescriptorsFeature>()) {
    startsAfter<BumpFileDescriptorsFeature>();
  }

  using arangodb::basics::FileUtils::buildFilename;
  using arangodb::basics::FileUtils::currentDirectory;
  _options.outputPath = buildFilename(currentDirectory().result(), "dump");
  _options.threadCount =
      std::max(uint32_t(_options.threadCount),
               static_cast<uint32_t>(NumberOfCores::getValue()));
}

void DumpFeature::collectOptions(
    std::shared_ptr<options::ProgramOptions> options) {
  using arangodb::options::BooleanParameter;
  using arangodb::options::StringParameter;
  using arangodb::options::UInt32Parameter;
  using arangodb::options::UInt64Parameter;
  using arangodb::options::VectorParameter;

  options->addOption(
      "--collection",
      "Restrict the dump to this collection name (can be specified multiple "
      "times).",
      new VectorParameter<StringParameter>(&_options.collections));

  options
      ->addOption(
          "--shard",
          "Restrict the dump to this shard (can be specified multiple times).",
          new VectorParameter<StringParameter>(&_options.shards))
      .setIntroducedIn(30800);

  options->addOption("--initial-batch-size",
                     "The initial size for individual data batches (in bytes).",
                     new UInt64Parameter(&_options.initialChunkSize));

  options->addOption("--batch-size",
                     "The maximum size for individual data batches (in bytes).",
                     new UInt64Parameter(&_options.maxChunkSize));

  options->addOption(
      "--threads",
      "The maximum number of collections/shards to process in parallel.",
      new UInt32Parameter(&_options.threadCount),
      arangodb::options::makeDefaultFlags(arangodb::options::Flags::Dynamic));

  options->addOption("--dump-data", "Whether to dump collection data.",
                     new BooleanParameter(&_options.dumpData));

  options
      ->addOption("--dump-views", "Whether to dump view definitions.",
                  new BooleanParameter(&_options.dumpViews))
      .setIntroducedIn(31100);

  options->addOption("--all-databases", "Whether to dump all databases.",
                     new BooleanParameter(&_options.allDatabases));

  options->addOption(
      "--force",
      "Continue dumping even in the face of some server-side errors.",
      new BooleanParameter(&_options.force));

  options->addOption(
      "--ignore-distribute-shards-like-errors",
      "Continue dumping even if a sharding prototype collection is "
      "not backed up, too.",
      new BooleanParameter(&_options.ignoreDistributeShardsLikeErrors));

  options->addOption("--include-system-collections",
                     "Include system collections.",
                     new BooleanParameter(&_options.includeSystemCollections));

  options->addOption("--output-directory", "The output directory.",
                     new StringParameter(&_options.outputPath));

  options->addOption("--overwrite", "Overwrite data in the output directory.",
                     new BooleanParameter(&_options.overwrite));

  options->addOption("--progress", "Show the progress.",
                     new BooleanParameter(&_options.progress));

  options->addObsoleteOption(
      "--envelope",
      "Wrap each document into a {type, data} envelope "
      "(this is required for compatibility with v3.7 and before).",
      false);

  options->addObsoleteOption("--tick-start",
                             "Only include data after this tick.", true);

  options->addObsoleteOption("--tick-end",
                             "Last tick to be included in data dump.", true);

  options->addOption("--maskings", "A path to a file with masking definitions.",
                     new StringParameter(&_options.maskingsFile));

  options->addOption("--compress-output",
                     "Compress files containing collection contents using the "
                     "gzip format.",
                     new BooleanParameter(&_options.useGzipForStorage));

  options
      ->addOption("--compress-transfer",
                  "Compress data for transport using the gzip format.",
                  new BooleanParameter(&_options.useGzipForTransport),
                  arangodb::options::makeDefaultFlags(
                      arangodb::options::Flags::Experimental,
                      arangodb::options::Flags::Uncommon))
      .setIntroducedIn(31200);

  options
      ->addOption("--dump-vpack",
                  "Dump collection data in velocypack format (more compact "
                  "than JSON, but requires ArangoDB 3.12 or higher to restore)",
                  new BooleanParameter(&_options.useVPack),
                  arangodb::options::makeDefaultFlags(
                      arangodb::options::Flags::Experimental,
                      arangodb::options::Flags::Uncommon))
      .setIntroducedIn(31200);

  options
      ->addOption("--parallel-dump", "Enable experimental dump behavior.",
                  new BooleanParameter(&_options.useParalleDump),
                  arangodb::options::makeDefaultFlags(
                      arangodb::options::Flags::Experimental,
                      arangodb::options::Flags::Uncommon))
      .setIntroducedIn(31200);
  // option was renamed in 3.12
  options->addOldOption("--use-experimental-dump", "--parallel-dump");

  options
      ->addOption(
          "--split-files",
          "Split a collection in multiple files to increase throughput.",
          new BooleanParameter(&_options.splitFiles),
          arangodb::options::makeDefaultFlags(
              arangodb::options::Flags::Uncommon))
      .setLongDescription(R"(This option only has effect when the option
`--parallel-dump` is set to `true`. Restoring split files also
requires an arangorestore version that is capable of restoring data of a
single collection/shard from multiple files.)")
      .setIntroducedIn(31200);

  options
      ->addOption("--dbserver-worker-threads",
                  "Number of worker threads on each dbserver.",
                  new UInt64Parameter(&_options.dbserverWorkerThreads),
                  arangodb::options::makeDefaultFlags(
                      arangodb::options::Flags::Uncommon))
      .setIntroducedIn(31200);

  options
      ->addOption("--dbserver-prefetch-batches",
                  "Number of batches to prefetch on each dbserver.",
                  new UInt64Parameter(&_options.dbserverPrefetchBatches),
                  arangodb::options::makeDefaultFlags(
                      arangodb::options::Flags::Uncommon))
      .setIntroducedIn(31200);

  options
      ->addOption("--local-writer-threads", "Number of local writer threads.",
                  new UInt64Parameter(&_options.localWriterThreads),
                  arangodb::options::makeDefaultFlags(
                      arangodb::options::Flags::Uncommon))
      .setIntroducedIn(31200);

  options
      ->addOption("--local-network-threads",
                  "Number of local network threads, i.e. how many requests "
                  "are sent in parallel.",
                  new UInt64Parameter(&_options.dbserverWorkerThreads),
                  arangodb::options::makeDefaultFlags(
                      arangodb::options::Flags::Uncommon))
      .setIntroducedIn(31200);
}

void DumpFeature::validateOptions(
    std::shared_ptr<options::ProgramOptions> options) {
  auto const& positionals = options->processingResult()._positionals;
  size_t n = positionals.size();

  if (1 == n) {
    _options.outputPath = positionals[0];
  } else if (1 < n) {
    LOG_TOPIC("a62e0", FATAL, arangodb::Logger::DUMP)
        << "expecting at most one directory, got " +
               arangodb::basics::StringUtils::join(positionals, ", ");
    FATAL_ERROR_EXIT();
  }

  // clamp chunk values to allowed ranges
  _options.initialChunkSize =
      std::clamp(_options.initialChunkSize, ::MinChunkSize, ::MaxChunkSize);
  _options.maxChunkSize = std::clamp(_options.maxChunkSize,
                                     _options.initialChunkSize, ::MaxChunkSize);

  if (options->processingResult().touched("server.database") &&
      _options.allDatabases) {
    LOG_TOPIC("17e2b", FATAL, arangodb::Logger::DUMP)
        << "cannot use --server.database and --all-databases at the same time";
    FATAL_ERROR_EXIT();
  }

  // trim trailing slash from path because it may cause problems on ...
  // Windows
  if (!_options.outputPath.empty() &&
      _options.outputPath.back() == TRI_DIR_SEPARATOR_CHAR) {
    TRI_ASSERT(_options.outputPath.size() > 0);
    _options.outputPath.pop_back();
  }
  TRI_NormalizePath(_options.outputPath);

  uint32_t clamped =
      std::clamp(_options.threadCount, uint32_t(1),
                 4 * static_cast<uint32_t>(NumberOfCores::getValue()));
  if (_options.threadCount != clamped) {
    LOG_TOPIC("0460e", WARN, Logger::DUMP)
        << "capping --threads value to " << clamped;
    _options.threadCount = clamped;
  }

  if (_options.splitFiles && !_options.useParalleDump) {
    LOG_TOPIC("b0cbe", FATAL, Logger::DUMP)
        << "--split-files is only available when using "
           "--parallel-dump.";
    FATAL_ERROR_EXIT();
  }
}

// dump data from cluster via a coordinator
Result DumpFeature::runClusterDump(httpclient::SimpleHttpClient& client,
                                   std::string const& dbName) {
  // get the cluster inventory
  std::string const url =
      absl::StrCat("/_api/replication/clusterInventory?includeSystem=",
                   (_options.includeSystemCollections ? "true" : "false"));

  return runDump(client, url, dbName, 0);
}

// dump data from single server
Result DumpFeature::runSingleDump(httpclient::SimpleHttpClient& client,
                                  std::string const& dbName) {
  Result res;
  uint64_t batchId;
  std::tie(res, batchId) = ::startBatch(client, "");
  if (res.fail()) {
    return res;
  }
  auto sg = arangodb::scopeGuard([&]() noexcept {
    try {
      ::endBatch(client, "", batchId);
    } catch (std::exception const& ex) {
      LOG_TOPIC("c4938", ERR, Logger::DUMP)
          << "Failed to end batch: " << ex.what();
    }
  });

  // get the cluster inventory
  std::string const url =
      absl::StrCat("/_api/replication/inventory?includeSystem=",
                   (_options.includeSystemCollections ? "true" : "false"),
                   "&includeFoxxQueues=",
                   (_options.includeSystemCollections ? "true" : "false"),
                   "&batchId=", batchId);

  return runDump(client, url, dbName, batchId);
}

Result DumpFeature::runDump(httpclient::SimpleHttpClient& client,
                            std::string const& baseUrl,
                            std::string const& dbName, uint64_t batchId) {
  std::unique_ptr<httpclient::SimpleHttpResult> response(
      client.request(rest::RequestType::GET, baseUrl, nullptr, 0));
  auto check = ::arangodb::HttpResponseChecker::check(client.getErrorMessage(),
                                                      response.get());
  if (check.fail()) {
    LOG_TOPIC("eb7f4", ERR, arangodb::Logger::DUMP)
        << "An error occurred while fetching inventory: "
        << check.errorMessage();
    return check;
  }

  // parse the inventory vpack body
  std::shared_ptr<VPackBuilder> parsedBody;
  try {
    parsedBody = response->getBodyVelocyPack();
  } catch (...) {
    return ::ErrorMalformedJsonResponse;
  }
  VPackSlice body = parsedBody->slice();
  if (!body.isObject()) {
    return ::ErrorMalformedJsonResponse;
  }

  if (_options.allDatabases) {
    std::string const dbId = body.get("properties").get("id").copyString();
    // inject current database
    LOG_TOPIC("4af42", INFO, Logger::DUMP)
        << "Dumping database '" << dbName << "' (" << dbId << ")";

    EncryptionFeature* encryption{};
    if constexpr (Server::contains<EncryptionFeature>()) {
      if (server().hasFeature<EncryptionFeature>()) {
        encryption = &server().getFeature<EncryptionFeature>();
      }
    }

    _directory = std::make_unique<ManagedDirectory>(
        encryption,
        arangodb::basics::FileUtils::buildFilename(
            _options.outputPath, ::getDatabaseDirName(dbName, dbId)),
        !_options.overwrite, true, _options.useGzipForStorage);

    if (_directory->status().fail()) {
      LOG_TOPIC("94201", ERR, Logger::DUMP)
          << _directory->status().errorMessage();
      return _directory->status();
    }
  }

  // parse collections array
  VPackSlice collections = body.get("collections");
  if (!collections.isArray()) {
    return ::ErrorMalformedJsonResponse;
  }

  // get the view list
  VPackSlice views = body.get("views");
  if (!views.isArray()) {
    views = VPackSlice::emptyArraySlice();
  }

  // Step 1. Store database properties files
  Result res = storeDumpJson(body, dbName);
  if (res.fail()) {
    return res;
  }

  // Step 2. Store view definition files
  if (_options.dumpViews) {
    res = storeViews(views);
    if (res.fail()) {
      return res;
    }
  }

  // create a lookup table for collections
  std::map<std::string, arangodb::velocypack::Slice> restrictList;
  for (auto const& name : _options.collections) {
    restrictList.emplace(name, arangodb::velocypack::Slice::noneSlice());
  }
  // restrictList now contains all collections the user has requested (can be
  // empty)

  // Step 3. iterate over collections
  for (auto collection : VPackArrayIterator(collections)) {
    // extract parameters about the individual collection
    if (!collection.isObject()) {
      return ::ErrorMalformedJsonResponse;
    }
    VPackSlice parameters = collection.get("parameters");

    if (!parameters.isObject()) {
      return ::ErrorMalformedJsonResponse;
    }

    // extract basic info about the collection
    uint64_t const cid = basics::VelocyPackHelper::extractIdValue(parameters);
    std::string const name = arangodb::basics::VelocyPackHelper::getStringValue(
        parameters, StaticStrings::DataSourceName, "");
    bool const deleted = arangodb::basics::VelocyPackHelper::getBooleanValue(
        parameters, StaticStrings::DataSourceDeleted.c_str(), false);

    // simple filtering
    if (cid == 0 || name.empty()) {
      return ::ErrorMalformedJsonResponse;
    }
    if (deleted) {
      continue;
    }
    if (name.starts_with('_') && !_options.includeSystemCollections) {
      // exclude system collections
      continue;
    }

    // filter by specified names
    if (!_options.collections.empty() && !restrictList.contains(name)) {
      // collection name not in list
      continue;
    }

    if (isIgnoredHiddenEnterpriseCollection(_options, name)) {
      continue;
    }

    // verify distributeShardsLike info
    if (!_options.ignoreDistributeShardsLikeErrors) {
      std::string prototypeCollection =
          basics::VelocyPackHelper::getStringValue(
              parameters, StaticStrings::DistributeShardsLike, "");

      if (!prototypeCollection.empty() && !_options.collections.empty()) {
        if (std::find(_options.collections.begin(), _options.collections.end(),
                      prototypeCollection) == _options.collections.end()) {
          return {
              TRI_ERROR_INTERNAL,
              absl::StrCat(
                  "Collection ", name,
                  "'s shard distribution is based on that of collection ",
                  prototypeCollection,
                  ", which is not dumped along. You may dump the collection "
                  "regardless of the missing prototype collection by using "
                  "the "
                  "--ignore-distribute-shards-like-errors parameter.")};
        }
      }
    }

    restrictList[name] = collection;
  }

  // now check if at least one of the specified collections was found
  if (!_options.collections.empty() &&
      std::all_of(restrictList.begin(), restrictList.end(),
                  [](auto const& it) { return it.second.isNone(); })) {
    LOG_TOPIC("11523", FATAL, arangodb::Logger::DUMP)
        << "None of the requested collections were found in the database";
    FATAL_ERROR_EXIT();
  }

  std::unordered_map<
      std::string,
      std::unordered_map<std::string, ParallelDumpServer::ShardInfo>>
      shardsByServer;
  std::shared_ptr<DumpFileProvider> fileProvider;

  for (auto const& [name, collectionInfo] : restrictList) {
    if (collectionInfo.isNone()) {
      LOG_TOPIC("e650c", WARN, arangodb::Logger::DUMP)
          << "Requested collection '" << name << "' not found in database";
      continue;
    }

    if (_options.useParalleDump) {
      if (_options.clusterMode) {
        // cluster: now build a list of shards for each server
        for (auto const& [shard, servers] : VPackObjectIterator(
                 collectionInfo.get("parameters").get("shards"))) {
          TRI_ASSERT(servers.isArray());
          auto serverStr = servers.at(0).copyString();
          auto shardStr = shard.copyString();

          if (!_options.shards.empty()) {
            // dump is restricted to specific shards
            if (std::find(_options.shards.begin(), _options.shards.end(),
                          shardStr) == _options.shards.end()) {
              // do not dump this shard, as it is not in the include list
              continue;
            }
          }
          TRI_ASSERT(!serverStr.empty());
          shardsByServer[std::move(serverStr)][std::move(shardStr)]
              .collectionName = name;
        }
      } else {
        // single server mode: all "shards" are on one server
        TRI_ASSERT(!_options.clusterMode);
        shardsByServer[""][name].collectionName = name;
      }
    }

    // queue job to actually dump collection
    auto dumpJob = std::make_unique<DumpCollectionJob>(
        *_directory, *this, _options, _maskings.get(), _stats, collectionInfo,
        batchId);
    _clientTaskQueue.queueJob(std::move(dumpJob));
  }

  if (_options.useParalleDump) {
    // now start jobs for each dbserver
    fileProvider = std::make_shared<DumpFileProvider>(
        *_directory, restrictList, _options.splitFiles, _options.useVPack);

    for (auto& [dbserver, shards] : shardsByServer) {
      auto job = std::make_unique<ParallelDumpServer>(
          *_directory, *this, _clientManager, _options, _maskings.get(), _stats,
          fileProvider, std::move(shards), dbserver);
      _clientTaskQueue.queueJob(std::move(job));
    }
  }

  // wait for all jobs to finish, then check for errors
  _clientTaskQueue.waitForIdle();
  {
    std::lock_guard lock{_workerErrorLock};
    if (!_workerErrors.empty()) {
      return _workerErrors.front();
    }
  }

  return {};
}

Result DumpFeature::storeDumpJson(VPackSlice body,
                                  std::string const& dbName) const {
  // read the server's max tick value
  std::string tickString =
      basics::VelocyPackHelper::getStringValue(body, "tick", "");
  if (tickString == "") {
    return ::ErrorMalformedJsonResponse;
  }
  LOG_TOPIC("e4134", INFO, Logger::DUMP)
      << "Last tick provided by server is: " << tickString;

  try {
    std::string dateString;
    LogTimeFormats::writeTime(dateString,
                              LogTimeFormats::TimeFormat::UTCDateString,
                              std::chrono::system_clock::now());

    VPackBuilder meta;
    meta.openObject();
    meta.add("database", VPackValue(dbName));
    meta.add("createdAt", VPackValue(dateString));
    meta.add("lastTickAtDumpStart", VPackValue(tickString));
    meta.add("useEnvelope", VPackValue(false));
    meta.add("useVPack", VPackValue(_options.useVPack));
    auto props = body.get("properties");
    if (props.isObject()) {
      meta.add("properties", props);
    }
    meta.close();

    // save last tick in file
    auto file = _directory->writableFile("dump.json", true, 0, false);
    if (!::fileOk(file.get())) {
      return ::fileError(file.get(), true);
    }

    std::string metaString = meta.slice().toJson();
    file->write(metaString.data(), metaString.size());
    if (file->status().fail()) {
      return file->status();
    }
  } catch (basics::Exception const& ex) {
    return {ex.code(), ex.what()};
  } catch (std::exception const& ex) {
    return {TRI_ERROR_INTERNAL, ex.what()};
  } catch (...) {
    return {TRI_ERROR_OUT_OF_MEMORY, "out of memory"};
  }
  return {};
}

Result DumpFeature::storeViews(VPackSlice views) const {
  for (VPackSlice view : VPackArrayIterator(views)) {
    auto nameSlice = view.get(StaticStrings::DataSourceName);
    if (!nameSlice.isString() || nameSlice.getStringLength() == 0) {
      continue;  // ignore
    }

    // problem: name of view may contain arbitrary characters
    std::string escapedName = escapedViewName(nameSlice.copyString(), view);

    try {
      escapedName.append(".view.json");
      // save last tick in file
      auto file = _directory->writableFile(escapedName, true, 0, false);
      if (!::fileOk(file.get())) {
        return ::fileError(file.get(), true);
      }

      std::string const viewString = view.toJson();
      file->write(viewString.c_str(), viewString.size());
      if (file->status().fail()) {
        return file->status();
      }
    } catch (basics::Exception const& ex) {
      return {ex.code(), ex.what()};
    } catch (std::exception const& ex) {
      return {TRI_ERROR_INTERNAL, ex.what()};
    } catch (...) {
      return {TRI_ERROR_OUT_OF_MEMORY, "out of memory"};
    }
  }
  return {};
}

void DumpFeature::reportError(Result const& error) {
  try {
    {
      std::lock_guard lock{_workerErrorLock};
      _workerErrors.emplace_back(error);
    }
    _clientTaskQueue.clearQueue();
  } catch (...) {
  }
}

ClientTaskQueue<DumpFeature::DumpJob>& DumpFeature::taskQueue() {
  return _clientTaskQueue;
}

void DumpFeature::start() {
  using arangodb::basics::StringUtils::formatSize;

  if (!_options.maskingsFile.empty()) {
    maskings::MaskingsResult m =
        maskings::Maskings::fromFile(_options.maskingsFile);

    if (m.status != maskings::MaskingsResult::VALID) {
      LOG_TOPIC("cabd7", FATAL, Logger::CONFIG)
          << m.message << " in maskings file '" << _options.maskingsFile << "'";
      FATAL_ERROR_EXIT();
    }

    _maskings = std::move(m.maskings);
  }

  _exitCode = EXIT_SUCCESS;

  // generate a fake client id that we send to the server
  // TODO: convert this into a proper string "arangodump-<numeric id>"
  // in the future, if we are sure the server is an ArangoDB 3.5 or
  // higher
  ::clientId = std::to_string(
      RandomGenerator::interval(static_cast<uint64_t>(0x0000FFFFFFFFFFFFULL)));
  ::syncerId = std::to_string(
      RandomGenerator::interval(static_cast<uint64_t>(0xFFFFFFFFFFFFFFFFULL)));

  double const start = TRI_microtime();

  EncryptionFeature* encryption{};
  if constexpr (Server::contains<EncryptionFeature>()) {
    if (server().hasFeature<EncryptionFeature>()) {
      encryption = &server().getFeature<EncryptionFeature>();
    }
  }

  // set up the output directory, not much else
  _directory = std::make_unique<ManagedDirectory>(
      encryption, _options.outputPath, !_options.overwrite, true,
      _options.useGzipForStorage);
  if (_directory->status().fail()) {
    switch (static_cast<int>(_directory->status().errorNumber())) {
      case static_cast<int>(TRI_ERROR_FILE_EXISTS):
        LOG_TOPIC("efed0", FATAL, Logger::DUMP)
            << "cannot write to output directory '" << _options.outputPath
            << "'";
        break;
      case static_cast<int>(TRI_ERROR_CANNOT_OVERWRITE_FILE):
        LOG_TOPIC("bd7fe", FATAL, Logger::DUMP)
            << "output directory '" << _options.outputPath
            << "' already exists. use \"--overwrite true\" to "
               "overwrite data in it";
        break;
      default:
        LOG_TOPIC("8f227", ERR, Logger::DUMP)
            << _directory->status().errorMessage();
        break;
    }
    FATAL_ERROR_EXIT();
  }

  // get database name to operate on
  auto& client = server().getFeature<HttpEndpointProvider, ClientFeature>();
  // get a client to use in main thread
  auto httpClient =
      _clientManager.getConnectedClient(_options.force, true, true, 0);

  // check if we are in cluster or single-server mode
  Result result{TRI_ERROR_NO_ERROR};
  std::string role;
  std::tie(result, role) = _clientManager.getArangoIsCluster(*httpClient);
  _options.clusterMode = (role == "COORDINATOR");
  if (result.fail()) {
    LOG_TOPIC("8ba2f", FATAL, arangodb::Logger::DUMP)
        << "Error: could not detect ArangoDB instance type: "
        << result.errorMessage();
    FATAL_ERROR_EXIT();
  }

  if (role == "PRIMARY") {
    LOG_TOPIC("eeabc", WARN, arangodb::Logger::DUMP)
        << "You connected to a DBServer node, but operations in a cluster "
           "should be carried out via a Coordinator. This is an unsupported "
           "operation!";
  }

  // set up threads and workers
  _clientTaskQueue.spawnWorkers(_clientManager, _options.threadCount);

  if (_options.progress) {
    LOG_TOPIC("f3a1f", INFO, Logger::DUMP)
        << "Connected to ArangoDB '" << client.endpoint() << "', database: '"
        << client.databaseName() << "', username: '" << client.username()
        << "'";

    LOG_TOPIC("5e989", INFO, Logger::DUMP)
        << "Writing dump to output directory '" << _directory->path()
        << "' with " << _options.threadCount << " thread(s)";
  }

  // final result
  Result res;

  std::vector<std::string> databases;
  if (_options.allDatabases) {
    // get list of available databases
    std::tie(res, databases) = ::getDatabases(*httpClient);
  } else {
    // use just the single database that was specified
    databases.push_back(client.databaseName());
  }

  if (res.ok()) {
    for (auto const& db : databases) {
      if (_options.allDatabases) {
        client.setDatabaseName(db);
        httpClient =
            _clientManager.getConnectedClient(_options.force, false, true, 0);
      }

      try {
        // if any of the specified collections is a system collection, we
        // auto-enable --include-system-collections for the user
        _options.includeSystemCollections |= std::any_of(
            _options.collections.begin(), _options.collections.end(),
            [&](auto const& name) { return name.starts_with('_'); });

        if (_options.clusterMode) {
          res = runClusterDump(*httpClient, db);
        } else {
          res = runSingleDump(*httpClient, db);
        }
      } catch (basics::Exception const& ex) {
        LOG_TOPIC("771d0", ERR, Logger::DUMP)
            << "caught exception: " << ex.what();
        res = {ex.code(), ex.what()};
      } catch (std::exception const& ex) {
        LOG_TOPIC("ad866", ERR, Logger::DUMP)
            << "caught exception: " << ex.what();
        res = {TRI_ERROR_INTERNAL, ex.what()};
      } catch (...) {
        LOG_TOPIC("7d8c3", ERR, Logger::DUMP) << "caught unknown exception";
        res = {TRI_ERROR_INTERNAL};
      }

      if (res.fail() && !_options.force) {
        break;
      }
    }
  }

  if (res.fail()) {
    LOG_TOPIC("f7ff5", ERR, Logger::DUMP)
        << "An error occurred: " << res.errorMessage();
    _exitCode = EXIT_FAILURE;
  }

  if (_options.progress) {
    double totalTime = TRI_microtime() - start;
    size_t totalSize = 0;

    try {
      auto list = basics::FileUtils::listFiles(_options.outputPath);
      for (auto const& it : list) {
        auto f = absl::StrCat(
            basics::FileUtils::buildFilename(_options.outputPath, it));
        if (basics::FileUtils::isRegularFile(f)) {
          totalSize += basics::FileUtils::size(f);
        }
      }
    } catch (...) {
    }

    if (_options.dumpData) {
      LOG_TOPIC("66c0e", INFO, Logger::DUMP)
          << "Processed " << _stats.totalCollections.load()
          << " collection(s) from " << databases.size() << " database(s) in "
          << Logger::FIXED(totalTime, 2) << " s total time. Retrieved "
          << formatSize(_stats.totalReceived.load()) << " from server, sent "
          << _stats.totalBatches.load()
          << " batch(es) in total. Total written to disk (before compression): "
          << formatSize(_stats.totalWritten.load())
          << ". Size of dump directory on disk (after compression): "
          << formatSize(totalSize);
    } else {
      LOG_TOPIC("aaa17", INFO, Logger::DUMP)
          << "Processed " << _stats.totalCollections.load()
          << " collection(s) from " << databases.size() << " database(s) in "
          << Logger::FIXED(totalTime, 2)
          << " s total time. Size of dump directory on disk: "
          << formatSize(totalSize);
    }
  }
}

namespace {
bool shouldRetryRequest(httpclient::SimpleHttpResult const* response,
                        Result const& check) {
  if (response != nullptr) {
    // check for retryable errors in simple http client
    switch (response->getResultType()) {
      case httpclient::SimpleHttpResult::COULD_NOT_CONNECT:
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        return true;
      case httpclient::SimpleHttpResult::WRITE_ERROR:
      case httpclient::SimpleHttpResult::READ_ERROR:
        return true;  // retry loop
      default:
        break;
    }
  }

  if (check.is(TRI_ERROR_CLUSTER_TIMEOUT) ||
      check.is(TRI_ERROR_HTTP_GATEWAY_TIMEOUT)) {
    // retry
    return true;
  }

  return false;
}
}  // namespace

void DumpFeature::ParallelDumpServer::createDumpContext(
    httpclient::SimpleHttpClient& client) {
  VPackBuilder builder;
  {
    VPackObjectBuilder ob(&builder);
    builder.add("batchSize", VPackValue(options.maxChunkSize));
    builder.add("prefetchCount", VPackValue(options.dbserverPrefetchBatches));
    builder.add("parallelism", VPackValue(options.dbserverWorkerThreads));
    {
      VPackArrayBuilder ab(&builder, "shards");
      for (auto const& [shard, info] : shards) {
        builder.add(VPackValue(shard));
      }
    }
  }

  auto bodystr = builder.toJson();
  size_t retryCount = 100;

  using arangodb::basics::StringUtils::urlEncode;

  std::string url = absl::StrCat("/_api/dump/start?useVPack=",
                                 (options.useVPack ? "true" : "false"));
  if (!server.empty()) {
    url += absl::StrCat("&dbserver=", urlEncode(server));
  }
  std::unique_ptr<arangodb::httpclient::SimpleHttpResult> response;
  while (true) {
    response.reset(client.request(arangodb::rest::RequestType::POST, url,
                                  bodystr.c_str(), bodystr.size(), {}));

    auto check = ::arangodb::HttpResponseChecker::check(
        client.getErrorMessage(), response.get());
    if (check.fail()) {
      LOG_TOPIC("45d6e", ERR, Logger::DUMP)
          << "An error occurred while creating a dump context"
          << serverLabel(server) << ": " << check;
      bool const retry = shouldRetryRequest(response.get(), check);

      if (retry && --retryCount > 0) {
        continue;
      }

      if (retryCount == 0) {
        LOG_TOPIC("7a3e4", ERR, Logger::DUMP) << "Too many connection errors.";
      }
      LOG_TOPIC("bdecf", FATAL, Logger::DUMP)
          << "failed to create dump context" << serverLabel(server) << ": "
          << check.errorMessage();
      FATAL_ERROR_EXIT();
    } else {
      break;
    }
  }

  bool headerExtracted;
  dumpId = response->getHeaderField(StaticStrings::DumpId, headerExtracted);
  if (!headerExtracted) {
    LOG_TOPIC("d7a76", FATAL, Logger::DUMP)
        << "dump create response did not contain any dump id"
        << serverLabel(server) << ". body: " << response->getBody();
    FATAL_ERROR_EXIT();
  }
}

void DumpFeature::ParallelDumpServer::finishDumpContext(
    httpclient::SimpleHttpClient& client) {
  using arangodb::basics::StringUtils::urlEncode;
  auto url = absl::StrCat("/_api/dump/", dumpId);
  if (!server.empty()) {
    url += absl::StrCat("?dbserver=", urlEncode(server));
  }
  std::unique_ptr<arangodb::httpclient::SimpleHttpResult> response(
      client.request(arangodb::rest::RequestType::DELETE_REQ, url, nullptr, 0,
                     {}));
  auto check = ::arangodb::HttpResponseChecker::check(client.getErrorMessage(),
                                                      response.get());
  if (check.fail()) {
    LOG_TOPIC("bdedf", WARN, Logger::DUMP)
        << "failed to finish dump context" << serverLabel(server) << ": "
        << check;
  }
}

Result DumpFeature::ParallelDumpServer::run(
    httpclient::SimpleHttpClient& client) {
  LOG_TOPIC("23f92", INFO, Logger::DUMP)
      << "preparing data stream" << serverLabel(server) << ", using "
      << options.dbserverWorkerThreads << " DBServer worker thread(s), "
      << options.localNetworkThreads << " network thread(s), "
      << options.localWriterThreads
      << " local writer thread(s), number of prefetch batches: "
      << options.dbserverPrefetchBatches;

  // create context on dbserver
  createDumpContext(client);

  std::vector<std::thread> threads;

  auto threadGuard = scopeGuard([&threads]() noexcept {
    // on our way out, we wait for all threads to join
    for (auto& thrd : threads) {
      thrd.join();
    }
    threads.clear();
  });

  // start n network threads
  for (size_t i = 0; i < options.localNetworkThreads; i++) {
    threads.emplace_back([&, i, guard = BoundedChannelProducerGuard{queue}] {
      runNetworkThread(i);
    });
  }

  // start k writer threads
  for (size_t i = 0; i < options.localWriterThreads; i++) {
    threads.emplace_back(&ParallelDumpServer::runWriterThread, this);
  }

  threadGuard.fire();

  // remove dump context from server - get a new client because the old might
  // already be disconnected.
  finishDumpContext(*clientManager.getConnectedClient(true, false, false, 0));

  printBlockStats();

  LOG_TOPIC("1b7fe", INFO, Logger::DUMP) << "all data received for " << server;

  return {};
}

void DumpFeature::ParallelDumpServer::ParallelDumpServer::printBlockStats() {
  const char* locations[] = {
      "writer threads (+) / network threads (-)",
      "dbserver worker put batch (+) / rest handler get batch (-)",
  };

  std::string msg;
  for (size_t i = 0; i < 2; i++) {
    if (i > 0) {
      msg += ", ";
    }
    msg += locations[i];
    msg += " = ";
    msg += std::to_string(blockCounter[i]);
  }

  LOG_TOPIC("d1349", DEBUG, Logger::DUMP) << "block counter " << msg;
}

void DumpFeature::ParallelDumpServer::ParallelDumpServer::countBlocker(
    BlockAt where, int64_t c) {
  /* clang-format off */
  char const* locations[] = {
      "writer threads - consider increasing the number of network threads",
      "network threads - consider increasing the number of local writer threads",
      "dbserver get batch - consider increasing the parallelism on dbservers",
      "dbserver put batch - consider increasing the number of network threads",
  };
  /* clang-format on */
  char const* msg = nullptr;
  auto actual = blockCounter[where].fetch_add(c);
  if (actual == 100) {
    msg = locations[2 * where];
    blockCounter[where].fetch_sub(100);
  } else if (actual == -100) {
    msg = locations[2 * where + 1];
    blockCounter[where].fetch_add(100);
  }

  if (msg) {
    LOG_TOPIC("3cc53", DEBUG, Logger::DUMP)
        << "when dumping data" << serverLabel(server) << " system blocking at "
        << msg;
  }
}

DumpFeature::ParallelDumpServer::ParallelDumpServer(
    ManagedDirectory& directory, DumpFeature& feature,
    ClientManager& clientManager, DumpFeature::Options const& options,
    maskings::Maskings* maskings, DumpFeature::Stats& stats,
    std::shared_ptr<DumpFileProvider> fileProvider,
    std::unordered_map<std::string, ShardInfo> shards, std::string server)
    : DumpJob(directory, feature, options, maskings, stats,
              VPackSlice::noneSlice()),
      clientManager(clientManager),
      fileProvider(std::move(fileProvider)),
      shards(std::move(shards)),
      server(std::move(server)),
      queue(options.localWriterThreads) {
  TRI_ASSERT(options.clusterMode != this->server.empty());
}

std::unique_ptr<httpclient::SimpleHttpResult>
DumpFeature::ParallelDumpServer::receiveNextBatch(
    httpclient::SimpleHttpClient& client, std::uint64_t batchId,
    std::optional<std::uint64_t> lastBatch) {
  using arangodb::basics::StringUtils::urlEncode;
  std::string url =
      absl::StrCat("/_api/dump/next/", dumpId, "?batchId=", batchId);
  if (!server.empty()) {
    url += absl::StrCat("&dbserver=", urlEncode(server));
  }
  if (lastBatch) {
    url += "&lastBatch=" + std::to_string(*lastBatch);
  }

  std::unordered_map<std::string, std::string> headers;
  if (options.useGzipForTransport) {
    headers.emplace(arangodb::StaticStrings::AcceptEncoding,
                    arangodb::StaticStrings::EncodingGzip);
  }

  std::size_t retryCounter = 100;

  while (true) {
    std::unique_ptr<arangodb::httpclient::SimpleHttpResult> response(
        client.request(arangodb::rest::RequestType::POST, url, nullptr, 0,
                       headers));
    auto check = ::arangodb::HttpResponseChecker::check(
        client.getErrorMessage(), response.get());
    if (check.fail()) {
      LOG_TOPIC("ad972", ERR, arangodb::Logger::DUMP)
          << "An error occurred while dumping" << serverLabel(server) << ": "
          << check;

      bool const retry = shouldRetryRequest(response.get(), check);
      if (!retry || --retryCounter == 0) {
        if (retryCounter == 0) {
          LOG_TOPIC("684ee", FATAL, Logger::DUMP) << "Too many network errors.";
        }
        LOG_TOPIC("5cb01", FATAL, Logger::DUMP)
            << "Unrecoverable network/http error: " << check;
        FATAL_ERROR_EXIT();
      }
    } else if (response->getHttpReturnCode() == 204) {
      return nullptr;
    } else if (response->getHttpReturnCode() == 200) {
      return response;
    } else {
      LOG_TOPIC("2668f", FATAL, Logger::DUMP)
          << "Got invalid return code: " << response->getHttpReturnCode() << " "
          << response->getHttpReturnMessage();
      FATAL_ERROR_EXIT();
    }
  }
}

void DumpFeature::ParallelDumpServer::runNetworkThread(
    size_t threadId) noexcept {
  std::unique_ptr<httpclient::SimpleHttpClient> client;
  clientManager.getConnectedClient(client, /* force */ true, false, false, true,
                                   threadId);
  std::uint64_t batchId;
  std::optional<std::uint64_t> lastBatchId;
  while (true) {
    batchId = _batchCounter.fetch_add(1);
    auto response = receiveNextBatch(*client, batchId, lastBatchId);
    if (response == nullptr) {
      break;
    }
    ++stats.totalBatches;
    stats.totalReceived += static_cast<uint64_t>(response->getBody().size());
    auto [stopped, blocked] = queue.push(std::move(response));
    if (stopped) {
      LOG_TOPIC("b3cf8", DEBUG, Logger::DUMP)
          << "network thread stopped by stopped channel";
    }
    if (blocked) {
      countBlocker(kLocalQueue, -1);
    }
    lastBatchId = batchId;
  }
  LOG_TOPIC("ac308", DEBUG, Logger::DUMP)
      << serverLabel(server) << " exhausted";
}

void DumpFeature::ParallelDumpServer::runWriterThread() {
  std::unordered_map<
      std::string,
      std::pair<std::shared_ptr<ManagedDirectory::File>, std::string>>
      filesByShard;

  auto const getDataForShard = [&](std::string const& shardId) {
    if (auto it = filesByShard.find(shardId); it != filesByShard.end()) {
      return it->second;
    } else {
      auto it2 = shards.find(shardId);
      if (it2 == shards.end()) {
        LOG_TOPIC("cd43f", FATAL, Logger::DUMP)
            << "server returned an unexpected shard " << shardId;
        FATAL_ERROR_EXIT();
      }

      std::string collectionName = it2->second.collectionName;
      auto file = fileProvider->getFile(collectionName);
      filesByShard.emplace(shardId, std::make_pair(file, collectionName));
      return std::make_pair(std::move(file), std::move(collectionName));
    }
  };

  while (true) {
    auto [response, blocked] = queue.pop();
    if (response == nullptr) {
      break;
    }
    if (blocked) {
      countBlocker(kLocalQueue, 1);
    }
    // Decode which shard this is from header field
    bool headerExtracted;
    auto shardId =
        response->getHeaderField(StaticStrings::DumpShardId, headerExtracted);
    if (!headerExtracted) {
      LOG_TOPIC("14cbf", FATAL, Logger::DUMP)
          << "Missing header field '" << StaticStrings::DumpShardId << "'";
      FATAL_ERROR_EXIT();
    }

    // update block counts from remote servers
    auto count = [&, &response = response]() -> int64_t {
      bool headerExtracted;
      auto str = response->getHeaderField(StaticStrings::DumpBlockCounts,
                                          headerExtracted);
      if (!headerExtracted) {
        return 0;
      }
      return basics::StringUtils::int64(str);
    }();

    countBlocker(kRemoteQueue, count);

    std::string_view body = {response->getBody().c_str(),
                             response->getBody().size()};

    // transparently uncompress gzip-encoded data
    std::string uncompressed;
    auto header = response->getHeaderField(
        arangodb::StaticStrings::ContentEncoding, headerExtracted);
    if (headerExtracted && header == arangodb::StaticStrings::EncodingGzip) {
      auto res = arangodb::encoding::gzipUncompress(
          reinterpret_cast<uint8_t const*>(body.data()), body.size(),
          uncompressed);
      if (res != TRI_ERROR_NO_ERROR) {
        THROW_ARANGO_EXCEPTION(res);
      }
      body = uncompressed;
    }

    auto [file, collectionName] = getDataForShard(shardId);
    arangodb::Result result = dumpData(stats, maskings, *file, body,
                                       collectionName, options.useVPack);

    LOG_TOPIC("ab681", TRACE, Logger::DUMP)
        << "writing data for shard '" << shardId << "' of collection '"
        << collectionName << "' into file '" << file->path() << "'";

    if (result.fail()) {
      LOG_TOPIC("77881", FATAL, Logger::DUMP)
          << "Failed to write data: " << result;
      FATAL_ERROR_EXIT();
    }
  }
  LOG_TOPIC("18eb0", DEBUG, Logger::DUMP) << "Worker completed";
}

DumpFeature::DumpFileProvider::DumpFileProvider(
    ManagedDirectory& directory,
    std::map<std::string, arangodb::velocypack::Slice>& collectionInfo,
    bool splitFiles, bool useVPack)
    : _splitFiles(splitFiles),
      _useVPack(useVPack),
      _directory(directory),
      _collectionInfo(collectionInfo) {
  if (!_splitFiles) {
    // If we don't split files, i.e. arangorestore compatibility mode, we have
    // to create a file for each collection, even if it is empty. Otherwise,
    // arangorestore complains.
    for (auto const& [name, info] : collectionInfo) {
      if (info.isNone()) {
        // collection name present in dump
        continue;
      }
      std::string hexString(arangodb::rest::SslInterface::sslMD5(name));
      std::string escapedName =
          escapedCollectionName(name, info.get("parameters"));

      std::string filename = absl::StrCat(escapedName, "_", hexString, ".data.",
                                          ::getDatafileSuffix(_useVPack));
      auto file = _directory.writableFile(filename, true /*overwrite*/, 0,
                                          true /*gzipOk*/);
      if (file == nullptr || file->status().fail()) {
        LOG_TOPIC("40543", FATAL, Logger::DUMP)
            << "Failed to open file " << filename
            << " for writing: " << file->status().errorMessage();
        FATAL_ERROR_EXIT();
      }
      auto shared = std::shared_ptr<ManagedDirectory::File>(file.release());
      _filesByCollection.emplace(name, CollectionFiles{0, std::move(shared)});
    }
  }
}

std::shared_ptr<ManagedDirectory::File> DumpFeature::DumpFileProvider::getFile(
    std::string const& name) {
  std::string hexString(arangodb::rest::SslInterface::sslMD5(name));

  std::unique_lock guard(_mutex);

  auto info = _collectionInfo.at(name);

  std::string escapedName = escapedCollectionName(name, info.get("parameters"));

  if (_splitFiles) {
    auto cnt = _filesByCollection[name].count++;
    std::string filename =
        absl::StrCat(escapedName, "_", hexString, ".", cnt, ".data.",
                     ::getDatafileSuffix(_useVPack));
    auto file = _directory.writableFile(filename, true /*overwrite*/, 0,
                                        true /*gzipOk*/);
    if (file == nullptr || file->status().fail()) {
      LOG_TOPIC("43543", FATAL, Logger::DUMP)
          << "Failed to open file " << filename
          << " for writing: " << file->status();
      FATAL_ERROR_EXIT();
    }

    return file;
  } else {
    auto& fileInfo = _filesByCollection[name];
    TRI_ASSERT(fileInfo.file != nullptr);
    return fileInfo.file;
  }
}

}  // namespace arangodb
