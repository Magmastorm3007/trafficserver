/*
  Licensed to the Apache Software Foundation (ASF) under one
  or more contributor license agreements.  See the NOTICE file
  distributed with this work for additional information
  regarding copyright ownership.  The ASF licenses this file
  to you under the Apache License, Version 2.0 (the
  "License"); you may not use this file except in compliance
  with the License.  You may obtain a copy of the License at

  http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.
*/

/**
 * @file plugin.cc
 * @brief traffic server plugin entry points.
 */
#include <sstream>

#include "ts/ts.h" /* ATS API */

#include "ts/remap.h" /* TSRemapInterface, TSRemapStatus, apiInfo */

#include "common.h"
#include "configs.h"
#include "fetch.h"
#include "fetch_policy.h"
#include "headers.h"
#include "evaluate.h"

static const char *
getEventName(TSEvent event)
{
  switch (event) {
  case TS_EVENT_HTTP_CONTINUE:
    return "TS_EVENT_HTTP_CONTINUE";
  case TS_EVENT_HTTP_ERROR:
    return "TS_EVENT_HTTP_ERROR";
  case TS_EVENT_HTTP_READ_REQUEST_HDR:
    return "TS_EVENT_HTTP_READ_REQUEST_HDR";
  case TS_EVENT_HTTP_OS_DNS:
    return "TS_EVENT_HTTP_OS_DNS";
  case TS_EVENT_HTTP_SEND_REQUEST_HDR:
    return "TS_EVENT_HTTP_SEND_REQUEST_HDR";
  case TS_EVENT_HTTP_READ_CACHE_HDR:
    return "TS_EVENT_HTTP_READ_CACHE_HDR";
  case TS_EVENT_HTTP_READ_RESPONSE_HDR:
    return "TS_EVENT_HTTP_READ_RESPONSE_HDR";
  case TS_EVENT_HTTP_SEND_RESPONSE_HDR:
    return "TS_EVENT_HTTP_SEND_RESPONSE_HDR";
  case TS_EVENT_HTTP_REQUEST_TRANSFORM:
    return "TS_EVENT_HTTP_REQUEST_TRANSFORM";
  case TS_EVENT_HTTP_RESPONSE_TRANSFORM:
    return "TS_EVENT_HTTP_RESPONSE_TRANSFORM";
  case TS_EVENT_HTTP_SELECT_ALT:
    return "TS_EVENT_HTTP_SELECT_ALT";
  case TS_EVENT_HTTP_TXN_START:
    return "TS_EVENT_HTTP_TXN_START";
  case TS_EVENT_HTTP_TXN_CLOSE:
    return "TS_EVENT_HTTP_TXN_CLOSE";
  case TS_EVENT_HTTP_SSN_START:
    return "TS_EVENT_HTTP_SSN_START";
  case TS_EVENT_HTTP_SSN_CLOSE:
    return "TS_EVENT_HTTP_SSN_CLOSE";
  case TS_EVENT_HTTP_CACHE_LOOKUP_COMPLETE:
    return "TS_EVENT_HTTP_CACHE_LOOKUP_COMPLETE";
  case TS_EVENT_HTTP_PRE_REMAP:
    return "TS_EVENT_HTTP_PRE_REMAP";
  case TS_EVENT_HTTP_POST_REMAP:
    return "TS_EVENT_HTTP_POST_REMAP";
  default:
    return "UNHANDLED";
  }
  return "UNHANDLED";
}

static const char *
getCacheLookupResultName(TSCacheLookupResult result)
{
  switch (result) {
  case TS_CACHE_LOOKUP_MISS:
    return "TS_CACHE_LOOKUP_MISS";
    break;
  case TS_CACHE_LOOKUP_HIT_STALE:
    return "TS_CACHE_LOOKUP_HIT_STALE";
    break;
  case TS_CACHE_LOOKUP_HIT_FRESH:
    return "TS_CACHE_LOOKUP_HIT_FRESH";
    break;
  case TS_CACHE_LOOKUP_SKIPPED:
    return "TS_CACHE_LOOKUP_SKIPPED";
    break;
  default:
    return "UNKNOWN_CACHE_LOOKUP_EVENT";
    break;
  }
  return "UNKNOWN_CACHE_LOOKUP_EVENT";
}

