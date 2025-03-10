// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "simulation/LoadGenerator.h"
#include "database/Database.h"
#include "herder/Herder.h"
#include "ledger/LedgerManager.h"
#include "main/Config.h"
#include "overlay/OverlayManager.h"
#include "test/TestAccount.h"
#include "test/TxTests.h"
#include "transactions/MutableTransactionResult.h"
#include "transactions/TransactionBridge.h"
#include "transactions/TransactionSQL.h"
#include "transactions/TransactionUtils.h"
#include "transactions/test/SorobanTxTestUtils.h"
#include "util/Logging.h"
#include "util/Math.h"
#include "util/Timer.h"
#include "util/XDRCereal.h"
#include "util/numeric.h"
#include "util/types.h"

#include "xdrpp/marshal.h"

#include "medida/meter.h"
#include "medida/metrics_registry.h"

#include "ledger/test/LedgerTestUtils.h"
#include <Tracy.hpp>
#include <cmath>
#include <crypto/SHA.h>
#include <fmt/format.h>
#include <iomanip>
#include <set>

namespace stellar
{

using namespace std;
using namespace txtest;

namespace
{
// Default distribution settings, largely based on averages seen on testnet
constexpr unsigned short DEFAULT_OP_COUNT = 1;
constexpr uint32_t DEFAULT_WASM_BYTES = 35 * 1024;
constexpr uint32_t DEFAULT_NUM_DATA_ENTRIES = 2;
constexpr uint32_t DEFAULT_IO_KILOBYTES = 1;
constexpr uint32_t DEFAULT_TX_SIZE_BYTES = 256;
constexpr uint64_t DEFAULT_INSTRUCTIONS = 28'000'000;

// Sample from a discrete distribution of `values` with weights `weights`.
// Returns `defaultValue` if `values` is empty.
template <typename T>
T
sampleDiscrete(std::vector<T> const& values,
               std::vector<uint32_t> const& weights, T defaultValue)
{
    if (values.empty())
    {
        return defaultValue;
    }

    std::discrete_distribution<uint32_t> distribution(weights.begin(),
                                                      weights.end());
    return values.at(distribution(gRandomEngine));
}
} // namespace

// Units of load are scheduled at 100ms intervals.
const uint32_t LoadGenerator::STEP_MSECS = 100;

// If submission fails with txBAD_SEQ, attempt refreshing the account or
// re-submitting a new payment
const uint32_t LoadGenerator::TX_SUBMIT_MAX_TRIES = 10;

// After successfully submitting desired load, wait a bit to let it get into the
// ledger.
const uint32_t LoadGenerator::TIMEOUT_NUM_LEDGERS = 20;

// After successfully submitting desired load, wait for this many ledgers
// without checking for account consistency.
const uint32_t LoadGenerator::COMPLETION_TIMEOUT_WITHOUT_CHECKS = 4;

// Minimum unique account multiplier. This is used to calculate the minimum
// number of accounts needed to sustain desired tx/s rate (this provides a
// buffer in case loadgen is unstable and needs more accounts)
const uint32_t LoadGenerator::MIN_UNIQUE_ACCOUNT_MULTIPLIER = 3;

LoadGenerator::LoadGenerator(Application& app)
    : mMinBalance(0)
    , mLastSecond(0)
    , mApp(app)
    , mTotalSubmitted(0)
    , mLoadgenComplete(
          mApp.getMetrics().NewMeter({"loadgen", "run", "complete"}, "run"))
    , mLoadgenFail(
          mApp.getMetrics().NewMeter({"loadgen", "run", "failed"}, "run"))
    , mApplySorobanSuccess(
          mApp.getMetrics().NewCounter({"ledger", "apply-soroban", "success"}))
    , mApplySorobanFailure(
          mApp.getMetrics().NewCounter({"ledger", "apply-soroban", "failure"}))
    , mStepTimer(mApp.getMetrics().NewTimer({"loadgen", "step", "submit"}))
    , mStepMeter(
          mApp.getMetrics().NewMeter({"loadgen", "step", "count"}, "step"))
    , mTxMetrics(app.getMetrics())
    , mApplyTxTimer(
          mApp.getMetrics().NewTimer({"ledger", "transaction", "apply"}))
    , mApplyOpTimer(
          mApp.getMetrics().NewTimer({"ledger", "operation", "apply"}))
{
}

LoadGenMode
LoadGenerator::getMode(std::string const& mode)
{
    if (mode == "create")
    {
        return LoadGenMode::CREATE;
    }
    else if (mode == "pay")
    {
        return LoadGenMode::PAY;
    }
    else if (mode == "pretend")
    {
        return LoadGenMode::PRETEND;
    }
    else if (mode == "mixed_classic")
    {
        return LoadGenMode::MIXED_CLASSIC;
    }
    else if (mode == "soroban_upload")
    {
        return LoadGenMode::SOROBAN_UPLOAD;
    }
    else if (mode == "soroban_invoke_setup")
    {
        return LoadGenMode::SOROBAN_INVOKE_SETUP;
    }
    else if (mode == "soroban_invoke")
    {
        return LoadGenMode::SOROBAN_INVOKE;
    }
    else if (mode == "upgrade_setup")
    {
        return LoadGenMode::SOROBAN_UPGRADE_SETUP;
    }
    else if (mode == "create_upgrade")
    {
        return LoadGenMode::SOROBAN_CREATE_UPGRADE;
    }
    else if (mode == "mixed_classic_soroban")
    {
        return LoadGenMode::MIXED_CLASSIC_SOROBAN;
    }
    else
    {
        throw std::runtime_error(
            fmt::format(FMT_STRING("Unknown loadgen mode: {}"), mode));
    }
}

int
generateFee(std::optional<uint32_t> maxGeneratedFeeRate, Application& app,
            size_t opsCnt)
{
    int fee = 0;
    auto baseFee = app.getLedgerManager().getLastTxFee();

    if (maxGeneratedFeeRate)
    {
        auto feeRateDistr =
            uniform_int_distribution<uint32_t>(baseFee, *maxGeneratedFeeRate);
        // Add a bit more fee to get non-integer fee rates, such that
        // `floor(fee / opsCnt) == feeRate`, but
        // `fee / opsCnt >= feeRate`.
        // This is to create a bit more realistic fee structure: in reality not
        // every transaction would necessarily have the `fee == ops_count *
        // some_int`. This also would exercise more code paths/logic during the
        // transaction comparisons.
        auto fractionalFeeDistr = uniform_int_distribution<uint32_t>(
            0, static_cast<uint32_t>(opsCnt) - 1);
        fee = static_cast<uint32_t>(opsCnt) * feeRateDistr(gRandomEngine) +
              fractionalFeeDistr(gRandomEngine);
    }
    else
    {
        fee = static_cast<int>(opsCnt * baseFee);
    }

    return fee;
}

void
LoadGenerator::createRootAccount()
{
    releaseAssert(!mRoot);
    auto rootTestAccount = TestAccount::createRoot(mApp);
    mRoot = make_shared<TestAccount>(rootTestAccount);
    if (!loadAccount(mRoot, mApp))
    {
        CLOG_ERROR(LoadGen, "Could not retrieve root account!");
    }
}

unsigned short
LoadGenerator::chooseOpCount(Config const& cfg) const
{
    return sampleDiscrete(cfg.LOADGEN_OP_COUNT_FOR_TESTING,
                          cfg.LOADGEN_OP_COUNT_DISTRIBUTION_FOR_TESTING,
                          DEFAULT_OP_COUNT);
}

int64_t
LoadGenerator::getTxPerStep(uint32_t txRate, std::chrono::seconds spikeInterval,
                            uint32_t spikeSize)
{
    if (!mStartTime)
    {
        throw std::runtime_error("Load generation start time must be set");
    }

    mStepMeter.Mark();

    auto now = mApp.getClock().now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - *mStartTime);
    auto txs =
        bigDivideOrThrow(elapsed.count(), txRate, 1000, Rounding::ROUND_DOWN);
    if (spikeInterval.count() > 0)
    {
        txs += bigDivideOrThrow(
                   std::chrono::duration_cast<std::chrono::seconds>(elapsed)
                       .count(),
                   1, spikeInterval.count(), Rounding::ROUND_DOWN) *
               spikeSize;
    }

    if (txs <= mTotalSubmitted)
    {
        return 0;
    }

    return txs - mTotalSubmitted;
}

void
LoadGenerator::cleanupAccounts()
{
    ZoneScoped;

    // Check if creation source accounts have been created
    for (auto it = mCreationSourceAccounts.begin();
         it != mCreationSourceAccounts.end();)
    {
        if (loadAccount(it->second, mApp))
        {
            mAccountsAvailable.insert(it->first);
            it = mCreationSourceAccounts.erase(it);
        }
        else
        {
            ++it;
        }
    }

    // "Free" any accounts that aren't used by the tx queue anymore
    for (auto it = mAccountsInUse.begin(); it != mAccountsInUse.end();)
    {
        auto accIt = mAccounts.find(*it);
        releaseAssert(accIt != mAccounts.end());
        if (!mApp.getHerder().sourceAccountPending(
                accIt->second->getPublicKey()))
        {
            mAccountsAvailable.insert(*it);
            it = mAccountsInUse.erase(it);
        }
        else
        {
            it++;
        }
    }
}

// Reset everything except Soroban persistent state
void
LoadGenerator::reset()
{
    mAccounts.clear();
    mAccountsInUse.clear();
    mAccountsAvailable.clear();
    mCreationSourceAccounts.clear();
    mContractInstances.clear();
    mLoadTimer.reset();
    mRoot.reset();
    mStartTime.reset();
    mTotalSubmitted = 0;
    mWaitTillCompleteForLedgers = 0;
    mSorobanWasmWaitTillLedgers = 0;
    mFailed = false;
    mStarted = false;
    mInitialAccountsCreated = false;
    mPreLoadgenApplySorobanSuccess = 0;
    mPreLoadgenApplySorobanFailure = 0;
}

// Reset Soroban persistent state
void
LoadGenerator::resetSorobanState()
{
    mContractInstanceKeys.clear();
    mCodeKey.reset();
    mContactOverheadBytes = 0;
}

