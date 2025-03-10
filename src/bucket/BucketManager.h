#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "bucket/Bucket.h"
#include "util/NonCopyable.h"
#include "util/types.h"
#include <future>
#include <map>
#include <memory>
#include <optional>
#include <set>

#include "medida/timer_context.h"

namespace medida
{
class Meter;
}

namespace stellar
{

class AbstractLedgerTxn;
class Application;
class BasicWork;
class BucketList;
class BucketSnapshotManager;
class Config;
class SearchableBucketListSnapshot;
class TmpDirManager;
struct HistoryArchiveState;
struct InflationWinner;
struct LedgerHeader;
struct MergeKey;

// A fine-grained merge-operation-counter structure for tracking various
// events during merges. These are not medida counters because we do not
// want or need to publish this level of granularity outside of testing, and
// we do want merges to run as quickly as possible.
struct MergeCounters
{
    uint64_t mPreInitEntryProtocolMerges{0};
    uint64_t mPostInitEntryProtocolMerges{0};

    uint64_t mRunningMergeReattachments{0};
    uint64_t mFinishedMergeReattachments{0};

    uint64_t mPreShadowRemovalProtocolMerges{0};
    uint64_t mPostShadowRemovalProtocolMerges{0};

    uint64_t mNewMetaEntries{0};
    uint64_t mNewInitEntries{0};
    uint64_t mNewLiveEntries{0};
    uint64_t mNewDeadEntries{0};
    uint64_t mOldMetaEntries{0};
    uint64_t mOldInitEntries{0};
    uint64_t mOldLiveEntries{0};
    uint64_t mOldDeadEntries{0};

    uint64_t mOldEntriesDefaultAccepted{0};
    uint64_t mNewEntriesDefaultAccepted{0};
    uint64_t mNewInitEntriesMergedWithOldDead{0};
    uint64_t mOldInitEntriesMergedWithNewLive{0};
    uint64_t mOldInitEntriesMergedWithNewDead{0};
    uint64_t mNewEntriesMergedWithOldNeitherInit{0};

    uint64_t mShadowScanSteps{0};
    uint64_t mMetaEntryShadowElisions{0};
    uint64_t mLiveEntryShadowElisions{0};
    uint64_t mInitEntryShadowElisions{0};
    uint64_t mDeadEntryShadowElisions{0};

    uint64_t mOutputIteratorTombstoneElisions{0};
    uint64_t mOutputIteratorBufferUpdates{0};
    uint64_t mOutputIteratorActualWrites{0};
    MergeCounters& operator+=(MergeCounters const& delta);
    bool operator==(MergeCounters const& other) const;
};

// Stores key that is eligible for eviction and the position of the eviction
// iterator as if that key was the last entry evicted
struct EvictionResultEntry
{
    LedgerKey key;
    EvictionIterator iter;
    uint32_t liveUntilLedger;

    EvictionResultEntry(LedgerKey const& key, EvictionIterator const& iter,
                        uint32_t liveUntilLedger)
        : key(key), iter(iter), liveUntilLedger(liveUntilLedger)
    {
    }
};

struct EvictionResult
{
    // List of keys eligible for eviction in the order in which they occur in
    // the bucket
    std::list<EvictionResultEntry> eligibleKeys{};

    // Eviction iterator at the end of the scan region
    EvictionIterator endOfRegionIterator;

    // LedgerSeq which this scan is based on
    uint32_t initialLedger{};

    // State archival settings that this scan is based on
    StateArchivalSettings initialSas;

    EvictionResult(StateArchivalSettings const& sas) : initialSas(sas)
    {
    }

    // Returns true if this is a valid archival scan for the current ledger
    // and archival settings. This is necessary because we start the scan
    // for ledger N immediately after N - 1 closes. However, ledger N may
    // contain a network upgrade changing eviction scan settings. Legacy SQL
    // scans will run based on the changes that occurred during ledger N,
    // meaning the scan we started at ledger N - 1 is invalid since it was based
    // off of older settings.
    bool isValid(uint32_t currLedger,
                 StateArchivalSettings const& currSas) const;
};

struct EvictionCounters
{
    medida::Counter& entriesEvicted;
    medida::Counter& bytesScannedForEviction;
    medida::Counter& incompleteBucketScan;
    medida::Counter& evictionCyclePeriod;
    medida::Counter& averageEvictedEntryAge;

    EvictionCounters(Application& app);
};

class EvictionStatistics
{
  private:
    std::mutex mLock{};

    // Only record metrics if we've seen a complete cycle to avoid noise
    bool mCompleteCycle{false};
    uint64_t mEvictedEntriesAgeSum{};
    uint64_t mNumEntriesEvicted{};
    uint32_t mEvictionCycleStartLedger{};

  public:
    // Evicted entry "age" is the delta between its liveUntilLedger and the
    // ledger when the entry is actually evicted
    void recordEvictedEntry(uint64_t age);