/**
 * @brief Plugin initialization.
 * @param apiInfo remap interface info pointer
 * @param errBuf error message buffer
 * @param errBufSize error message buffer size
 * @return always TS_SUCCESS.
 */
TSReturnCode
TSRemapInit(TSRemapInterface *apiInfo, char *errBuf, int erroBufSize)
{
  return TS_SUCCESS;
}

/**
 * @brief Plugin instance data.
 */

struct PrefetchInstance {
  PrefetchInstance() = default;
  ;

private:
  PrefetchInstance(PrefetchInstance const &);
  void operator=(BgFetchState const &);

public:
  PrefetchConfig _config;
  BgFetchState *_state = nullptr;
};

/**
 * brief Plugin transaction data.
 */
class PrefetchTxnData
{
public:
  PrefetchTxnData(PrefetchInstance *inst)
    : _inst(inst), _front(true), _firstPass(true), _fetchable(false), _status(TS_HTTP_STATUS_OK)
  {
  }

  bool
  firstPass() const
  {
    return _firstPass;
  }

  bool
  secondPass() const
  {
    return !_firstPass;
  }

  bool
  frontend() const
  {
    return _front;
  }

  bool
  backend() const
  {
    return !_front;
  }

  PrefetchInstance *_inst; /* Pointer to the plugin instance */

  bool _front;     /* front-end vs back-end */
  bool _firstPass; /* first vs second pass */

  /* saves state between hooks */
  String _cachekey;     /* cache key */
  bool _fetchable;      /* saves the result of the attempt to fetch */
  TSHttpStatus _status; /* status to return to the UA */
  String _body;         /* body to return to the UA */
};

/**
 * @brief Expand+evaluate (in place) an expression surrounded with "{" and "}" and uses evaluate() to evaluate the math expression.
 *
 * @param s string containing an expression, i.e. "{3 + 4}"
 * @return void
 */
static void
expand(String &s, const EvalPolicy oflow)
{
  size_t cur = 0;
  while (String::npos != cur) {
    size_t start = s.find_first_of('{', cur);
    size_t stop  = s.find_first_of('}', start);

    if (String::npos != start && String::npos != stop) {
      s.replace(start, stop - start + 1, evaluate(s.substr(start + 1, stop - start - 1), oflow));
      cur = stop + 1;
    } else {
      cur = stop;
    }
  }
}

/**
 * @brief Get the cachekey used for the particular object in this transaction.
 *
 * @param txnp HTTP transaction structure
 * @param reqBuffer request TSMBuffer
 * @param destination string reference to where the result is to be appended.
 * @return true if success or false if failure
 */
bool
appendCacheKey(const TSHttpTxn txnp, const TSMBuffer reqBuffer, String &key)
{
  bool ret      = false;
  TSMLoc keyLoc = TS_NULL_MLOC;
  if (TS_SUCCESS == TSUrlCreate(reqBuffer, &keyLoc)) {
    if (TS_SUCCESS == TSHttpTxnCacheLookupUrlGet(txnp, reqBuffer, keyLoc)) {
      int urlLen = 0;
      char *url  = TSUrlStringGet(reqBuffer, keyLoc, &urlLen);
      if (nullptr != url) {
        key.append(url, urlLen);
        PrefetchDebug("cache key: %s", key.c_str());
        TSfree(static_cast<void *>(url));
        ret = true;
      }
    } else {
      PrefetchDebug("Failure lookup up cache url");
    }
    TSHandleMLocRelease(reqBuffer, TS_NULL_MLOC, keyLoc);
  } else {
    PrefetchDebug("Failure creating url");
  }

  if (!ret) {
    PrefetchError("failed to get cache key");
  }
  return ret;
}

/**
 * @brief Find out if the object was found fresh in cache.
 *
 * This function finely controls if the pre-fetch should be scheduled or not.
 * @param txnp HTTP transaction structure
 * @return true - hit fresh, false - miss/stale/skipped or error
 */