void
LoadGenerator::start(GeneratedLoadConfig& cfg)
{
    if (mStarted)
    {
        return;
    }

    createRootAccount();

    if (cfg.txRate == 0)
    {
        cfg.txRate = 1;
    }

    // Setup config for soroban modes
    if (cfg.isSoroban() && cfg.mode != LoadGenMode::SOROBAN_UPLOAD)
    {
        auto& sorobanLoadCfg = cfg.getMutSorobanConfig();
        sorobanLoadCfg.nWasms = 1;

        if (cfg.mode == LoadGenMode::SOROBAN_UPGRADE_SETUP)
        {
            // Only deploy single upgrade contract instance
            sorobanLoadCfg.nInstances = 1;
        }

        if (cfg.mode == LoadGenMode::SOROBAN_CREATE_UPGRADE)
        {
            // Submit a single upgrade TX
            sorobanLoadCfg.nInstances = 1;
            cfg.nTxs = 1;
        }

        if (cfg.isSorobanSetup())
        {
            resetSorobanState();

            // For first round of txs, we need to deploy the wasms.
            // waitTillFinished will set nTxs for instances once wasms have been
            // verified
            cfg.nTxs = sorobanLoadCfg.nWasms;

            // Must include all TXs
            cfg.skipLowFeeTxs = false;

            // No spikes during setup
            cfg.spikeInterval = std::chrono::seconds(0);
            cfg.spikeSize = 0;
        }

        if (cfg.modeSetsUpInvoke() || cfg.modeInvokes())
        {
            // Default instances to 1
            if (sorobanLoadCfg.nInstances == 0)
            {
                sorobanLoadCfg.nInstances = 1;
            }
        }
    }

    if (cfg.mode != LoadGenMode::CREATE)
    {
        // Mark all accounts "available" as source accounts
        for (auto i = 0u; i < cfg.nAccounts; i++)
        {
            mAccountsAvailable.insert(i + cfg.offset);
        }

        if (cfg.modeInvokes())
        {
            auto const& sorobanLoadCfg = cfg.getSorobanConfig();
            releaseAssert(mContractInstances.empty());
            releaseAssert(mCodeKey);
            releaseAssert(mAccountsAvailable.size() >= cfg.nAccounts);
            releaseAssert(mContractInstanceKeys.size() >=
                          sorobanLoadCfg.nInstances);
            releaseAssert(cfg.nAccounts >= sorobanLoadCfg.nInstances);

            // assign a contract instance to each accountID
            auto accountIter = mAccountsAvailable.begin();
            for (size_t i = 0; i < cfg.nAccounts; ++i)
            {
                auto instanceKeyIter = mContractInstanceKeys.begin();
                std::advance(instanceKeyIter, i % sorobanLoadCfg.nInstances);

                ContractInstance instance;
                instance.readOnlyKeys.emplace_back(*mCodeKey);
                instance.readOnlyKeys.emplace_back(*instanceKeyIter);
                instance.contractID = instanceKeyIter->contractData().contract;
                mContractInstances.emplace(*accountIter, instance);
                ++accountIter;
            }
        }
    }

    releaseAssert(!mLoadTimer);
    mLoadTimer = std::make_unique<VirtualTimer>(mApp.getClock());

    releaseAssert(!mStartTime);
    mStartTime =
        std::make_unique<VirtualClock::time_point>(mApp.getClock().now());

    releaseAssert(mPreLoadgenApplySorobanSuccess == 0);
    releaseAssert(mPreLoadgenApplySorobanFailure == 0);
    mPreLoadgenApplySorobanSuccess = mApplySorobanSuccess.count();
    mPreLoadgenApplySorobanFailure = mApplySorobanFailure.count();

    mStarted = true;
}

// Schedule a callback to generateLoad() STEP_MSECS milliseconds from now.
void
LoadGenerator::scheduleLoadGeneration(GeneratedLoadConfig cfg)
{
    std::optional<std::string> errorMsg;
    // If previously scheduled step of load did not succeed, fail this loadgen
    // run.
    if (mFailed)
    {
        errorMsg = "Load generation failed, ensure correct "
                   "number parameters are set and accounts are "
                   "created, or retry with smaller tx rate.";
    }

    // During load submission, we must have enough unique source accounts (with
    // a buffer) to accommodate the desired tx rate.
    if (cfg.mode != LoadGenMode::CREATE && cfg.nTxs > cfg.nAccounts &&
        (cfg.txRate * Herder::EXP_LEDGER_TIMESPAN_SECONDS.count()) *
                MIN_UNIQUE_ACCOUNT_MULTIPLIER >
            cfg.nAccounts)
    {
        errorMsg = fmt::format(
            "Tx rate is too high, there are not enough unique accounts. Make "
            "sure there are at least {}x "
            "unique accounts than desired number of transactions per ledger.",
            MIN_UNIQUE_ACCOUNT_MULTIPLIER);
    }

    if (cfg.isSoroban() &&
        protocolVersionIsBefore(mApp.getLedgerManager()
                                    .getLastClosedLedgerHeader()
                                    .header.ledgerVersion,
                                SOROBAN_PROTOCOL_VERSION))
    {
        errorMsg = "Soroban modes require protocol version 20 or higher";
    }

    if (cfg.modeInvokes())
    {
        auto const& sorobanLoadCfg = cfg.getSorobanConfig();
        releaseAssertOrThrow(sorobanLoadCfg.nInstances != 0);
        if (mContractInstanceKeys.size() < sorobanLoadCfg.nInstances ||
            !mCodeKey)
        {
            errorMsg = "must run SOROBAN_INVOKE_SETUP with at least nInstances";
        }
        else if (cfg.nAccounts < sorobanLoadCfg.nInstances)
        {
            errorMsg = "must have more accounts than instances";
        }
    }

    if (cfg.mode == LoadGenMode::SOROBAN_CREATE_UPGRADE)
    {
        if (mContractInstanceKeys.size() != 1 || !mCodeKey)
        {
            errorMsg = "must run SOROBAN_UPGRADE_SETUP";
        }
    }

    if (errorMsg)
    {
        CLOG_ERROR(LoadGen, "{}", *errorMsg);
        mLoadgenFail.Mark();
        reset();
        if (cfg.isSorobanSetup())
        {
            resetSorobanState();
        }

        return;
    }

    if (mApp.getState() == Application::APP_SYNCED_STATE)
    {
        mLoadTimer->expires_from_now(std::chrono::milliseconds(STEP_MSECS));
        mLoadTimer->async_wait([this, cfg]() { this->generateLoad(cfg); },
                               &VirtualTimer::onFailureNoop);
    }
    else
    {
        CLOG_WARNING(
            LoadGen,
            "Application is not in sync, load generation inhibited. State {}",
            mApp.getStateHuman());
        mLoadTimer->expires_from_now(std::chrono::seconds(10));
        mLoadTimer->async_wait(
            [this, cfg]() { this->scheduleLoadGeneration(cfg); },
            &VirtualTimer::onFailureNoop);
    }
}

bool
GeneratedLoadConfig::isDone() const
{
    return (isCreate() && nAccounts == 0) || (isLoad() && nTxs == 0) ||
           (isSorobanSetup() && getSorobanConfig().nInstances == 0);
}

bool
GeneratedLoadConfig::areTxsRemaining() const
{
    return (isCreate() && nAccounts != 0) || (!isCreate() && nTxs != 0);
}

Json::Value
GeneratedLoadConfig::getStatus() const
{
    Json::Value ret;
    std::string modeStr;
    switch (mode)
    {
    case LoadGenMode::CREATE:
        modeStr = "create";
        break;
    case LoadGenMode::PAY:
        modeStr = "pay";
        break;
    case LoadGenMode::PRETEND:
        modeStr = "pretend";
        break;
    case LoadGenMode::MIXED_CLASSIC:
        modeStr = "mixed_classic";
        break;
    case LoadGenMode::SOROBAN_UPLOAD:
        modeStr = "soroban_upload";
        break;
    case LoadGenMode::SOROBAN_INVOKE_SETUP:
        modeStr = "soroban_invoke_setup";
        break;
    case LoadGenMode::SOROBAN_INVOKE:
        modeStr = "soroban_invoke";
        break;
    case LoadGenMode::SOROBAN_UPGRADE_SETUP:
        modeStr = "upgrade_setup";
        break;
    case LoadGenMode::SOROBAN_CREATE_UPGRADE:
        modeStr = "create_upgrade";
        break;
    case LoadGenMode::MIXED_CLASSIC_SOROBAN:
        modeStr = "mixed_classic_soroban";
        break;
    }

    ret["mode"] = modeStr;

    if (isCreate())
    {
        ret["accounts_remaining"] = nAccounts;
    }
    else if (isSorobanSetup())
    {
        ret["wasms_remaining"] = getSorobanConfig().nWasms;
        ret["instances_remaining"] = getSorobanConfig().nInstances;
    }
    else
    {
        ret["txs_remaining"] = nTxs;
    }

    ret["tx_rate"] = std::to_string(txRate) + " tx/s";
    if (mode == LoadGenMode::MIXED_CLASSIC)
    {
        ret["dex_tx_percent"] = std::to_string(getDexTxPercent()) + "%";
    }
    else if (modeInvokes())
    {
        ret["instances"] = getSorobanConfig().nInstances;
        ret["wasms"] = getSorobanConfig().nWasms;
    }

    if (mode == LoadGenMode::MIXED_CLASSIC_SOROBAN)
    {
        auto const& blendCfg = getMixClassicSorobanConfig();
        ret["pay_weight"] = blendCfg.payWeight;
        ret["soroban_upload_weight"] = blendCfg.sorobanUploadWeight;
        ret["soroban_invoke_weight"] = blendCfg.sorobanInvokeWeight;
    }

    if (isSoroban())
    {
        ret["min_soroban_percent_success"] = mMinSorobanPercentSuccess;
    }

    return ret;
}