    void submitMetricsAndRestartCycle(uint32_t currLedgerSeq,
                                      EvictionCounters& counters);
};

/**
 * BucketManager is responsible for maintaining a collection of Buckets of
 * ledger entries (each sorted, de-duplicated and identified by hash) and,
 * primarily, for holding the BucketList: the distinguished, ordered collection
 * of buckets that are arranged in such a way as to efficiently provide a single
 * canonical hash for the state of all the entries in the ledger.
 *
 * Not every bucket is present in the BucketList at every instant; buckets
 * live in a few transient states while being merged, upload or downloaded
 * from history archives.
 *
 * Every bucket corresponds to a file on disk and the BucketManager owns a
 * directory in which the buckets it's responsible for reside. It locks this
 * directory exclusively while the process is running; only one BucketManager
 * should be attached to a single directory at a time.
 *
 * Buckets can be created outside the BucketManager's directory -- for example
 * in temporary directories -- and then "adopted" by the BucketManager, moved
 * into its directory and managed by it.
 */

class BucketManager : NonMovableOrCopyable
{

  public:
    static std::unique_ptr<BucketManager> create(Application&);

    virtual ~BucketManager()
    {
    }
    virtual void initialize() = 0;
    virtual void dropAll() = 0;
    virtual std::string bucketIndexFilename(Hash const& hash) const = 0;
    virtual std::string const& getTmpDir() = 0;
    virtual TmpDirManager& getTmpDirManager() = 0;
    virtual std::string const& getBucketDir() const = 0;
    virtual BucketList& getBucketList() = 0;
    virtual BucketSnapshotManager& getBucketSnapshotManager() const = 0;
    virtual bool renameBucketDirFile(std::filesystem::path const& src,
                                     std::filesystem::path const& dst) = 0;

    virtual medida::Timer& getMergeTimer() = 0;

    // Reading and writing the merge counters is done in bulk, and takes a lock
    // briefly; this can be done from any thread.
    virtual MergeCounters readMergeCounters() = 0;
    virtual void incrMergeCounters(MergeCounters const& delta) = 0;

    // Get a reference to a persistent bucket (in the BucketManager's bucket
    // directory), from the BucketManager's shared bucket-set.
    //
    // Concretely: if `hash` names an existing bucket -- either in-memory or on
    // disk -- delete `filename` and return an object for the existing bucket;
    // otherwise move `filename` to the bucket directory, stored under `hash`,
    // and return a new bucket pointing to that.
    //
    // This method is mostly-threadsafe -- assuming you don't destruct the
    // BucketManager mid-call -- and is intended to be called from both main and
    // worker threads. Very carefully.
    virtual std::shared_ptr<Bucket>
    adoptFileAsBucket(std::string const& filename, uint256 const& hash,
                      MergeKey* mergeKey,
                      std::unique_ptr<BucketIndex const> index) = 0;

    // Companion method to `adoptFileAsBucket` also called from the
    // `BucketOutputIterator::getBucket` merge-completion path. This method
    // however should be called when the output bucket is _empty_ and thereby
    // doesn't correspond to a file on disk; the method forgets about the
    // `FutureBucket` associated with the in-progress merge, allowing the merge
    // inputs to be GC'ed.
    virtual void noteEmptyMergeOutput(MergeKey const& mergeKey) = 0;

    // Returns a bucket by hash if it exists and is currently managed by the
    // bucket list.
    virtual std::shared_ptr<Bucket> getBucketIfExists(uint256 const& hash) = 0;

    // Return a bucket by hash if we have it, else return nullptr.
    virtual std::shared_ptr<Bucket> getBucketByHash(uint256 const& hash) = 0;

    // Get a reference to a merge-future that's either running (or finished
    // somewhat recently) from either a map of the std::shared_futures doing the
    // merges and/or a set of records mapping merge inputs to outputs and the
    // set of outputs held in the BucketManager. Returns an invalid future if no
    // such future can be found or synthesized.
    virtual std::shared_future<std::shared_ptr<Bucket>>
    getMergeFuture(MergeKey const& key) = 0;

    // Add a reference to a merge _in progress_ (not yet adopted as a file) to
    // the BucketManager's internal map of std::shared_futures doing merges.
    // There is no corresponding entry-removal API: the std::shared_future will
    // be removed from the map when the merge completes and the output file is
    // adopted.
    virtual void
    putMergeFuture(MergeKey const& key,
                   std::shared_future<std::shared_ptr<Bucket>>) = 0;

#ifdef BUILD_TESTS
    // Drop all references to merge futures in progress.
    virtual void clearMergeFuturesForTesting() = 0;
#endif

    // Forget any buckets not referenced by the current BucketList. This will
    // not immediately cause the buckets to delete themselves, if someone else
    // is using them via a shared_ptr<>, but the BucketManager will no longer
    // independently keep them alive.
    virtual void forgetUnreferencedBuckets() = 0;

    // Feed a new batch of entries to the bucket list. This interface expects to
    // be given separate init (created) and live (updated) entry vectors. The
    // `header` value should be taken from the ledger at which this batch is
    // being added.
    virtual void addBatch(Application& app, LedgerHeader header,
                          std::vector<LedgerEntry> const& initEntries,
                          std::vector<LedgerEntry> const& liveEntries,
                          std::vector<LedgerKey> const& deadEntries) = 0;