static bool
foundFresh(TSHttpTxn txnp)
{
  bool fresh = false;
  int lookupStatus;
  if (TS_SUCCESS == TSHttpTxnCacheLookupStatusGet(txnp, &lookupStatus)) {
    PrefetchDebug("lookup status: %s", getCacheLookupResultName((TSCacheLookupResult)lookupStatus));
    if (TS_CACHE_LOOKUP_HIT_FRESH == lookupStatus) {
      fresh = true;
    }
  } else {
    /* Failed to get the lookup status,  likely a previous plugin already prepared the client response w/o a cache lookup,
     * we don't really know if the cache has a fresh object, so just don't trigger pre-fetch  */
    PrefetchDebug("failed to check cache-ability");
  }
  return fresh;
}

/**
 * @brief Check if the response from origin for N-th object is success (200 and 206)
 *
 * and only then schedule a pre-fetch for the next
 *
 * @param txnp HTTP transaction structure
 * @return true - yes, false - no
 */
bool
isResponseGood(TSHttpTxn txnp)
{
  bool good = false;
  TSMBuffer respBuffer;
  TSMLoc respHdrLoc;
  if (TS_SUCCESS == TSHttpTxnServerRespGet(txnp, &respBuffer, &respHdrLoc)) {
    TSHttpStatus status = TSHttpHdrStatusGet(respBuffer, respHdrLoc);
    PrefetchDebug("origin response code: %d", status);
    if (TS_HTTP_STATUS_PARTIAL_CONTENT == status || TS_HTTP_STATUS_OK == status) {
      good = true;
    }
    /* Release the response MLoc */
    TSHandleMLocRelease(respBuffer, TS_NULL_MLOC, respHdrLoc);
  } else {
    /* Failed to get the origin response, possible cause could be a origin connection problems or timeouts or
     * a previous plugin could have already prepared the client response w/o going to origin server */
    PrefetchDebug("failed to get origin response");
  }
  return good;
}

/**
 * @brief get the pristin URL path
 *
 * @param txnp HTTP transaction structure
 * @return pristine URL path
 */
static String
getPristineUrlPath(TSHttpTxn txnp)
{
  String pristinePath;
  TSMLoc pristineUrlLoc;
  TSMBuffer reqBuffer;

  if (TS_SUCCESS == TSHttpTxnPristineUrlGet(txnp, &reqBuffer, &pristineUrlLoc)) {
    int pathLen      = 0;
    const char *path = TSUrlPathGet(reqBuffer, pristineUrlLoc, &pathLen);
    if (nullptr != path) {
      PrefetchDebug("path: '%.*s'", pathLen, path);
      pristinePath.assign(path, pathLen);
    } else {
      PrefetchError("failed to get pristine URL path");
    }
    TSHandleMLocRelease(reqBuffer, TS_NULL_MLOC, pristineUrlLoc);
  } else {
    PrefetchError("failed to get pristine URL");
  }
  return pristinePath;
}

/**
 * @brief get the pristin URL querystring
 *
 * @param txnp HTTP transaction structure
 * @return pristine URL querystring
 */
static String
getPristineUrlQuery(TSHttpTxn txnp)
{
  String pristineQuery;
  TSMLoc pristineUrlLoc;
  TSMBuffer reqBuffer;

  if (TS_SUCCESS == TSHttpTxnPristineUrlGet(txnp, &reqBuffer, &pristineUrlLoc)) {
    int queryLen      = 0;
    const char *query = TSUrlHttpQueryGet(reqBuffer, pristineUrlLoc, &queryLen);
    if (nullptr != query) {
      PrefetchDebug("query: '%.*s'", queryLen, query);
      pristineQuery.assign(query, queryLen);
    }
    TSHandleMLocRelease(reqBuffer, TS_NULL_MLOC, pristineUrlLoc);
  } else {
    PrefetchError("failed to get pristine URL");
  }
  return pristineQuery;
}

static constexpr StringView CmcdHeader{"Cmcd-Request"};
static constexpr StringView CmcdNorFieldPrefix{"nor="};
static constexpr StringView CmcdNrrFieldPrefix{"nrr="};

/**
 * @brief Look for and return the nor field of any Cmcd-Request header
 *
 * @param txnp HTTP transaction structure
 * @param buffer request TSMBuffer
 * @param hdrloc request TSMLoc
 * @return unquoted relative cmcd path from the nor field
 *
 * If the 'nrr' field is encountered the 'nor' field will be ignored.
 *
 * sample header:
 * Cmcd-Request: mtp=103600,bl=153500,nor="14_176.mp4a"
 */