// Generate one "step" worth of load (assuming 1 step per STEP_MSECS) at a
// given target number of accounts and txs, and a given target tx/s rate.
// If work remains after the current step, call scheduleLoadGeneration()
// with the remainder.
void
LoadGenerator::generateLoad(GeneratedLoadConfig cfg)
{
    ZoneScoped;

    start(cfg);

    // Finish if no more txs need to be created.
    if (!cfg.areTxsRemaining())
    {
        // Done submitting the load, now ensure it propagates to the DB.
        if (!cfg.isCreate() && cfg.skipLowFeeTxs)
        {
            // skipLowFeeTxs allows triggering tx queue limiter, which
            // makes it hard to track the final seq nums. Hence just
            // wait unconditionally.
            waitTillCompleteWithoutChecks();
        }
        else
        {
            waitTillComplete(cfg);
        }
        return;
    }

    updateMinBalance();

    auto txPerStep = getTxPerStep(cfg.txRate, cfg.spikeInterval, cfg.spikeSize);
    if (cfg.mode == LoadGenMode::CREATE)
    {
        // Limit creation to the number of accounts we have. This is only the
        // case at the very beginning, when only root account is available for
        // account creation
        size_t expectedSize =
            mInitialAccountsCreated ? mAccountsAvailable.size() : 1;
        txPerStep = std::min<int64_t>(txPerStep, expectedSize);
    }
    auto submitScope = mStepTimer.TimeScope();

    uint64_t now = mApp.timeNow();
    // Cleaning up accounts every second, so we don't call potentially expensive
    // cleanup function too often
    if (now != mLastSecond)
    {
        cleanupAccounts();
    }

    uint32_t ledgerNum = mApp.getLedgerManager().getLastClosedLedgerNum() + 1;

    for (int64_t i = 0; i < txPerStep; ++i)
    {
        if (cfg.mode == LoadGenMode::CREATE)
        {
            cfg.nAccounts =
                submitCreationTx(cfg.nAccounts, cfg.offset, ledgerNum);
        }
        else
        {
            if (mAccountsAvailable.empty())
            {
                CLOG_WARNING(
                    LoadGen,
                    "Load generation failed: no more accounts available");
                mLoadgenFail.Mark();
                reset();
                return;
            }

            uint64_t sourceAccountId = getNextAvailableAccount(ledgerNum);

            std::function<std::pair<LoadGenerator::TestAccountPtr,
                                    TransactionFrameBaseConstPtr>()>
                generateTx;

            switch (cfg.mode)
            {
            case LoadGenMode::CREATE:
                releaseAssert(false);
                break;
            case LoadGenMode::PAY:
                generateTx = [&]() {
                    return paymentTransaction(cfg.nAccounts, cfg.offset,
                                              ledgerNum, sourceAccountId, 1,
                                              cfg.maxGeneratedFeeRate);
                };
                break;
            case LoadGenMode::PRETEND:
            {
                auto opCount = chooseOpCount(mApp.getConfig());
                generateTx = [&, opCount]() {
                    return pretendTransaction(cfg.nAccounts, cfg.offset,
                                              ledgerNum, sourceAccountId,
                                              opCount, cfg.maxGeneratedFeeRate);
                };
            }
            break;
            case LoadGenMode::MIXED_CLASSIC:
            {
                auto opCount = chooseOpCount(mApp.getConfig());
                bool isDex =
                    rand_uniform<uint32_t>(1, 100) <= cfg.getDexTxPercent();
                generateTx = [&, opCount, isDex]() {
                    if (isDex)
                    {
                        return manageOfferTransaction(ledgerNum,
                                                      sourceAccountId, opCount,
                                                      cfg.maxGeneratedFeeRate);
                    }
                    else
                    {
                        return paymentTransaction(
                            cfg.nAccounts, cfg.offset, ledgerNum,
                            sourceAccountId, opCount, cfg.maxGeneratedFeeRate);
                    }
                };
            }
            break;
            case LoadGenMode::SOROBAN_UPLOAD:
            {
                generateTx = [&]() {
                    return sorobanRandomWasmTransaction(
                        ledgerNum, sourceAccountId,
                        generateFee(cfg.maxGeneratedFeeRate, mApp,
                                    /* opsCnt */ 1));
                };
            }
            break;
            case LoadGenMode::SOROBAN_INVOKE_SETUP:
            case LoadGenMode::SOROBAN_UPGRADE_SETUP:
                generateTx = [&] {
                    auto& sorobanCfg = cfg.getMutSorobanConfig();
                    if (sorobanCfg.nWasms != 0)
                    {
                        --sorobanCfg.nWasms;
                        return createUploadWasmTransaction(
                            ledgerNum, sourceAccountId, cfg);
                    }
                    else
                    {
                        --sorobanCfg.nInstances;
                        return createContractTransaction(ledgerNum,
                                                         sourceAccountId, cfg);
                    }
                };
                break;
            case LoadGenMode::SOROBAN_INVOKE:
                generateTx = [&]() {
                    return invokeSorobanLoadTransaction(ledgerNum,
                                                        sourceAccountId, cfg);
                };
                break;
            case LoadGenMode::SOROBAN_CREATE_UPGRADE:
                generateTx = [&]() {
                    return invokeSorobanCreateUpgradeTransaction(
                        ledgerNum, sourceAccountId, cfg);
                };
                break;
            case LoadGenMode::MIXED_CLASSIC_SOROBAN:
                generateTx = [&]() {
                    return createMixedClassicSorobanTransaction(
                        ledgerNum, sourceAccountId, cfg);
                };
                break;
            }

            if (submitTx(cfg, generateTx))
            {
                --cfg.nTxs;
            }
            else if (mFailed)
            {
                break;
            }
        }
        if (cfg.nAccounts == 0 || !cfg.areTxsRemaining())
        {
            // Nothing to do for the rest of the step
            break;
        }
    }

    auto submit = submitScope.Stop();

    now = mApp.timeNow();

    // Emit a log message once per second.
    if (now != mLastSecond)
    {
        logProgress(submit, cfg);
    }

    mLastSecond = now;
    mTotalSubmitted += txPerStep;
    scheduleLoadGeneration(cfg);
}

uint32_t
LoadGenerator::submitCreationTx(uint32_t nAccounts, uint32_t offset,
                                uint32_t ledgerNum)
{
    uint32_t numToProcess =
        nAccounts < MAX_OPS_PER_TX ? nAccounts : MAX_OPS_PER_TX;
    auto [from, tx] =
        creationTransaction(mAccounts.size() + offset, numToProcess, ledgerNum);
    TransactionResultCode code;
    TransactionQueue::AddResultCode status;
    bool createDuplicate = false;
    uint32_t numTries = 0;

    while ((status = execute(tx, LoadGenMode::CREATE, code)) !=
           TransactionQueue::AddResultCode::ADD_STATUS_PENDING)
    {
        // Ignore duplicate transactions, simply continue generating load
        if (status == TransactionQueue::AddResultCode::ADD_STATUS_DUPLICATE)
        {
            createDuplicate = true;
            break;
        }

        if (++numTries >= TX_SUBMIT_MAX_TRIES ||
            status != TransactionQueue::AddResultCode::ADD_STATUS_ERROR)
        {
            // Failed to submit the step of load
            mFailed = true;
            return 0;
        }

        // In case of bad seqnum, attempt refreshing it from the DB
        maybeHandleFailedTx(tx, from, status, code);
    }

    if (!createDuplicate)
    {
        nAccounts -= numToProcess;
    }

    return nAccounts;
}

bool
LoadGenerator::submitTx(GeneratedLoadConfig const& cfg,
                        std::function<std::pair<LoadGenerator::TestAccountPtr,
                                                TransactionFrameBaseConstPtr>()>
                            generateTx)
{
    auto [from, tx] = generateTx();

    TransactionResultCode code;
    TransactionQueue::AddResultCode status;
    uint32_t numTries = 0;

    while ((status = execute(tx, cfg.mode, code)) !=
           TransactionQueue::AddResultCode::ADD_STATUS_PENDING)
    {

        if (cfg.skipLowFeeTxs &&
            (status ==
                 TransactionQueue::AddResultCode::ADD_STATUS_TRY_AGAIN_LATER ||
             (status == TransactionQueue::AddResultCode::ADD_STATUS_ERROR &&
              code == txINSUFFICIENT_FEE)))
        {
            // Rollback the seq num of the test account as we regenerate the
            // transaction.
            from->setSequenceNumber(from->getLastSequenceNumber() - 1);
            CLOG_INFO(LoadGen, "skipped low fee tx with fee {}",
                      tx->getInclusionFee());
            return false;
        }
        if (++numTries >= TX_SUBMIT_MAX_TRIES ||
            status != TransactionQueue::AddResultCode::ADD_STATUS_ERROR)
        {
            mFailed = true;
            return false;
        }

        // In case of bad seqnum, attempt refreshing it from the DB
        maybeHandleFailedTx(tx, from, status, code); // Update seq num

        // Regenerate a new payment tx
        std::tie(from, tx) = generateTx();
    }

    return true;
}

uint64_t
LoadGenerator::getNextAvailableAccount(uint32_t ledgerNum)
{
    uint64_t sourceAccountId;
    do
    {
        releaseAssert(!mAccountsAvailable.empty());

        auto sourceAccountIdx =
            rand_uniform<uint64_t>(0, mAccountsAvailable.size() - 1);
        auto it = mAccountsAvailable.begin();
        std::advance(it, sourceAccountIdx);
        sourceAccountId = *it;
        mAccountsAvailable.erase(it);
        releaseAssert(mAccountsInUse.insert(sourceAccountId).second);

        // Although mAccountsAvailable shouldn't contain pending accounts, it is
        // possible when the network is overloaded. Consider the following
        // scenario:
        // 1. This node generates a transaction `t` using account `a` and
        //    broadcasts it on. In doing so, loadgen marks `a` as in use,
        //    removing it from `mAccountsAvailable.
        // 2. For whatever reason, `t` never makes it out of the queue and this
        //    node bans it.
        // 3. After some period of time, this node unbans `t` because bans only
        //    last for so many ledgers.
        // 4. Loadgen marks `a` available, moving it back into
        //    `mAccountsAvailable`.
        // 5. This node hears about `t` again on the network and (as it is no
        //    longer banned) adds it back to the queue
        // 6. getNextAvailableAccount draws `a` from `mAccountsAvailable`.
        //    However, `a` is no longer available as `t` is in the transaction
        //    queue!
        //
        // In this scenario, returning `a` results in an assertion failure
        // later. To resolve this, we resample a new account by simply looping
        // here.
    } while (mApp.getHerder().sourceAccountPending(
        findAccount(sourceAccountId, ledgerNum)->getPublicKey()));

    return sourceAccountId;
}

void
LoadGenerator::logProgress(std::chrono::nanoseconds submitTimer,
                           GeneratedLoadConfig const& cfg) const
{
    using namespace std::chrono;

    auto& applyTx = mApplyTxTimer;
    auto& applyOp = mApplyOpTimer;

    auto submitSteps = duration_cast<milliseconds>(submitTimer).count();

    auto remainingTxCount = 0;
    if (cfg.mode == LoadGenMode::CREATE)
    {
        remainingTxCount = cfg.nAccounts / MAX_OPS_PER_TX;
    }
    else if (cfg.isSorobanSetup())
    {
        remainingTxCount =
            cfg.getSorobanConfig().nWasms + cfg.getSorobanConfig().nInstances;
    }
    else
    {
        remainingTxCount = cfg.nTxs;
    }

    auto etaSecs = (uint32_t)(((double)remainingTxCount) /
                              max<double>(1, applyTx.one_minute_rate()));

    auto etaHours = etaSecs / 3600;
    auto etaMins = etaSecs % 60;

    if (cfg.isSoroban())
    {
        CLOG_INFO(LoadGen,
                  "Tx/s: {} target, {} tx actual (1m EWMA). Pending: {} txs. "
                  "ETA: {}h{}m",
                  cfg.txRate, applyTx.one_minute_rate(), remainingTxCount,
                  etaHours, etaMins);
    }
    else if (cfg.mode == LoadGenMode::CREATE)
    {
        CLOG_INFO(LoadGen,
                  "Tx/s: {} target, {}tx/{}op actual (1m EWMA). Pending: {} "
                  "accounts, {} txs. ETA: {}h{}m",
                  cfg.txRate, applyTx.one_minute_rate(),
                  applyOp.one_minute_rate(), cfg.nAccounts, remainingTxCount,
                  etaHours, etaMins);
    }
    else
    {
        CLOG_INFO(LoadGen,
                  "Tx/s: {} target, {}tx/{}op actual (1m EWMA). Pending: {} "
                  "txs. ETA: {}h{}m",
                  cfg.txRate, applyTx.one_minute_rate(),
                  applyOp.one_minute_rate(), remainingTxCount, etaHours,
                  etaMins);
    }

    CLOG_DEBUG(LoadGen, "Step timing: {}ms submit.", submitSteps);

    mTxMetrics.report();
}

std::pair<LoadGenerator::TestAccountPtr, TransactionFrameBaseConstPtr>
LoadGenerator::creationTransaction(uint64_t startAccount, uint64_t numItems,
                                   uint32_t ledgerNum)
{
    TestAccountPtr sourceAcc =
        mInitialAccountsCreated
            ? findAccount(getNextAvailableAccount(ledgerNum), ledgerNum)
            : mRoot;
    vector<Operation> creationOps = createAccounts(
        startAccount, numItems, ledgerNum, !mInitialAccountsCreated);
    mInitialAccountsCreated = true;
    return std::make_pair(
        sourceAcc, createTransactionFrame(sourceAcc, creationOps,
                                          LoadGenMode::CREATE, std::nullopt));
}

void
LoadGenerator::updateMinBalance()
{
    auto b = mApp.getLedgerManager().getLastMinBalance(0);
    if (b > mMinBalance)
    {
        mMinBalance = b;
    }
}