    // Update the given LedgerHeader's bucketListHash to reflect the current
    // state of the bucket list.
    virtual void snapshotLedger(LedgerHeader& currentHeader) = 0;

    // Sets index for bucket b if b is not already indexed and if BucketManager
    // is not shutting down. In most cases, there should only be a single index
    // for each bucket. However, during startup there are race conditions where
    // a bucket may be indexed twice. If there is an index race, set index with
    // this function, otherwise use Bucket::setIndex().
    virtual void maybeSetIndex(std::shared_ptr<Bucket> b,
                               std::unique_ptr<BucketIndex const>&& index) = 0;

    // Scans BucketList for non-live entries to evict starting at the entry
    // pointed to by EvictionIterator. Scans until `maxEntriesToEvict` entries
    // have been evicted or maxEvictionScanSize bytes have been scanned.
    virtual void scanForEvictionLegacy(AbstractLedgerTxn& ltx,
                                       uint32_t ledgerSeq) = 0;

    virtual void startBackgroundEvictionScan(uint32_t ledgerSeq) = 0;
    virtual void
    resolveBackgroundEvictionScan(AbstractLedgerTxn& ltx, uint32_t ledgerSeq,
                                  LedgerKeySet const& modifiedKeys) = 0;

    virtual medida::Meter& getBloomMissMeter() const = 0;
    virtual medida::Meter& getBloomLookupMeter() const = 0;

#ifdef BUILD_TESTS
    // Install a fake/assumed ledger version and bucket list hash to use in next
    // call to addBatch and snapshotLedger. This interface exists only for
    // testing in a specific type of history replay.
    virtual void setNextCloseVersionAndHashForTesting(uint32_t protocolVers,
                                                      uint256 const& hash) = 0;

    // Return the set of buckets in the current `getBucketDir()` directory.
    // This interface exists only for checking that the BucketDir isn't
    // leaking buckets, in tests.
    virtual std::set<Hash> getBucketHashesInBucketDirForTesting() const = 0;

    virtual medida::Counter& getEntriesEvictedCounter() const = 0;
#endif

    // Return the set of buckets referenced by the BucketList
    virtual std::set<Hash> getBucketListReferencedBuckets() const = 0;

    // Return the set of buckets referenced by the BucketList, LCL HAS,
    // and publish queue.
    virtual std::set<Hash> getAllReferencedBuckets() const = 0;

    // Check for missing bucket files that would prevent `assumeState` from
    // succeeding
    virtual std::vector<std::string>
    checkForMissingBucketsFiles(HistoryArchiveState const& has) = 0;

    // Assume state from `has` in BucketList: find and attach all buckets in
    // `has`, set current BL.
    virtual void assumeState(HistoryArchiveState const& has,
                             uint32_t maxProtocolVersion,
                             bool restartMerges) = 0;

    virtual void shutdown() = 0;

    virtual bool isShutdown() const = 0;

    // Load the complete state of the ledger from the provided HAS. Throws if
    // any of the buckets referenced in the HAS do not exist.
    //
    // Note: this returns an _ordered_ map because we want to enable writing it
    // straight to a single "merged bucket" with a canonical order for debugging
    // purposes.
    //
    // Also note: this returns a large map -- likely multiple GB of memory on
    // public nodes. The whole ledger. Call carefully, and only offline.
    virtual std::map<LedgerKey, LedgerEntry>
    loadCompleteLedgerState(HistoryArchiveState const& has) = 0;

    // Merge the bucket list of the provided HAS into a single "super bucket"
    // consisting of only live entries, and return it.
    virtual std::shared_ptr<Bucket>
    mergeBuckets(HistoryArchiveState const& has) = 0;

    // Visits all the active ledger entries or subset thereof.
    //
    // The order in which the entries are visited is not defined, but roughly
    // goes from more fresh entries to the older ones.
    //
    // This accepts two visitors. `filterEntry` has to return `true`
    // if the ledger entry can *potentially* be accepted. The passed entry isn't
    // necessarily fresh or even alive. `acceptEntry` will only get the fresh
    // alive entries that have passed the filter. If it returns `false` the
    // iteration will immediately finish.
    //
    // When `minLedger` is specified, only entries that have been modified at
    // `minLedger` or later are visited.
    //
    // When `filterEntry` and `acceptEntry` always return `true`, this is
    // equivalent to iterating over `loadCompleteLedgerState`, so the same
    // memory/runtime implications apply.
    virtual void visitLedgerEntries(
        HistoryArchiveState const& has, std::optional<int64_t> minLedger,
        std::function<bool(LedgerEntry const&)> const& filterEntry,
        std::function<bool(LedgerEntry const&)> const& acceptEntry,
        bool includeAllStates) = 0;

    // Schedule a Work class that verifies the hashes of all referenced buckets
    // on background threads.
    virtual std::shared_ptr<BasicWork>
    scheduleVerifyReferencedBucketsWork() = 0;

    virtual Config const& getConfig() const = 0;

    // Get bucketlist snapshot
    virtual std::shared_ptr<SearchableBucketListSnapshot>
    getSearchableBucketListSnapshot() = 0;
};
}