static String
getCmcdNor(const TSMBuffer buffer, const TSMLoc hdrloc)
{
  String relpath;
  bool hasnrr = false; // don't prefetch if range request

  const TSMLoc cmcdloc = TSMimeHdrFieldFind(buffer, hdrloc, CmcdHeader.data(), CmcdHeader.length());
  if (TS_NULL_MLOC != cmcdloc) {
    // iterate through the fields
    const int cnt = TSMimeHdrFieldValuesCount(buffer, hdrloc, cmcdloc);
    for (int ind = 0; ind < cnt; ++ind) {
      int flen               = 0;
      const char *const fval = TSMimeHdrFieldValueStringGet(buffer, hdrloc, cmcdloc, ind, &flen);

      StringView fv(fval, flen);
      PrefetchDebug("cmcd-request field: '%.*s'", (int)fv.length(), fv.data());
      if (0 == fv.compare(0, CmcdNrrFieldPrefix.length(), CmcdNrrFieldPrefix)) {
        PrefetchDebug("cmcd-request nrr field encountered, skipping prefetch!");
        hasnrr = true;
        break;
      }

      if (0 == fv.compare(0, CmcdNorFieldPrefix.length(), CmcdNorFieldPrefix)) {
        fv.remove_prefix(CmcdNorFieldPrefix.length());
        if (fv.front() == '"') {
          fv.remove_prefix(1);
        }
        if (fv.back() == '"') {
          fv.remove_suffix(1);
        }

        PrefetchDebug("Extracted nor field: '%.*s'", (int)fv.length(), fv.data());

        // Undo any percent encoding
        char buf[8192];
        size_t blen = sizeof(buf);
        if (TS_SUCCESS == TSStringPercentDecode(fv.data(), fv.length(), buf, blen, &blen)) {
          relpath.assign(buf, blen);
        } else {
          PrefetchDebug("Error percent decoding nor field: '%.*s'", (int)fv.length(), fv.data());
        }
      }
    }
    TSHandleMLocRelease(buffer, hdrloc, cmcdloc);
  } else {
    PrefetchDebug("No Cmcd-Request header found");
  }

  // don't prefetch if range request
  if (hasnrr) {
    relpath.clear();
  }

  return relpath;
}

/**
 * @brief short-cut to set the response .
 */
TSEvent
shortcutResponse(PrefetchTxnData *data, TSHttpStatus status, const char *body, TSEvent event)
{
  data->_status = status;
  data->_body.assign(body);
  return event;
}

/**
 * @brief Checks if we are still supposed to schedule a background fetch based on whether the object is in the cache.
 * It is 'fetchable' only if not a fresh hit.
 *
 * @param txnp HTTP transaction structure
 * @param data transaction data
 * @return true if fetchable and false if not.
 */
static bool
isFetchable(TSHttpTxn txnp, PrefetchTxnData *data)
{
  bool fetchable      = false;
  BgFetchState *state = data->_inst->_state;
  if (!foundFresh(txnp)) {
    /* Schedule fetch only if not in cache */
    PrefetchDebug("object to be fetched");
    fetchable = true;
  } else {
    PrefetchDebug("object already in cache or to be skipped");
    state->incrementMetric(FETCH_ALREADY_CACHED);
    state->incrementMetric(FETCH_TOTAL);
  }
  return fetchable;
}

/**
 * @brief Find out if the current response to trigger a background prefetch.
 *
 * Pre-fetch only on HTTP codes 200 and 206 or object found in cache (previous good response)
 *
 * @param txnp HTTP transaction structure
 * @return true - trigger prefetch, false - don't trigger.
 */
static bool
respToTriggerPrefetch(TSHttpTxn txnp)
{
  bool trigger = false;
  if (foundFresh(txnp)) {
    /* If found in cache and fresh trigger next (same as good response from origin) */
    PrefetchDebug("trigger background fetch (cached)");
    trigger = true;
  } else if (isResponseGood(txnp)) {
    /* Trigger all necessary background fetches based on the next path pattern */
    PrefetchDebug("trigger background fetch (good origin response)");
    trigger = true;
  } else {
    PrefetchDebug("don't trigger background fetch");
  }
  return trigger;
}