std::vector<Operation>
LoadGenerator::createAccounts(uint64_t start, uint64_t count,
                              uint32_t ledgerNum, bool initialAccounts)
{
    vector<Operation> ops;
    SequenceNumber sn = static_cast<SequenceNumber>(ledgerNum) << 32;
    auto balance = initialAccounts ? mMinBalance * 10000000 : mMinBalance * 100;
    for (uint64_t i = start; i < start + count; i++)
    {
        auto name = "TestAccount-" + to_string(i);
        auto account = TestAccount{mApp, txtest::getAccount(name.c_str()), sn};
        ops.push_back(txtest::createAccount(account.getPublicKey(), balance));

        // Cache newly created account
        auto acc = make_shared<TestAccount>(account);
        mAccounts.emplace(i, acc);
        if (initialAccounts)
        {
            mCreationSourceAccounts.emplace(i, acc);
        }
    }
    return ops;
}

bool
LoadGenerator::loadAccount(TestAccount& account, Application& app)
{
    LedgerSnapshot lsg(mApp);
    auto const entry = lsg.getAccount(account.getPublicKey());
    if (!entry)
    {
        return false;
    }
    account.setSequenceNumber(entry.current().data.account().seqNum);
    return true;
}

bool
LoadGenerator::loadAccount(TestAccountPtr acc, Application& app)
{
    if (acc)
    {
        return loadAccount(*acc, app);
    }
    return false;
}

std::pair<LoadGenerator::TestAccountPtr, LoadGenerator::TestAccountPtr>
LoadGenerator::pickAccountPair(uint32_t numAccounts, uint32_t offset,
                               uint32_t ledgerNum, uint64_t sourceAccountId)
{
    auto sourceAccount = findAccount(sourceAccountId, ledgerNum);
    releaseAssert(
        !mApp.getHerder().sourceAccountPending(sourceAccount->getPublicKey()));

    auto destAccountId = rand_uniform<uint64_t>(0, numAccounts - 1) + offset;

    auto destAccount = findAccount(destAccountId, ledgerNum);

    CLOG_DEBUG(LoadGen, "Generated pair for payment tx - {} and {}",
               sourceAccountId, destAccountId);
    return std::pair<TestAccountPtr, TestAccountPtr>(sourceAccount,
                                                     destAccount);
}

LoadGenerator::TestAccountPtr
LoadGenerator::findAccount(uint64_t accountId, uint32_t ledgerNum)
{
    // Load account and cache it.
    TestAccountPtr newAccountPtr;

    auto res = mAccounts.find(accountId);
    if (res == mAccounts.end())
    {
        SequenceNumber sn = static_cast<SequenceNumber>(ledgerNum) << 32;
        auto name = "TestAccount-" + std::to_string(accountId);
        newAccountPtr =
            std::make_shared<TestAccount>(mApp, txtest::getAccount(name), sn);

        if (!loadAccount(newAccountPtr, mApp))
        {
            throw std::runtime_error(
                fmt::format("Account {0} must exist in the DB.", accountId));
        }
        mAccounts.insert(
            std::pair<uint64_t, TestAccountPtr>(accountId, newAccountPtr));
    }
    else
    {
        newAccountPtr = res->second;
    }

    return newAccountPtr;
}

std::pair<LoadGenerator::TestAccountPtr, TransactionFrameBaseConstPtr>
LoadGenerator::paymentTransaction(uint32_t numAccounts, uint32_t offset,
                                  uint32_t ledgerNum, uint64_t sourceAccount,
                                  uint32_t opCount,
                                  std::optional<uint32_t> maxGeneratedFeeRate)
{
    TestAccountPtr to, from;
    uint64_t amount = 1;
    std::tie(from, to) =
        pickAccountPair(numAccounts, offset, ledgerNum, sourceAccount);
    vector<Operation> paymentOps;
    paymentOps.reserve(opCount);
    for (uint32_t i = 0; i < opCount; ++i)
    {
        paymentOps.emplace_back(txtest::payment(to->getPublicKey(), amount));
    }

    return std::make_pair(from, createTransactionFrame(from, paymentOps,
                                                       LoadGenMode::PAY,
                                                       maxGeneratedFeeRate));
}

std::pair<LoadGenerator::TestAccountPtr, TransactionFrameBaseConstPtr>
LoadGenerator::manageOfferTransaction(
    uint32_t ledgerNum, uint64_t accountId, uint32_t opCount,
    std::optional<uint32_t> maxGeneratedFeeRate)
{
    auto account = findAccount(accountId, ledgerNum);
    Asset selling(ASSET_TYPE_NATIVE);
    Asset buying(ASSET_TYPE_CREDIT_ALPHANUM4);
    strToAssetCode(buying.alphaNum4().assetCode, "USD");
    vector<Operation> ops;
    for (uint32_t i = 0; i < opCount; ++i)
    {
        ops.emplace_back(txtest::manageBuyOffer(
            rand_uniform<int64_t>(1, 10000000), selling, buying,
            Price{rand_uniform<int32_t>(1, 100), rand_uniform<int32_t>(1, 100)},
            100));
    }
    return std::make_pair(account, createTransactionFrame(
                                       account, ops, LoadGenMode::MIXED_CLASSIC,
                                       maxGeneratedFeeRate));
}

static void
increaseOpSize(Operation& op, uint32_t increaseUpToBytes)
{
    if (increaseUpToBytes == 0)
    {
        return;
    }

    SorobanAuthorizationEntry auth;
    auth.credentials.type(SOROBAN_CREDENTIALS_SOURCE_ACCOUNT);
    auth.rootInvocation.function.type(
        SOROBAN_AUTHORIZED_FUNCTION_TYPE_CONTRACT_FN);
    SCVal val(SCV_BYTES);

    auto const overheadBytes = xdr::xdr_size(auth) + xdr::xdr_size(val);
    if (overheadBytes > increaseUpToBytes)
    {
        increaseUpToBytes = 0;
    }
    else
    {
        increaseUpToBytes -= overheadBytes;
    }

    val.bytes().resize(increaseUpToBytes);
    auth.rootInvocation.function.contractFn().args = {val};
    op.body.invokeHostFunctionOp().auth = {auth};
}

std::pair<LoadGenerator::TestAccountPtr, TransactionFrameBaseConstPtr>
LoadGenerator::createUploadWasmTransaction(uint32_t ledgerNum,
                                           uint64_t accountId,
                                           GeneratedLoadConfig const& cfg)
{
    releaseAssert(cfg.isSorobanSetup());
    releaseAssert(!mCodeKey);
    auto wasm = cfg.modeSetsUpInvoke() ? rust_bridge::get_test_wasm_loadgen()
                                       : rust_bridge::get_write_bytes();
    auto account = findAccount(accountId, ledgerNum);

    SorobanResources uploadResources{};
    uploadResources.instructions = 2'500'000;
    uploadResources.readBytes = wasm.data.size() + 500;
    uploadResources.writeBytes = wasm.data.size() + 500;

    Operation uploadOp;
    uploadOp.body.type(INVOKE_HOST_FUNCTION);
    auto& uploadHF = uploadOp.body.invokeHostFunctionOp().hostFunction;
    uploadHF.type(HOST_FUNCTION_TYPE_UPLOAD_CONTRACT_WASM);
    uploadHF.wasm().assign(wasm.data.begin(), wasm.data.end());

    LedgerKey contractCodeLedgerKey;
    contractCodeLedgerKey.type(CONTRACT_CODE);
    contractCodeLedgerKey.contractCode().hash = sha256(uploadHF.wasm());
    uploadResources.footprint.readWrite = {contractCodeLedgerKey};

    int64_t resourceFee =
        sorobanResourceFee(mApp, uploadResources, 5000 + wasm.data.size(), 100);
    resourceFee += 1'000'000;
    auto tx = sorobanTransactionFrameFromOps(
        mApp.getNetworkID(), *account, {uploadOp}, {}, uploadResources,
        generateFee(cfg.maxGeneratedFeeRate, mApp,
                    /* opsCnt */ 1),
        resourceFee);
    mCodeKey = contractCodeLedgerKey;

    // Wasm blob + approximate overhead for contract instance and ContractCode
    // LE overhead
    mContactOverheadBytes = wasm.data.size() + 160;
    return std::make_pair(account, tx);
}

std::pair<LoadGenerator::TestAccountPtr, TransactionFrameBaseConstPtr>
LoadGenerator::createContractTransaction(uint32_t ledgerNum, uint64_t accountId,
                                         GeneratedLoadConfig const& cfg)
{
    releaseAssert(mCodeKey);

    auto account = findAccount(accountId, ledgerNum);
    SorobanResources createResources{};
    createResources.instructions = 1'000'000;
    createResources.readBytes = mContactOverheadBytes;
    createResources.writeBytes = 300;

    auto salt = sha256("upgrade" +
                       std::to_string(++mNumCreateContractTransactionCalls));
    auto contractIDPreimage = makeContractIDPreimage(*account, salt);

    auto tx = makeSorobanCreateContractTx(
        mApp, *account, contractIDPreimage,
        makeWasmExecutable(mCodeKey->contractCode().hash), createResources,
        generateFee(cfg.maxGeneratedFeeRate, mApp,
                    /* opsCnt */ 1));

    auto const& instanceLk = createResources.footprint.readWrite.back();
    mContractInstanceKeys.emplace(instanceLk);

    return std::make_pair(account, tx);
}

std::pair<LoadGenerator::TestAccountPtr, TransactionFrameBaseConstPtr>
LoadGenerator::invokeSorobanLoadTransaction(uint32_t ledgerNum,
                                            uint64_t accountId,
                                            GeneratedLoadConfig const& cfg)
{
    auto const& appCfg = mApp.getConfig();

    auto account = findAccount(accountId, ledgerNum);
    auto instanceIter = mContractInstances.find(accountId);
    releaseAssert(instanceIter != mContractInstances.end());
    auto const& instance = instanceIter->second;

    auto const& networkCfg = mApp.getLedgerManager().getSorobanNetworkConfig();

    // Approximate instruction measurements from loadgen contract. While the
    // guest and host cycle counts are exact, and we can predict the cost of the
    // guest and host loops correctly, it is difficult to estimate the CPU cost
    // of storage given that the number and size of keys is variable.
    // baseInstructionCount is a rough estimate for storage cost, but might be
    // too small if a given invocation writes many or large entries.  This means
    // some TXs will fail due to exceeding resource limitations.  However these
    // should fail at apply time, so will still generate siginificant load
    uint64_t const baseInstructionCount = 3'000'000;
    uint64_t const instructionsPerGuestCycle = 80;
    uint64_t const instructionsPerHostCycle = 5030;

    // Pick random number of cycles between bounds
    uint64_t targetInstructions =
        sampleDiscrete(appCfg.LOADGEN_INSTRUCTIONS_FOR_TESTING,
                       appCfg.LOADGEN_INSTRUCTIONS_DISTRIBUTION_FOR_TESTING,
                       DEFAULT_INSTRUCTIONS);

    // Factor in instructions for storage
    targetInstructions = baseInstructionCount >= targetInstructions
                             ? 0
                             : targetInstructions - baseInstructionCount;

    // Randomly select a number of guest cycles
    uint64_t guestCyclesMax = targetInstructions / instructionsPerGuestCycle;
    uint64_t guestCycles = rand_uniform<uint64_t>(0, guestCyclesMax);

    // Rest of instructions consumed by host cycles
    targetInstructions -= guestCycles * instructionsPerGuestCycle;
    uint64_t hostCycles = targetInstructions / instructionsPerHostCycle;

    SorobanResources resources;
    resources.footprint.readOnly = instance.readOnlyKeys;

    auto numEntries =
        sampleDiscrete(appCfg.LOADGEN_NUM_DATA_ENTRIES_FOR_TESTING,
                       appCfg.LOADGEN_NUM_DATA_ENTRIES_DISTRIBUTION_FOR_TESTING,
                       DEFAULT_NUM_DATA_ENTRIES);
    for (uint32_t i = 0; i < numEntries; ++i)
    {
        auto lk = contractDataKey(instance.contractID, makeU32(i),
                                  ContractDataDurability::PERSISTENT);
        resources.footprint.readWrite.emplace_back(lk);
    }

    std::vector<uint32_t> const& ioKilobytesValues =
        appCfg.LOADGEN_IO_KILOBYTES_FOR_TESTING;
    auto totalWriteBytes =
        sampleDiscrete(ioKilobytesValues,
                       appCfg.LOADGEN_IO_KILOBYTES_DISTRIBUTION_FOR_TESTING,
                       DEFAULT_IO_KILOBYTES) *
        1024;

    if (totalWriteBytes < mContactOverheadBytes)
    {
        totalWriteBytes = mContactOverheadBytes;
        numEntries = 0;
    }

    uint32_t kiloBytesPerEntry = 0;
    if (numEntries > 0)
    {
        kiloBytesPerEntry =
            (totalWriteBytes - mContactOverheadBytes) / numEntries / 1024;

        // If numEntries > 0, we can't write a 0 byte entry
        if (kiloBytesPerEntry == 0)
        {
            kiloBytesPerEntry = 1;
        }
    }

    auto guestCyclesU64 = makeU64(guestCycles);
    auto hostCyclesU64 = makeU64(hostCycles);
    auto numEntriesU32 = makeU32(numEntries);
    auto kiloBytesPerEntryU32 = makeU32(kiloBytesPerEntry);

    Operation op;
    op.body.type(INVOKE_HOST_FUNCTION);
    auto& ihf = op.body.invokeHostFunctionOp().hostFunction;
    ihf.type(HOST_FUNCTION_TYPE_INVOKE_CONTRACT);
    ihf.invokeContract().contractAddress = instance.contractID;
    ihf.invokeContract().functionName = "do_work";
    ihf.invokeContract().args = {guestCyclesU64, hostCyclesU64, numEntriesU32,
                                 kiloBytesPerEntryU32};

    // baseInstructionCount is a very rough estimate and may be a significant
    // underestimation based on the IO load used, so use max instructions
    resources.instructions = networkCfg.mTxMaxInstructions;

    // We don't have a good way of knowing how many bytes we will need to read
    // since the previous invocation writes a random number of bytes, so use
    // upper bound
    uint32_t const maxReadKilobytes =
        ioKilobytesValues.empty() ? DEFAULT_IO_KILOBYTES
                                  : *std::max_element(ioKilobytesValues.begin(),
                                                      ioKilobytesValues.end());
    resources.readBytes = maxReadKilobytes * 1024;
    resources.writeBytes = totalWriteBytes;

    // Approximate TX size before padding and footprint, slightly over estimated
    // by `baselineTxOverheadBytes` so we stay below limits, plus footprint size
    uint32_t constexpr baselineTxOverheadBytes = 260;
    uint32_t const txOverheadBytes =
        baselineTxOverheadBytes + xdr::xdr_size(resources);
    uint32_t desiredTxBytes =
        sampleDiscrete(appCfg.LOADGEN_TX_SIZE_BYTES_FOR_TESTING,
                       appCfg.LOADGEN_TX_SIZE_BYTES_DISTRIBUTION_FOR_TESTING,
                       DEFAULT_TX_SIZE_BYTES);
    auto paddingBytes =
        txOverheadBytes > desiredTxBytes ? 0 : desiredTxBytes - txOverheadBytes;
    increaseOpSize(op, paddingBytes);

    auto resourceFee =
        sorobanResourceFee(mApp, resources, txOverheadBytes + paddingBytes, 40);
    resourceFee += 1'000'000;

    auto tx = sorobanTransactionFrameFromOps(
        mApp.getNetworkID(), *account, {op}, {}, resources,
        generateFee(cfg.maxGeneratedFeeRate, mApp,
                    /* opsCnt */ 1),
        resourceFee);

    return std::make_pair(account, tx);
}

ConfigUpgradeSetKey
LoadGenerator::getConfigUpgradeSetKey(GeneratedLoadConfig const& cfg) const
{
    releaseAssertOrThrow(mCodeKey.has_value());
    releaseAssertOrThrow(mContractInstanceKeys.size() == 1);
    auto const& instanceLK = *mContractInstanceKeys.begin();

    SCBytes upgradeBytes = getConfigUpgradeSetFromLoadConfig(cfg);
    auto upgradeHash = sha256(upgradeBytes);

    ConfigUpgradeSetKey upgradeSetKey;
    upgradeSetKey.contentHash = upgradeHash;
    upgradeSetKey.contractID = instanceLK.contractData().contract.contractId();
    return upgradeSetKey;
}

SCBytes
LoadGenerator::getConfigUpgradeSetFromLoadConfig(
    GeneratedLoadConfig const& cfg) const
{
    xdr::xvector<ConfigSettingEntry> updatedEntries;
    auto const& upgradeCfg = cfg.getSorobanUpgradeConfig();

    LedgerSnapshot lsg(mApp);

    for (uint32_t i = 0;
         i < static_cast<uint32_t>(CONFIG_SETTING_BUCKETLIST_SIZE_WINDOW); ++i)
    {
        auto entry = lsg.load(configSettingKey(static_cast<ConfigSettingID>(i)))
                         .current();

        auto& setting = entry.data.configSetting();
        switch (static_cast<ConfigSettingID>(i))
        {
        case CONFIG_SETTING_CONTRACT_MAX_SIZE_BYTES:
            if (upgradeCfg.maxContractSizeBytes > 0)
            {
                setting.contractMaxSizeBytes() =
                    upgradeCfg.maxContractSizeBytes;
            }
            break;
        case CONFIG_SETTING_CONTRACT_COMPUTE_V0:
            if (upgradeCfg.ledgerMaxInstructions > 0)
            {
                setting.contractCompute().ledgerMaxInstructions =
                    upgradeCfg.ledgerMaxInstructions;
            }

            if (upgradeCfg.txMaxInstructions > 0)
            {
                setting.contractCompute().txMaxInstructions =
                    upgradeCfg.txMaxInstructions;
            }

            if (upgradeCfg.txMemoryLimit > 0)
            {
                setting.contractCompute().txMemoryLimit =
                    upgradeCfg.txMemoryLimit;
            }
            break;
        case CONFIG_SETTING_CONTRACT_LEDGER_COST_V0:
            if (upgradeCfg.ledgerMaxReadLedgerEntries > 0)
            {
                setting.contractLedgerCost().ledgerMaxReadLedgerEntries =
                    upgradeCfg.ledgerMaxReadLedgerEntries;
            }

            if (upgradeCfg.ledgerMaxReadBytes > 0)
            {
                setting.contractLedgerCost().ledgerMaxReadBytes =
                    upgradeCfg.ledgerMaxReadBytes;
            }

            if (upgradeCfg.ledgerMaxWriteLedgerEntries > 0)
            {
                setting.contractLedgerCost().ledgerMaxWriteLedgerEntries =
                    upgradeCfg.ledgerMaxWriteLedgerEntries;
            }

            if (upgradeCfg.ledgerMaxWriteBytes > 0)
            {
                setting.contractLedgerCost().ledgerMaxWriteBytes =
                    upgradeCfg.ledgerMaxWriteBytes;
            }

            if (upgradeCfg.txMaxReadLedgerEntries > 0)
            {
                setting.contractLedgerCost().txMaxReadLedgerEntries =
                    upgradeCfg.txMaxReadLedgerEntries;
            }

            if (upgradeCfg.txMaxReadBytes > 0)
            {
                setting.contractLedgerCost().txMaxReadBytes =
                    upgradeCfg.txMaxReadBytes;
            }

            if (upgradeCfg.txMaxWriteLedgerEntries > 0)
            {
                setting.contractLedgerCost().txMaxWriteLedgerEntries =
                    upgradeCfg.txMaxWriteLedgerEntries;
            }

            if (upgradeCfg.txMaxWriteBytes > 0)
            {
                setting.contractLedgerCost().txMaxWriteBytes =
                    upgradeCfg.txMaxWriteBytes;
            }
            break;
        case CONFIG_SETTING_CONTRACT_HISTORICAL_DATA_V0:
            break;
        case CONFIG_SETTING_CONTRACT_EVENTS_V0:
            if (upgradeCfg.txMaxContractEventsSizeBytes > 0)
            {
                setting.contractEvents().txMaxContractEventsSizeBytes =
                    upgradeCfg.txMaxContractEventsSizeBytes;
            }
            break;
        case CONFIG_SETTING_CONTRACT_BANDWIDTH_V0:
            if (upgradeCfg.ledgerMaxTransactionsSizeBytes > 0)
            {
                setting.contractBandwidth().ledgerMaxTxsSizeBytes =
                    upgradeCfg.ledgerMaxTransactionsSizeBytes;
            }

            if (upgradeCfg.txMaxSizeBytes > 0)
            {
                setting.contractBandwidth().txMaxSizeBytes =
                    upgradeCfg.txMaxSizeBytes;
            }
            break;
        case CONFIG_SETTING_CONTRACT_COST_PARAMS_CPU_INSTRUCTIONS:
        case CONFIG_SETTING_CONTRACT_COST_PARAMS_MEMORY_BYTES:
            break;
        case CONFIG_SETTING_CONTRACT_DATA_KEY_SIZE_BYTES:
            if (upgradeCfg.maxContractDataKeySizeBytes > 0)
            {
                setting.contractDataKeySizeBytes() =
                    upgradeCfg.maxContractDataKeySizeBytes;
            }
            break;
        case CONFIG_SETTING_CONTRACT_DATA_ENTRY_SIZE_BYTES:
            if (upgradeCfg.maxContractDataEntrySizeBytes > 0)
            {
                setting.contractDataEntrySizeBytes() =
                    upgradeCfg.maxContractDataEntrySizeBytes;
            }
            break;
        case CONFIG_SETTING_STATE_ARCHIVAL:
        {
            auto& ses = setting.stateArchivalSettings();
            if (upgradeCfg.maxEntryTTL > 0)
            {
                ses.maxEntryTTL = upgradeCfg.maxEntryTTL;
            }

            if (upgradeCfg.minTemporaryTTL > 0)
            {
                ses.minTemporaryTTL = upgradeCfg.minTemporaryTTL;
            }

            if (upgradeCfg.minPersistentTTL > 0)
            {
                ses.minPersistentTTL = upgradeCfg.minPersistentTTL;
            }

            if (upgradeCfg.persistentRentRateDenominator > 0)
            {
                ses.persistentRentRateDenominator =
                    upgradeCfg.persistentRentRateDenominator;
            }

            if (upgradeCfg.tempRentRateDenominator > 0)
            {
                ses.tempRentRateDenominator =
                    upgradeCfg.tempRentRateDenominator;
            }

            if (upgradeCfg.maxEntriesToArchive > 0)
            {
                ses.maxEntriesToArchive = upgradeCfg.maxEntriesToArchive;
            }

            if (upgradeCfg.bucketListSizeWindowSampleSize > 0)
            {
                ses.bucketListSizeWindowSampleSize =
                    upgradeCfg.bucketListSizeWindowSampleSize;
            }

            if (upgradeCfg.bucketListWindowSamplePeriod > 0)
            {
                ses.bucketListWindowSamplePeriod =
                    upgradeCfg.bucketListWindowSamplePeriod;
            }

            if (upgradeCfg.evictionScanSize > 0)
            {
                ses.evictionScanSize = upgradeCfg.evictionScanSize;
            }

            if (upgradeCfg.startingEvictionScanLevel > 0)
            {
                ses.startingEvictionScanLevel =
                    upgradeCfg.startingEvictionScanLevel;
            }
        }
        break;
        case CONFIG_SETTING_CONTRACT_EXECUTION_LANES:
            if (upgradeCfg.ledgerMaxTxCount > 0)
            {
                setting.contractExecutionLanes().ledgerMaxTxCount =
                    upgradeCfg.ledgerMaxTxCount;
            }
            break;
        default:
            releaseAssert(false);
            break;
        }

        // These two definitely aren't changing, and including both will hit the
        // contractDataEntrySizeBytes limit
        if (entry.data.configSetting().configSettingID() !=
                CONFIG_SETTING_CONTRACT_COST_PARAMS_CPU_INSTRUCTIONS &&
            entry.data.configSetting().configSettingID() !=
                CONFIG_SETTING_CONTRACT_COST_PARAMS_MEMORY_BYTES)
        {
            updatedEntries.emplace_back(entry.data.configSetting());
        }
    }

    ConfigUpgradeSet upgradeSet;
    upgradeSet.updatedEntry = updatedEntries;

    return xdr::xdr_to_opaque(upgradeSet);
}