/**
 * @brief Callback function that handles necessary foreground / background fetch operations.
 *
 * @param contp continuation associated with this function.
 * @param event corresponding event triggered at different hooks.
 * @param edata HTTP transaction structures.
 * @return always 0
 */
int
contHandleFetch(const TSCont contp, TSEvent event, void *edata)
{
  PrefetchTxnData *data  = static_cast<PrefetchTxnData *>(TSContDataGet(contp));
  TSHttpTxn txnp         = static_cast<TSHttpTxn>(edata);
  PrefetchConfig &config = data->_inst->_config;
  BgFetchState *state    = data->_inst->_state;
  TSMBuffer reqBuffer    = nullptr;
  TSMLoc reqHdrLoc       = TS_NULL_MLOC;

  PrefetchDebug("event: %s (%d)", getEventName(event), event);

  TSEvent retEvent = TS_EVENT_HTTP_CONTINUE;

  // For these cases we need to access the client request
  switch (event) {
  case TS_EVENT_HTTP_POST_REMAP:
  case TS_EVENT_HTTP_CACHE_LOOKUP_COMPLETE:
  case TS_EVENT_HTTP_SEND_RESPONSE_HDR:
    if (TS_SUCCESS != TSHttpTxnClientReqGet(txnp, &reqBuffer, &reqHdrLoc)) {
      PrefetchError("failed to get client request");
      TSHttpTxnReenable(txnp, TS_EVENT_HTTP_ERROR);
      return 0;
    }
    break;
  default:
    break;
  }

  switch (event) {
  case TS_EVENT_HTTP_POST_REMAP: {
    /* Use the cache key since this has better lookup behavior when using plugins like the cachekey plugin,
     * for example multiple URIs can match a single cache key */
    if (data->frontend() && data->secondPass()) {
      /* Create a separate cache key name space to be used only for front-end and second-pass fetch policy checks. */
      data->_cachekey.assign("/prefetch");
    }
    if (!appendCacheKey(txnp, reqBuffer, data->_cachekey)) {
      PrefetchError("failed to get the cache key");
      TSHttpTxnReenable(txnp, TS_EVENT_HTTP_ERROR);
      return 0;
    }

    if (data->frontend()) {
      /* front-end instance */
      if (data->firstPass()) {
        /* first-pass */
        if (!config.isExactMatch()) {
          data->_fetchable = state->acquire(data->_cachekey);
          PrefetchDebug("request is %s fetchable", data->_fetchable ? " " : " not ");
        }
      }
    }
  } break;

  case TS_EVENT_HTTP_CACHE_LOOKUP_COMPLETE: {
    if (data->frontend()) {
      /* front-end instance */
      if (data->secondPass()) {
        /* second-pass */
        data->_fetchable = state->acquire(data->_cachekey);
        data->_fetchable = data->_fetchable && state->uniqueAcquire(data->_cachekey);
        PrefetchDebug("request is %s fetchable", data->_fetchable ? " " : " not ");

        if (isFetchable(txnp, data)) {
          if (!data->_fetchable) {
            /* Cancel the requested fetch */
            retEvent = shortcutResponse(data, TS_HTTP_STATUS_ALREADY_REPORTED, "fetch not scheduled\n", TS_EVENT_HTTP_ERROR);
          } else {
            /* Fetch */
          }
        } else {
          retEvent = shortcutResponse(data, TS_HTTP_STATUS_ALREADY_REPORTED, "fetch not scheduled\n", TS_EVENT_HTTP_ERROR);
        }
      }
    } else {
      /* back-end instance */
      if (data->firstPass()) {
        if (isFetchable(txnp, data)) {
          if (BgFetch::schedule(state, config, /* askPermission */ true, reqBuffer, reqHdrLoc, txnp, nullptr, 0, data->_cachekey)) {
            retEvent = shortcutResponse(data, TS_HTTP_STATUS_OK, "fetch scheduled\n", TS_EVENT_HTTP_ERROR);
          } else {
            retEvent = shortcutResponse(data, TS_HTTP_STATUS_ALREADY_REPORTED, "fetch not scheduled\n", TS_EVENT_HTTP_ERROR);
          }
        } else {
          retEvent = shortcutResponse(data, TS_HTTP_STATUS_ALREADY_REPORTED, "fetch not scheduled\n", TS_EVENT_HTTP_ERROR);
        }
      }
    }
  } break;

  case TS_EVENT_HTTP_SEND_RESPONSE_HDR: {
    if (data->frontend()) {
      /* front-end instance */

      const String currentPath  = getPristineUrlPath(txnp);
      const String currentQuery = getPristineUrlQuery(txnp);
      bool hasValidQuery        = false;

      // If there is a --fetch-query defined in the config, and that string is found in the querystring, assume it is
      // valid, and prefer the --fetch-query over the --fetch-path-pattern(s).
      if (!config.getQueryKeyName().empty() && currentQuery.find(config.getQueryKeyName()) != String::npos) {
        PrefetchDebug("Setting hasValidQuery to true");
        hasValidQuery = true;
      }

      if (data->firstPass() && data->_fetchable && respToTriggerPrefetch(txnp)) {
        // If configured to handle Cmcd-Request nor field.
        // This still allows other prefetch configurations to work.
        if (config.isCmcdNor()) {
          PrefetchDebug("Considering cmcd nor request");

          TSAssert(nullptr != reqBuffer);
          TSAssert(TS_NULL_MLOC != reqHdrLoc);

          const String relpath = getCmcdNor(reqBuffer, reqHdrLoc);
          if (!relpath.empty()) {
            PrefetchDebug("Current path: '%s'", currentPath.c_str());
            PrefetchDebug("Parsed cmcd nor relpath: '%s'", relpath.c_str());

            const String::size_type lsi = currentPath.find_last_of("/");
            const String nextPath       = currentPath.substr(0, lsi + 1) + relpath;

            PrefetchDebug("Next cmcd nor path: '%s'", nextPath.c_str());

            constexpr bool askPermission = false;
            constexpr bool removeQuery   = true;
            BgFetch::schedule(state, config, askPermission, reqBuffer, reqHdrLoc, txnp, nextPath.c_str(), nextPath.length(),
                              data->_cachekey, removeQuery);
          }
        }

        if (!hasValidQuery && !config.getNextPath().empty()) {
          /* Trigger all necessary background fetches based on the next path pattern */

          if (!currentPath.empty()) {
            const unsigned total = config.getFetchCount();
            String workingPath   = currentPath;
            for (unsigned i = 0; i < total; ++i) {
              PrefetchDebug("generating prefetch request %d/%d", i + 1, total);
              String expandedPath;

              if (config.getNextPath().replace(workingPath, expandedPath)) {
                PrefetchDebug("replaced: %s", expandedPath.c_str());
                expand(expandedPath, config.getFetchOverflow());
                PrefetchDebug("expanded: %s cachekey: %s", expandedPath.c_str(), data->_cachekey.c_str());

                BgFetch::schedule(state, config, /* askPermission */ false, reqBuffer, reqHdrLoc, txnp, expandedPath.c_str(),
                                  expandedPath.length(), data->_cachekey);
              } else {
                /* We should be here only if the pattern replacement fails (match already checked) */
                PrefetchError("failed to process the pattern");

                /* If the first or any matches fails there must be something wrong so don't continue */
                break;
              }
              workingPath.assign(expandedPath);
            }
          } else {
            PrefetchDebug("failed to get current path");
          }
        } else if (hasValidQuery) {
          /* Trigger all necessary background fetches based on the query string(s) */

          PrefetchDebug("currentQuery: %s", currentQuery.c_str());
          const size_t lastSlashIndex = currentPath.find_last_of("/");
          const size_t keyLen         = config.getQueryKeyName().size();
          unsigned done               = 1;
          std::istringstream cStringStream(currentQuery);
          String param;

          while (getline(cStringStream, param, '&')) {
            if (param.find(config.getQueryKeyName()) != 0) {
              continue;
            }
            if (config.getFetchCount() < done++) {
              break;
            }
            String nextFile = param.substr(keyLen + 1); // +1 for the '='
            String nextPath = currentPath.substr(0, lastSlashIndex + 1) + nextFile;

            PrefetchDebug("nextPath %s, cacheKey %s", nextPath.c_str(), data->_cachekey.c_str());
            BgFetch::schedule(state, config, /* askPermission */ false, reqBuffer, reqHdrLoc, txnp, nextPath.c_str(),
                              nextPath.length(), data->_cachekey);
          }
        }
      }
    }

    if ((data->backend() && data->firstPass()) || (data->frontend() && data->secondPass() && !data->_body.empty())) {
      TSMBuffer bufp;
      TSMLoc hdrLoc;

      if (TS_SUCCESS == TSHttpTxnClientRespGet(txnp, &bufp, &hdrLoc)) {
        const char *reason = TSHttpHdrReasonLookup(data->_status);
        int reasonLen      = strlen(reason);
        TSHttpHdrStatusSet(bufp, hdrLoc, data->_status);
        TSHttpHdrReasonSet(bufp, hdrLoc, reason, reasonLen);
        PrefetchDebug("set response: %d %.*s '%s'", data->_status, reasonLen, reason, data->_body.c_str());

        size_t bufsize = data->_body.length() + 1;
        char *buf      = static_cast<char *>(TSmalloc(bufsize));
        memcpy(buf, data->_body.c_str(), bufsize);
        TSHttpTxnErrorBodySet(txnp, buf, strlen(buf), nullptr);

        setHeader(bufp, hdrLoc, TS_MIME_FIELD_CACHE_CONTROL, TS_MIME_LEN_CACHE_CONTROL, TS_HTTP_VALUE_NO_STORE,
                  TS_HTTP_LEN_NO_STORE);

        TSHandleMLocRelease(bufp, TS_NULL_MLOC, hdrLoc);
      } else {
        PrefetchError("failed to retrieve client response header");
      }
    }
  } break;

  case TS_EVENT_HTTP_TXN_CLOSE: {
    if (data->_fetchable) {
      if (data->frontend()) {
        /* front-end */
        if (data->firstPass()) {
          /* first-pass */
          if (!config.isExactMatch()) {
            state->release(data->_cachekey);
          }
        } else {
          /* second-pass */
          state->uniqueRelease(data->_cachekey);
          state->release(data->_cachekey);
        }
      }
    }

    /* Destroy the txn continuation and its data */
    delete data;
    TSContDestroy(contp);
  } break;

  default: {
    PrefetchError("unhandled event: %s(%d)", getEventName(event), event);
  } break;
  }

  /* Release the request MLoc */
  if (event == TS_EVENT_HTTP_POST_REMAP || event == TS_EVENT_HTTP_CACHE_LOOKUP_COMPLETE ||
      event == TS_EVENT_HTTP_SEND_RESPONSE_HDR) {
    TSHandleMLocRelease(reqBuffer, TS_NULL_MLOC, reqHdrLoc);
  }

  /* Reenable and continue with the state machine. */
  TSHttpTxnReenable(txnp, retEvent);
  return 0;
}