std::pair<LoadGenerator::TestAccountPtr, TransactionFrameBaseConstPtr>
LoadGenerator::invokeSorobanCreateUpgradeTransaction(
    uint32_t ledgerNum, uint64_t accountId, GeneratedLoadConfig const& cfg)
{
    releaseAssert(mCodeKey);
    releaseAssert(mContractInstanceKeys.size() == 1);

    auto account = findAccount(accountId, ledgerNum);
    auto const& instanceLK = *mContractInstanceKeys.begin();
    auto const& contractID = instanceLK.contractData().contract;

    SCBytes upgradeBytes = getConfigUpgradeSetFromLoadConfig(cfg);

    LedgerKey upgradeLK(CONTRACT_DATA);
    upgradeLK.contractData().durability = TEMPORARY;
    upgradeLK.contractData().contract = contractID;

    SCVal upgradeHashBytes(SCV_BYTES);
    auto upgradeHash = sha256(upgradeBytes);
    upgradeHashBytes.bytes() = xdr::xdr_to_opaque(upgradeHash);
    upgradeLK.contractData().key = upgradeHashBytes;

    ConfigUpgradeSetKey upgradeSetKey;
    upgradeSetKey.contentHash = upgradeHash;
    upgradeSetKey.contractID = contractID.contractId();

    SorobanResources resources;
    resources.footprint.readOnly = {instanceLK, *mCodeKey};
    resources.footprint.readWrite = {upgradeLK};
    resources.instructions = 2'500'000;
    resources.readBytes = 3'100;
    resources.writeBytes = 3'100;

    SCVal b(SCV_BYTES);
    b.bytes() = upgradeBytes;

    Operation op;
    op.body.type(INVOKE_HOST_FUNCTION);
    auto& ihf = op.body.invokeHostFunctionOp().hostFunction;
    ihf.type(HOST_FUNCTION_TYPE_INVOKE_CONTRACT);
    ihf.invokeContract().contractAddress = contractID;
    ihf.invokeContract().functionName = "write";
    ihf.invokeContract().args.emplace_back(b);

    auto resourceFee = sorobanResourceFee(mApp, resources, 1'000, 40);
    resourceFee += 1'000'000;

    auto tx = sorobanTransactionFrameFromOps(
        mApp.getNetworkID(), *account, {op}, {}, resources,
        generateFee(cfg.maxGeneratedFeeRate, mApp,
                    /* opsCnt */ 1),
        resourceFee);

    return std::make_pair(account, tx);
}

std::pair<LoadGenerator::TestAccountPtr, TransactionFrameBaseConstPtr>
LoadGenerator::sorobanRandomWasmTransaction(uint32_t ledgerNum,
                                            uint64_t accountId,
                                            uint32_t inclusionFee)
{
    auto [resources, wasmSize] = sorobanRandomUploadResources();

    auto account = findAccount(accountId, ledgerNum);
    Operation uploadOp = createUploadWasmOperation(wasmSize);
    LedgerKey contractCodeLedgerKey;
    contractCodeLedgerKey.type(CONTRACT_CODE);
    contractCodeLedgerKey.contractCode().hash =
        sha256(uploadOp.body.invokeHostFunctionOp().hostFunction.wasm());
    resources.footprint.readWrite.push_back(contractCodeLedgerKey);

    int64_t resourceFee = sorobanResourceFee(
        mApp, resources, 5000 + static_cast<size_t>(wasmSize), 100);
    // Roughly cover the rent fee.
    resourceFee += 100000;
    auto tx = sorobanTransactionFrameFromOps(mApp.getNetworkID(), *account,
                                             {uploadOp}, {}, resources,
                                             inclusionFee, resourceFee);
    return std::make_pair(account, tx);
}

std::pair<SorobanResources, uint32_t>
LoadGenerator::sorobanRandomUploadResources()
{
    auto const& cfg = mApp.getConfig();
    SorobanResources resources{};

    // Sample a random Wasm size
    uint32_t wasmSize = sampleDiscrete(
        cfg.LOADGEN_WASM_BYTES_FOR_TESTING,
        cfg.LOADGEN_WASM_BYTES_DISTRIBUTION_FOR_TESTING, DEFAULT_WASM_BYTES);

    // Estimate VM instantiation cost, with some additional buffer to increase
    // the chance that this instruction count is sufficient.
    ContractCostParamEntry const& vmInstantiationCosts =
        mApp.getLedgerManager()
            .getSorobanNetworkConfig()
            .cpuCostParams()[VmInstantiation];
    // Amount to right shift `vmInstantiationCosts.linearTerm * wasmSize` by
    uint32_t constexpr vmShiftTerm = 7;
    // Additional buffer per byte to increase the chance that this instruction
    // count is sufficient
    uint32_t constexpr vmBufferPerByte = 100;
    // Perform multiplication as int64_t to avoid overflow
    uint32_t linearResult = static_cast<uint32_t>(
        (vmInstantiationCosts.linearTerm * static_cast<int64_t>(wasmSize)) >>
        vmShiftTerm);
    resources.instructions = vmInstantiationCosts.constTerm + linearResult +
                             (vmBufferPerByte * wasmSize);

    // Double instruction estimate because wasm parse price is charged twice (as
    // of protocol 21).
    resources.instructions *= 2;

    // Allocate enough write bytes to write the whole Wasm plus the 40 bytes of
    // the key with some additional buffer to increase the chance that this
    // write size is sufficient
    uint32_t constexpr keyOverhead = 40;
    uint32_t constexpr writeBuffer = 128;
    resources.writeBytes = wasmSize + keyOverhead + writeBuffer;

    return {resources, wasmSize};
}

std::pair<LoadGenerator::TestAccountPtr, TransactionFrameBaseConstPtr>
LoadGenerator::pretendTransaction(uint32_t numAccounts, uint32_t offset,
                                  uint32_t ledgerNum, uint64_t sourceAccount,
                                  uint32_t opCount,
                                  std::optional<uint32_t> maxGeneratedFeeRate)
{
    vector<Operation> ops;
    ops.reserve(opCount);
    auto acc = findAccount(sourceAccount, ledgerNum);
    for (uint32 i = 0; i < opCount; i++)
    {
        auto args = SetOptionsArguments{};

        // We make SetOptionsOps such that we end up
        // with a n-op transaction that is exactly 100n + 240 bytes.
        args.inflationDest = std::make_optional<AccountID>(acc->getPublicKey());
        args.homeDomain = std::make_optional<std::string>(std::string(16, '*'));
        if (i == 0)
        {
            // The first operation needs to be bigger to achieve
            // 100n + 240 bytes.
            args.homeDomain->append(std::string(8, '*'));
            args.signer = std::make_optional<Signer>(Signer{});
        }
        ops.push_back(txtest::setOptions(args));
    }
    return std::make_pair(acc,
                          createTransactionFrame(acc, ops, LoadGenMode::PRETEND,
                                                 maxGeneratedFeeRate));
}

std::pair<LoadGenerator::TestAccountPtr, TransactionFrameBaseConstPtr>
LoadGenerator::createMixedClassicSorobanTransaction(
    uint32_t ledgerNum, uint64_t sourceAccountId,
    GeneratedLoadConfig const& cfg)
{
    auto const& mixCfg = cfg.getMixClassicSorobanConfig();
    std::discrete_distribution<uint32_t> dist({mixCfg.payWeight,
                                               mixCfg.sorobanUploadWeight,
                                               mixCfg.sorobanInvokeWeight});
    switch (dist(gRandomEngine))
    {
    case 0:
    {
        // Create a payment transaction
        mLastMixedMode = LoadGenMode::PAY;
        return paymentTransaction(cfg.nAccounts, cfg.offset, ledgerNum,
                                  sourceAccountId, 1, cfg.maxGeneratedFeeRate);
    }
    case 1:
    {
        // Create a soroban upload transaction
        mLastMixedMode = LoadGenMode::SOROBAN_UPLOAD;
        return sorobanRandomWasmTransaction(ledgerNum, sourceAccountId,
                                            generateFee(cfg.maxGeneratedFeeRate,
                                                        mApp,
                                                        /* opsCnt */ 1));
    }
    case 2:
    {
        // Create a soroban invoke transaction
        mLastMixedMode = LoadGenMode::SOROBAN_INVOKE;
        return invokeSorobanLoadTransaction(ledgerNum, sourceAccountId, cfg);
    }
    default:
        releaseAssert(false);
    }
}

void
LoadGenerator::maybeHandleFailedTx(TransactionFrameBaseConstPtr tx,
                                   TestAccountPtr sourceAccount,
                                   TransactionQueue::AddResultCode status,
                                   TransactionResultCode code)
{
    // Note that if transaction is a DUPLICATE, its sequence number is
    // incremented on the next call to execute.
    if (status == TransactionQueue::AddResultCode::ADD_STATUS_ERROR &&
        code == txBAD_SEQ)
    {
        auto txQueueSeqNum =
            tx->isSoroban()
                ? mApp.getHerder()
                      .getSorobanTransactionQueue()
                      .getInQueueSeqNum(sourceAccount->getPublicKey())
                : mApp.getHerder().getTransactionQueue().getInQueueSeqNum(
                      sourceAccount->getPublicKey());
        if (txQueueSeqNum)
        {
            sourceAccount->setSequenceNumber(*txQueueSeqNum);
            return;
        }
        if (!loadAccount(sourceAccount, mApp))
        {
            CLOG_ERROR(LoadGen, "Unable to reload account {}",
                       sourceAccount->getAccountId());
        }
    }
}

std::vector<LedgerKey>
LoadGenerator::checkSorobanStateSynced(Application& app,
                                       GeneratedLoadConfig const& cfg)
{
    // We don't care if TXs succeed for SOROBAN_UPLOAD
    if (!cfg.isSoroban() || cfg.mode == LoadGenMode::SOROBAN_UPLOAD)
    {
        return {};
    }

    std::vector<LedgerKey> result;
    LedgerSnapshot lsg(mApp);
    for (auto const& lk : mContractInstanceKeys)
    {
        if (!lsg.load(lk))
        {
            result.emplace_back(lk);
        }
    }

    if (mCodeKey && !lsg.load(*mCodeKey))
    {
        result.emplace_back(*mCodeKey);
    }

    return result;
}

std::vector<LoadGenerator::TestAccountPtr>
LoadGenerator::checkAccountSynced(Application& app, bool isCreate)
{
    std::vector<TestAccountPtr> result;
    for (auto const& acc : mAccounts)
    {
        TestAccountPtr account = acc.second;
        auto accountFromDB = *account;

        auto reloadRes = loadAccount(accountFromDB, app);
        // For account creation, reload accounts from the DB
        // For payments, ensure that the sequence number matches expected
        // seqnum. Timeout after 20 ledgers.
        if (isCreate)
        {
            if (!reloadRes)
            {
                CLOG_TRACE(LoadGen, "Account {} is not created yet!",
                           account->getAccountId());
                result.push_back(account);
            }
        }
        else if (!reloadRes)
        {
            auto msg =
                fmt::format("Account {} used to submit payment tx could not "
                            "load, DB might be in a corrupted state",
                            account->getAccountId());
            throw std::runtime_error(msg);
        }
        else if (account->getLastSequenceNumber() !=
                 accountFromDB.getLastSequenceNumber())
        {
            CLOG_TRACE(LoadGen,
                       "Account {} is at sequence num {}, but the DB is at  {}",
                       account->getAccountId(),
                       account->getLastSequenceNumber(),
                       accountFromDB.getLastSequenceNumber());
            result.push_back(account);
        }
    }
    return result;
}

bool
LoadGenerator::checkMinimumSorobanSuccess(GeneratedLoadConfig const& cfg)
{
    if (!cfg.isSoroban())
    {
        // Only applies to soroban modes
        return true;
    }

    int64_t nTxns =
        mApplySorobanSuccess.count() + mApplySorobanFailure.count() -
        mPreLoadgenApplySorobanSuccess - mPreLoadgenApplySorobanFailure;

    if (nTxns == 0)
    {
        // Special case to avoid division by zero
        return true;
    }

    int64_t nSuccessful =
        mApplySorobanSuccess.count() - mPreLoadgenApplySorobanSuccess;
    return (nSuccessful * 100) / nTxns >= cfg.getMinSorobanPercentSuccess();
}

void
LoadGenerator::waitTillComplete(GeneratedLoadConfig cfg)
{
    if (!mLoadTimer)
    {
        mLoadTimer = std::make_unique<VirtualTimer>(mApp.getClock());
    }
    vector<TestAccountPtr> inconsistencies;
    inconsistencies = checkAccountSynced(mApp, cfg.isCreate());
    auto sorobanInconsistencies = checkSorobanStateSynced(mApp, cfg);

    // If there are no inconsistencies and we have generated all load, finish
    if (inconsistencies.empty() && sorobanInconsistencies.empty() &&
        cfg.isDone())
    {
        // Check whether run met the minimum success rate for soroban invoke
        if (checkMinimumSorobanSuccess(cfg))
        {
            CLOG_INFO(LoadGen, "Load generation complete.");
            mLoadgenComplete.Mark();
            reset();
        }
        else
        {
            CLOG_INFO(LoadGen, "Load generation failed to meet minimum success "
                               "rate for soroban transactions.");
            // In this case the soroban setup phase executed successfully (as
            // indicated by the lack of entries in `sorobanInconsistencies`), so
            // the soroban persistent state does not need to be reset.
            emitFailure(false);
        }
        return;
    }
    // If we have an inconsistency, reset the timer and wait for another ledger
    else if (!inconsistencies.empty() || !sorobanInconsistencies.empty())
    {
        if (++mWaitTillCompleteForLedgers >= TIMEOUT_NUM_LEDGERS)
        {
            emitFailure(!sorobanInconsistencies.empty());
            return;
        }

        mLoadTimer->expires_from_now(
            mApp.getConfig().getExpectedLedgerCloseTime());
        mLoadTimer->async_wait([this, cfg]() { this->waitTillComplete(cfg); },
                               &VirtualTimer::onFailureNoop);
    }
    // If there are no inconsistencies but we aren't done yet, we have more load
    // to generate, so schedule loadgen
    else
    {
        // If there are no inconsistencies but we aren't done, we must be in a
        // two phase mode (soroban setup).
        releaseAssert(cfg.isSorobanSetup());

        // All Wasms should be deployed
        releaseAssert(cfg.getSorobanConfig().nWasms == 0);

        // 1 deploy TX per instance
        cfg.nTxs = cfg.getSorobanConfig().nInstances;
        scheduleLoadGeneration(cfg);
    }
}

void
LoadGenerator::emitFailure(bool resetSoroban)
{
    CLOG_INFO(LoadGen, "Load generation failed.");
    mLoadgenFail.Mark();
    reset();
    if (resetSoroban)
    {
        resetSorobanState();
    }
}

void
LoadGenerator::waitTillCompleteWithoutChecks()
{
    if (!mLoadTimer)
    {
        mLoadTimer = std::make_unique<VirtualTimer>(mApp.getClock());
    }
    if (++mWaitTillCompleteForLedgers == COMPLETION_TIMEOUT_WITHOUT_CHECKS)
    {
        auto inconsistencies = checkAccountSynced(mApp, /* isCreate */ false);
        CLOG_INFO(LoadGen, "Load generation complete.");
        if (!inconsistencies.empty())
        {
            CLOG_INFO(
                LoadGen,
                "{} account seq nums are not in sync with db; this is expected "
                "for high traffic due to tx queue limiter evictions.",
                inconsistencies.size());
        }
        mLoadgenComplete.Mark();
        reset();
        return;
    }
    mLoadTimer->expires_from_now(mApp.getConfig().getExpectedLedgerCloseTime());
    mLoadTimer->async_wait([this]() { this->waitTillCompleteWithoutChecks(); },
                           &VirtualTimer::onFailureNoop);
}

LoadGenerator::TxMetrics::TxMetrics(medida::MetricsRegistry& m)
    : mAccountCreated(m.NewMeter({"loadgen", "account", "created"}, "account"))
    , mNativePayment(m.NewMeter({"loadgen", "payment", "submitted"}, "op"))
    , mManageOfferOps(m.NewMeter({"loadgen", "manageoffer", "submitted"}, "op"))
    , mPretendOps(m.NewMeter({"loadgen", "pretend", "submitted"}, "op"))
    , mSorobanUploadTxs(m.NewMeter({"loadgen", "soroban", "upload"}, "txn"))
    , mSorobanSetupInvokeTxs(
          m.NewMeter({"loadgen", "soroban", "setup_invoke"}, "txn"))
    , mSorobanSetupUpgradeTxs(
          m.NewMeter({"loadgen", "soroban", "setup_upgrade"}, "txn"))
    , mSorobanInvokeTxs(m.NewMeter({"loadgen", "soroban", "invoke"}, "txn"))
    , mSorobanCreateUpgradeTxs(
          m.NewMeter({"loadgen", "soroban", "create_upgrade"}, "txn"))
    , mTxnAttempted(m.NewMeter({"loadgen", "txn", "attempted"}, "txn"))
    , mTxnRejected(m.NewMeter({"loadgen", "txn", "rejected"}, "txn"))
    , mTxnBytes(m.NewMeter({"loadgen", "txn", "bytes"}, "txn"))
{
}

void
LoadGenerator::TxMetrics::report()
{
    CLOG_DEBUG(LoadGen,
               "Counts: {} tx, {} rj, {} by, {} ac, {} na, {} pr, {} dex, {} "
               "su, {} ssi, {} ssu, {} si, {} scu",
               mTxnAttempted.count(), mTxnRejected.count(), mTxnBytes.count(),
               mAccountCreated.count(), mNativePayment.count(),
               mPretendOps.count(), mManageOfferOps.count(),
               mSorobanUploadTxs.count(), mSorobanSetupInvokeTxs.count(),
               mSorobanSetupUpgradeTxs.count(), mSorobanInvokeTxs.count(),
               mSorobanCreateUpgradeTxs.count());

    CLOG_DEBUG(LoadGen,
               "Rates/sec (1m EWMA): {} tx, {} rj, {} by, {} ac, {} na, {} pr, "
               "{} dex, {} su, {} ssi, {} ssu, {} si, {} scu",
               mTxnAttempted.one_minute_rate(), mTxnRejected.one_minute_rate(),
               mTxnBytes.one_minute_rate(), mAccountCreated.one_minute_rate(),
               mNativePayment.one_minute_rate(), mPretendOps.one_minute_rate(),
               mManageOfferOps.one_minute_rate(),
               mSorobanUploadTxs.one_minute_rate(),
               mSorobanSetupInvokeTxs.one_minute_rate(),
               mSorobanSetupUpgradeTxs.one_minute_rate(),
               mSorobanInvokeTxs.one_minute_rate(),
               mSorobanCreateUpgradeTxs.one_minute_rate());
}

TransactionFrameBaseConstPtr
LoadGenerator::createTransactionFrame(
    TestAccountPtr from, std::vector<Operation> ops, LoadGenMode mode,
    std::optional<uint32_t> maxGeneratedFeeRate)
{

    auto txf = transactionFromOperations(
        mApp, from->getSecretKey(), from->nextSequenceNumber(), ops,
        generateFee(maxGeneratedFeeRate, mApp, ops.size()));
    if (mode == LoadGenMode::PRETEND)
    {
        Memo memo(MEMO_TEXT);
        memo.text() = std::string(28, ' ');
        txbridge::setMemo(txf, memo);

        txbridge::setMinTime(txf, 0);
        txbridge::setMaxTime(txf, UINT64_MAX);
    }

    txbridge::getSignatures(txf).clear();
    txf->addSignature(from->getSecretKey());
    return txf;
}

TransactionQueue::AddResultCode
LoadGenerator::execute(TransactionFrameBasePtr txf, LoadGenMode mode,
                       TransactionResultCode& code)
{
    TxMetrics txm(mApp.getMetrics());

    // Record tx metrics.
    switch (mode)
    {
    case LoadGenMode::CREATE:
        txm.mAccountCreated.Mark(txf->getNumOperations());
        break;
    case LoadGenMode::PAY:
        txm.mNativePayment.Mark(txf->getNumOperations());
        break;
    case LoadGenMode::PRETEND:
        txm.mPretendOps.Mark(txf->getNumOperations());
        break;
    case LoadGenMode::MIXED_CLASSIC:
        if (txf->hasDexOperations())
        {
            txm.mManageOfferOps.Mark(txf->getNumOperations());
        }
        else
        {
            txm.mNativePayment.Mark(txf->getNumOperations());
        }
        break;
    case LoadGenMode::SOROBAN_UPLOAD:
        txm.mSorobanUploadTxs.Mark();
        break;
    case LoadGenMode::SOROBAN_INVOKE_SETUP:
        txm.mSorobanSetupInvokeTxs.Mark();
        break;
    case LoadGenMode::SOROBAN_UPGRADE_SETUP:
        txm.mSorobanSetupUpgradeTxs.Mark();
        break;
    case LoadGenMode::SOROBAN_INVOKE:
        txm.mSorobanInvokeTxs.Mark();
        break;
    case LoadGenMode::SOROBAN_CREATE_UPGRADE:
        txm.mSorobanCreateUpgradeTxs.Mark();
        break;
    case LoadGenMode::MIXED_CLASSIC_SOROBAN:
        switch (mLastMixedMode)
        {
        case LoadGenMode::PAY:
            txm.mNativePayment.Mark(txf->getNumOperations());
            break;
        case LoadGenMode::SOROBAN_UPLOAD:
            txm.mSorobanUploadTxs.Mark();
            break;
        case LoadGenMode::SOROBAN_INVOKE:
            txm.mSorobanInvokeTxs.Mark();
            break;
        default:
            releaseAssert(false);
        }
        break;
    }

    txm.mTxnAttempted.Mark();

    auto msg = txf->toStellarMessage();
    txm.mTxnBytes.Mark(xdr::xdr_argpack_size(*msg));

    auto addResult = mApp.getHerder().recvTransaction(txf, true);
    if (addResult.code != TransactionQueue::AddResultCode::ADD_STATUS_PENDING)
    {

        auto resultStr =
            addResult.txResult
                ? xdrToCerealString(addResult.txResult->getResult(),
                                    "TransactionResult")
                : "";
        CLOG_INFO(LoadGen, "tx rejected '{}': ===> {}, {}",
                  TX_STATUS_STRING[static_cast<int>(addResult.code)],
                  txf->isSoroban() ? "soroban"
                                   : xdrToCerealString(txf->getEnvelope(),
                                                       "TransactionEnvelope"),
                  resultStr);
        if (addResult.code == TransactionQueue::AddResultCode::ADD_STATUS_ERROR)
        {
            releaseAssert(addResult.txResult);
            code = addResult.txResult->getResultCode();
        }
        txm.mTxnRejected.Mark();
    }
    else
    {
        mApp.getOverlayManager().broadcastMessage(msg, txf->getFullHash());
    }

    return addResult.code;
}

void
GeneratedLoadConfig::copySorobanNetworkConfigToUpgradeConfig(
    SorobanNetworkConfig const& cfg)
{
    releaseAssert(mode == LoadGenMode::SOROBAN_CREATE_UPGRADE);
    auto& upgradeCfg = getMutSorobanUpgradeConfig();

    upgradeCfg.maxContractSizeBytes = cfg.maxContractSizeBytes();
    upgradeCfg.maxContractDataKeySizeBytes = cfg.maxContractDataKeySizeBytes();
    upgradeCfg.maxContractDataEntrySizeBytes =
        cfg.maxContractDataEntrySizeBytes();

    upgradeCfg.ledgerMaxInstructions = cfg.ledgerMaxInstructions();
    upgradeCfg.txMaxInstructions = cfg.txMaxInstructions();
    upgradeCfg.txMemoryLimit = cfg.txMemoryLimit();

    upgradeCfg.ledgerMaxReadLedgerEntries = cfg.ledgerMaxReadLedgerEntries();
    upgradeCfg.ledgerMaxReadBytes = cfg.ledgerMaxReadBytes();
    upgradeCfg.ledgerMaxWriteLedgerEntries = cfg.ledgerMaxWriteLedgerEntries();
    upgradeCfg.ledgerMaxWriteBytes = cfg.ledgerMaxWriteBytes();
    upgradeCfg.ledgerMaxTxCount = cfg.ledgerMaxTxCount();
    upgradeCfg.txMaxReadLedgerEntries = cfg.txMaxReadLedgerEntries();
    upgradeCfg.txMaxReadBytes = cfg.txMaxReadBytes();
    upgradeCfg.txMaxWriteLedgerEntries = cfg.txMaxWriteLedgerEntries();
    upgradeCfg.txMaxWriteBytes = cfg.txMaxWriteBytes();

    upgradeCfg.txMaxContractEventsSizeBytes =
        cfg.txMaxContractEventsSizeBytes();

    upgradeCfg.ledgerMaxTransactionsSizeBytes =
        cfg.ledgerMaxTransactionSizesBytes();
    upgradeCfg.txMaxSizeBytes = cfg.txMaxSizeBytes();

    upgradeCfg.maxEntryTTL = cfg.stateArchivalSettings().maxEntryTTL;
    upgradeCfg.minTemporaryTTL = cfg.stateArchivalSettings().minTemporaryTTL;
    upgradeCfg.minPersistentTTL = cfg.stateArchivalSettings().minPersistentTTL;
    upgradeCfg.persistentRentRateDenominator =
        cfg.stateArchivalSettings().persistentRentRateDenominator;
    upgradeCfg.tempRentRateDenominator =
        cfg.stateArchivalSettings().tempRentRateDenominator;
    upgradeCfg.maxEntriesToArchive =
        cfg.stateArchivalSettings().maxEntriesToArchive;
    upgradeCfg.bucketListSizeWindowSampleSize =
        cfg.stateArchivalSettings().bucketListSizeWindowSampleSize;
    upgradeCfg.bucketListWindowSamplePeriod =
        cfg.stateArchivalSettings().bucketListWindowSamplePeriod;
    upgradeCfg.evictionScanSize = cfg.stateArchivalSettings().evictionScanSize;
    upgradeCfg.startingEvictionScanLevel =
        cfg.stateArchivalSettings().startingEvictionScanLevel;
}

GeneratedLoadConfig
GeneratedLoadConfig::createAccountsLoad(uint32_t nAccounts, uint32_t txRate)
{
    GeneratedLoadConfig cfg;
    cfg.mode = LoadGenMode::CREATE;
    cfg.nAccounts = nAccounts;
    cfg.txRate = txRate;
    return cfg;
}

GeneratedLoadConfig
GeneratedLoadConfig::createSorobanInvokeSetupLoad(uint32_t nAccounts,
                                                  uint32_t nInstances,
                                                  uint32_t txRate)
{
    GeneratedLoadConfig cfg;
    cfg.mode = LoadGenMode::SOROBAN_INVOKE_SETUP;
    cfg.nAccounts = nAccounts;
    cfg.getMutSorobanConfig().nInstances = nInstances;
    cfg.txRate = txRate;
    return cfg;
}

GeneratedLoadConfig
GeneratedLoadConfig::createSorobanUpgradeSetupLoad()
{
    GeneratedLoadConfig cfg;
    cfg.mode = LoadGenMode::SOROBAN_UPGRADE_SETUP;
    cfg.nAccounts = 1;
    cfg.getMutSorobanConfig().nInstances = 1;
    cfg.txRate = 1;
    return cfg;
}

GeneratedLoadConfig
GeneratedLoadConfig::txLoad(LoadGenMode mode, uint32_t nAccounts, uint32_t nTxs,
                            uint32_t txRate, uint32_t offset,
                            std::optional<uint32_t> maxFee)
{
    GeneratedLoadConfig cfg;
    cfg.mode = mode;
    cfg.nAccounts = nAccounts;
    cfg.nTxs = nTxs;
    cfg.txRate = txRate;
    cfg.offset = offset;
    cfg.maxGeneratedFeeRate = maxFee;
    return cfg;
}

GeneratedLoadConfig::SorobanConfig&
GeneratedLoadConfig::getMutSorobanConfig()
{
    releaseAssert(isSoroban() && mode != LoadGenMode::SOROBAN_UPLOAD);
    return sorobanConfig;
}

GeneratedLoadConfig::SorobanConfig const&
GeneratedLoadConfig::getSorobanConfig() const
{
    releaseAssert(isSoroban() && mode != LoadGenMode::SOROBAN_UPLOAD);
    return sorobanConfig;
}

GeneratedLoadConfig::SorobanUpgradeConfig&
GeneratedLoadConfig::getMutSorobanUpgradeConfig()
{
    releaseAssert(mode == LoadGenMode::SOROBAN_CREATE_UPGRADE);
    return sorobanUpgradeConfig;
}

GeneratedLoadConfig::SorobanUpgradeConfig const&
GeneratedLoadConfig::getSorobanUpgradeConfig() const
{
    releaseAssert(mode == LoadGenMode::SOROBAN_CREATE_UPGRADE);
    return sorobanUpgradeConfig;
}

GeneratedLoadConfig::MixClassicSorobanConfig&
GeneratedLoadConfig::getMutMixClassicSorobanConfig()
{
    releaseAssert(mode == LoadGenMode::MIXED_CLASSIC_SOROBAN);
    return mixClassicSorobanConfig;
}

GeneratedLoadConfig::MixClassicSorobanConfig const&
GeneratedLoadConfig::getMixClassicSorobanConfig() const
{
    releaseAssert(mode == LoadGenMode::MIXED_CLASSIC_SOROBAN);
    return mixClassicSorobanConfig;
}

uint32_t&
GeneratedLoadConfig::getMutDexTxPercent()
{
    releaseAssert(mode == LoadGenMode::MIXED_CLASSIC);
    return dexTxPercent;
}

uint32_t const&
GeneratedLoadConfig::getDexTxPercent() const
{
    releaseAssert(mode == LoadGenMode::MIXED_CLASSIC);
    return dexTxPercent;
}

uint32_t
GeneratedLoadConfig::getMinSorobanPercentSuccess() const
{
    releaseAssert(isSoroban());
    return mMinSorobanPercentSuccess;
}

void
GeneratedLoadConfig::setMinSorobanPercentSuccess(uint32_t percent)
{
    releaseAssert(isSoroban());
    if (percent > 100)
    {
        throw std::invalid_argument("percent must be <= 100");
    }
    mMinSorobanPercentSuccess = percent;
}

bool
GeneratedLoadConfig::isCreate() const
{
    return mode == LoadGenMode::CREATE;
}

bool
GeneratedLoadConfig::isSoroban() const
{
    return mode == LoadGenMode::SOROBAN_INVOKE ||
           mode == LoadGenMode::SOROBAN_INVOKE_SETUP ||
           mode == LoadGenMode::SOROBAN_UPLOAD ||
           mode == LoadGenMode::SOROBAN_UPGRADE_SETUP ||
           mode == LoadGenMode::SOROBAN_CREATE_UPGRADE ||
           mode == LoadGenMode::MIXED_CLASSIC_SOROBAN;
}

bool
GeneratedLoadConfig::isSorobanSetup() const
{
    return mode == LoadGenMode::SOROBAN_INVOKE_SETUP ||
           mode == LoadGenMode::SOROBAN_UPGRADE_SETUP;
}

bool
GeneratedLoadConfig::isLoad() const
{
    return mode == LoadGenMode::PAY || mode == LoadGenMode::PRETEND ||
           mode == LoadGenMode::MIXED_CLASSIC ||
           mode == LoadGenMode::SOROBAN_UPLOAD ||
           mode == LoadGenMode::SOROBAN_INVOKE ||
           mode == LoadGenMode::SOROBAN_CREATE_UPGRADE ||
           mode == LoadGenMode::MIXED_CLASSIC_SOROBAN;
}

bool
GeneratedLoadConfig::modeInvokes() const
{
    return mode == LoadGenMode::SOROBAN_INVOKE ||
           mode == LoadGenMode::MIXED_CLASSIC_SOROBAN;
}

bool
GeneratedLoadConfig::modeSetsUpInvoke() const
{
    return mode == LoadGenMode::SOROBAN_INVOKE_SETUP;
}

bool
GeneratedLoadConfig::modeUploads() const
{
    return mode == LoadGenMode::SOROBAN_UPLOAD ||
           mode == LoadGenMode::MIXED_CLASSIC_SOROBAN;
}
}