/**
 * @brief Plugin new instance entry point.
 *
 * Processes the configuration and initializes the plugin instance.
 * @param argc plugin arguments number
 * @param argv plugin arguments
 * @param instance new plugin instance pointer (initialized in this function)
 * @param errBuf error message buffer
 * @param errBufSize error message buffer size
 * @return TS_SUCCES if success or TS_ERROR if failure
 */
TSReturnCode
TSRemapNewInstance(int argc, char *argv[], void **instance, char *errBuf, int errBufSize)
{
  bool failed = true;

  PrefetchInstance *inst = new PrefetchInstance();
  if (nullptr != inst) {
    if (inst->_config.init(argc, argv)) {
      inst->_state = BgFetchStates::get()->getStateByName(inst->_config.getNameSpace());
      if (nullptr != inst->_state) {
        failed = !inst->_state->init(inst->_config);
      }
    }
  }

  if (failed) {
    PrefetchError("failed to initialize the plugin");
    delete inst;
    *instance = nullptr;
    return TS_ERROR;
  }

  *instance = inst;
  return TS_SUCCESS;
}

/**
 * @brief Plugin instance deletion clean-up entry point.
 * @param plugin instance pointer.
 */
void
TSRemapDeleteInstance(void *instance)
{
  PrefetchInstance *inst = static_cast<PrefetchInstance *>(instance);
  delete inst;
}

/**
 * @brief Organizes the background fetch by registering necessary hooks, by identifying front-end vs back-end, first vs second
 * pass.
 *
 * Remap is never done, continue with next in chain.
 * @param instance plugin instance pointer
 * @param txnp transaction handle
 * @param rri remap request info pointer
 * @return always TSREMAP_NO_REMAP
 */
TSRemapStatus
TSRemapDoRemap(void *instance, TSHttpTxn txnp, TSRemapRequestInfo *rri)
{
  PrefetchInstance *inst = static_cast<PrefetchInstance *>(instance);

  if (nullptr != inst) {
    PrefetchConfig &config = inst->_config;

    int methodLen        = 0;
    const char *method   = TSHttpHdrMethodGet(rri->requestBufp, rri->requestHdrp, &methodLen);
    const String &header = config.getApiHeader();
    if (nullptr != method && methodLen == TS_HTTP_LEN_GET && 0 == memcmp(TS_HTTP_METHOD_GET, method, TS_HTTP_LEN_GET)) {
      bool front     = config.isFront();
      bool firstPass = false;
      if (headerExist(rri->requestBufp, rri->requestHdrp, header.c_str(), header.length())) {
        PrefetchDebug("%s: found %.*s", front ? "front-end" : "back-end", (int)header.length(), header.c_str());
        /* On front-end: presence of header means second-pass, on back-end means first-pass. */
        firstPass = !front;
      } else {
        /* On front-end: lack of header means first-pass, on back-end means second-pass. */
        firstPass = front;
      }

      /* Make sure we handle only URLs that match the path pattern on the front-end + first-pass, cancel otherwise */
      bool handleFetch = true;
      if (front && firstPass && !config.isCmcdNor()) {
        /* Front-end plug-in instance + first pass. */
        if (config.getNextPath().empty()) {
          /* No next path pattern specified then pass this request untouched. */
          PrefetchDebug("next object pattern not specified, skip");
          handleFetch = false;
        } else {
          /* Next path pattern specified hence try to match. */
          String pristinePath = getPristineUrlPath(txnp);
          if (!pristinePath.empty()) {
            if (config.getNextPath().match(pristinePath)) {
              /* Matched - handle the request */
              PrefetchDebug("matched next object pattern");
              inst->_state->incrementMetric(FETCH_MATCH_YES);
            } else {
              /* Next path pattern specified but did not match. */
              PrefetchDebug("failed to match next object pattern, skip");
              inst->_state->incrementMetric(FETCH_MATCH_NO);
              handleFetch = false;
            }
          } else {
            PrefetchDebug("failed to get path to (pre)match");
          }

          String queryKey = config.getQueryKeyName();
          if (!queryKey.empty()) {
            PrefetchDebug("handling for query-key: %s", queryKey.c_str());
            handleFetch = true;
          }
        }
      }

      if (handleFetch) {
        PrefetchTxnData *data = new PrefetchTxnData(inst);
        if (nullptr != data) {
          data->_front     = front;
          data->_firstPass = firstPass;

          TSCont cont = TSContCreate(contHandleFetch, TSMutexCreate());
          TSContDataSet(cont, static_cast<void *>(data));

          TSHttpTxnHookAdd(txnp, TS_HTTP_POST_REMAP_HOOK, cont);
          TSHttpTxnHookAdd(txnp, TS_HTTP_CACHE_LOOKUP_COMPLETE_HOOK, cont);
          TSHttpTxnHookAdd(txnp, TS_HTTP_SEND_RESPONSE_HDR_HOOK, cont);
          TSHttpTxnHookAdd(txnp, TS_HTTP_TXN_CLOSE_HOOK, cont);
        } else {
          PrefetchError("failed to allocate transaction data object");
        }
      }
    } else {
      PrefetchDebug("not a GET method (%.*s), skipping", methodLen, method);
    }
  } else {
    PrefetchError("could not get prefetch instance");
  }

  return TSREMAP_NO_REMAP;
}
