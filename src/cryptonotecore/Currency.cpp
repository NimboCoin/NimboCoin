// Copyright (c) 2012-2017, The CryptoNote developers, The Bytecoin developers
// Copyright (c) 2014-2018, The Monero Project
// Copyright (c) 2018-2019, The TurtleCoin Developers
//
// Please see the included LICENSE file for more information.

/////////////////////
#include "Currency.h"
/////////////////////

#include <cctype>
#include <common/Base58.h>
#include <common/CheckDifficulty.h>
#include <common/CryptoNoteTools.h>
#include <common/StringTools.h>
#include <common/TransactionExtra.h>
#include <common/int-util.h>
#include <config/Constants.h>
#include <cryptonotecore/CryptoNoteBasicImpl.h>
#include <cryptonotecore/CryptoNoteFormatUtils.h>
#include <cryptonotecore/Difficulty.h>
#include <cryptonotecore/UpgradeDetector.h>
#include <utilities/Addresses.h>
#include <utilities/String.h>

#undef ERROR

using namespace Logging;
using namespace Common;

namespace CryptoNote
{
    bool Currency::init()
    {
        if (!generateGenesisBlock())
        {
            logger(ERROR, BRIGHT_RED) << "Failed to generate genesis block";
            return false;
        }

        try
        {
            cachedGenesisBlock->getBlockHash();
        }
        catch (std::exception &e)
        {
            logger(ERROR, BRIGHT_RED) << "Failed to get genesis block hash: " << e.what();
            return false;
        }

        return true;
    }

    bool Currency::generateGenesisBlock()
    {
        genesisBlockTemplate = BlockTemplate{};

        std::string genesisCoinbaseTxHex = CryptoNote::parameters::GENESIS_COINBASE_TX_HEX;
        BinaryArray minerTxBlob;

        bool r = fromHex(genesisCoinbaseTxHex, minerTxBlob)
                 && fromBinaryArray(genesisBlockTemplate.baseTransaction, minerTxBlob);

        if (!r)
        {
            logger(ERROR, BRIGHT_RED) << "failed to parse coinbase tx from hard coded blob";
            return false;
        }

        genesisBlockTemplate.majorVersion = BLOCK_MAJOR_VERSION_1;
        genesisBlockTemplate.minorVersion = BLOCK_MINOR_VERSION_0;
        genesisBlockTemplate.timestamp = 0;
        genesisBlockTemplate.nonce = 70;

        // miner::find_nonce_for_given_block(bl, 1, 0);
        cachedGenesisBlock.reset(new CachedBlock(genesisBlockTemplate));
        return true;
    }

    size_t Currency::blockGrantedFullRewardZoneByBlockVersion(uint8_t blockMajorVersion) const
    {
        if (blockMajorVersion >= BLOCK_MAJOR_VERSION_3)
        {
            return m_blockGrantedFullRewardZone;
        }
        else if (blockMajorVersion == BLOCK_MAJOR_VERSION_2)
        {
            return CryptoNote::parameters::CRYPTONOTE_BLOCK_GRANTED_FULL_REWARD_ZONE_V2;
        }
        else
        {
            return CryptoNote::parameters::CRYPTONOTE_BLOCK_GRANTED_FULL_REWARD_ZONE_V1;
        }
    }

    uint32_t Currency::upgradeHeight(uint8_t majorVersion) const
    {
        if (majorVersion == BLOCK_MAJOR_VERSION_2)
        {
            return m_upgradeHeightV2;
        }
        else if (majorVersion == BLOCK_MAJOR_VERSION_3)
        {
            return m_upgradeHeightV3;
        }
        else if (majorVersion == BLOCK_MAJOR_VERSION_4)
        {
            return m_upgradeHeightV4;
        }
        else if (majorVersion == BLOCK_MAJOR_VERSION_5)
        {
            return m_upgradeHeightV5;
        }
	else if (majorVersion == BLOCK_MAJOR_VERSION_6)
        {
            return m_upgradeHeightV6;
        }
        else
        {
            return static_cast<uint32_t>(-1);
        }
    }

    bool Currency::getBlockReward(
        uint8_t blockMajorVersion,
        size_t medianSize,
        size_t currentBlockSize,
        uint64_t alreadyGeneratedCoins,
        uint64_t fee,
        uint64_t blockHeight,
        uint64_t &reward,
        int64_t &emissionChange) const
    {
        assert(alreadyGeneratedCoins <= m_moneySupply);
        uint32_t emission;

        if (blockHeight >= CryptoNote::parameters::EMISSION_SPEED_FACTOR_V2_HEIGHT)
        {
            emission = CryptoNote::parameters::EMISSION_SPEED_FACTOR_V2;
        }
        else
        {
            emission = CryptoNote::parameters::EMISSION_SPEED_FACTOR;
        }

        assert(emission > 0 && emission <= 8 * sizeof(uint64_t));

        uint64_t baseReward = (m_moneySupply - alreadyGeneratedCoins) >> emission;

        size_t blockGrantedFullRewardZone = blockGrantedFullRewardZoneByBlockVersion(blockMajorVersion);
        medianSize = std::max(medianSize, blockGrantedFullRewardZone);
        if (currentBlockSize > UINT64_C(2) * medianSize)
        {
            logger(TRACE) << "Block cumulative size is too big: " << currentBlockSize << ", expected less than "
                          << 2 * medianSize;
            return false;
        }

        uint64_t penalizedBaseReward = getPenalizedAmount(baseReward, medianSize, currentBlockSize);
        uint64_t penalizedFee =
            blockMajorVersion >= BLOCK_MAJOR_VERSION_2 ? getPenalizedAmount(fee, medianSize, currentBlockSize) : fee;

        emissionChange = penalizedBaseReward - (fee - penalizedFee);
        reward = penalizedBaseReward + penalizedFee;

        return true;
    }

    size_t Currency::maxBlockCumulativeSize(uint64_t height) const
    {
        assert(height <= std::numeric_limits<uint64_t>::max() / m_maxBlockSizeGrowthSpeedNumerator);
        size_t maxSize = static_cast<size_t>(
            m_maxBlockSizeInitial
            + (height * m_maxBlockSizeGrowthSpeedNumerator) / m_maxBlockSizeGrowthSpeedDenominator);
        assert(maxSize >= m_maxBlockSizeInitial);
        return maxSize;
    }

    bool Currency::constructMinerTx(
        uint8_t blockMajorVersion,
        uint32_t height,
        size_t medianSize,
        uint64_t alreadyGeneratedCoins,
        size_t currentBlockSize,
        uint64_t fee,
        const Crypto::PublicKey &publicViewKey,
        const Crypto::PublicKey &publicSpendKey,
        Transaction &tx,
        const BinaryArray &extraNonce /* = BinaryArray()*/,
        size_t maxOuts /* = 1*/) const
    {
        tx.inputs.clear();
        tx.outputs.clear();
        tx.extra.clear();

        KeyPair txkey = generateKeyPair();
        addTransactionPublicKeyToExtra(tx.extra, txkey.publicKey);
        if (!extraNonce.empty())
        {
            if (!addExtraNonceToTransactionExtra(tx.extra, extraNonce))
            {
                return false;
            }
        }

        BaseInput in;
        in.blockIndex = height;

        uint64_t blockReward;
        int64_t emissionChange;
        if (!getBlockReward(
                blockMajorVersion,
                medianSize,
                currentBlockSize,
                alreadyGeneratedCoins,
                fee,
                height,
                blockReward,
                emissionChange))
        {
            logger(INFO) << "Block is too big";
            return false;
        }

        std::vector<uint64_t> outAmounts;
        decompose_amount_into_digits(
            blockReward,
            defaultDustThreshold(height),
            [&outAmounts](uint64_t a_chunk) { outAmounts.push_back(a_chunk); },
            [&outAmounts](uint64_t a_dust) { outAmounts.push_back(a_dust); });

        if (!(1 <= maxOuts))
        {
            logger(ERROR, BRIGHT_RED) << "max_out must be non-zero";
            return false;
        }
        while (maxOuts < outAmounts.size())
        {
            outAmounts[outAmounts.size() - 2] += outAmounts.back();
            outAmounts.resize(outAmounts.size() - 1);
        }

        uint64_t summaryAmounts = 0;
        for (size_t no = 0; no < outAmounts.size(); no++)
        {
            Crypto::KeyDerivation derivation;
            Crypto::PublicKey outEphemeralPubKey;

            bool r = Crypto::generate_key_derivation(publicViewKey, txkey.secretKey, derivation);

            if (!(r))
            {
                logger(ERROR, BRIGHT_RED) << "while creating outs: failed to generate_key_derivation("
                                          << publicViewKey << ", " << txkey.secretKey << ")";
                return false;
            }

            r = Crypto::derive_public_key(derivation, no, publicSpendKey, outEphemeralPubKey);

            if (!(r))
            {
                logger(ERROR, BRIGHT_RED) << "while creating outs: failed to derive_public_key(" << derivation << ", "
                                          << no << ", " << publicSpendKey << ")";
                return false;
            }

            KeyOutput tk;
            tk.key = outEphemeralPubKey;

            TransactionOutput out;
            summaryAmounts += out.amount = outAmounts[no];
            out.target = tk;
            tx.outputs.push_back(out);
        }

        if (!(summaryAmounts == blockReward))
        {
            logger(ERROR, BRIGHT_RED) << "Failed to construct miner tx, summaryAmounts = " << summaryAmounts
                                      << " not equal blockReward = " << blockReward;
            return false;
        }

        tx.version = CURRENT_TRANSACTION_VERSION;

        uint64_t unlockTime = height + CryptoNote::parameters::CRYPTONOTE_MINED_MONEY_UNLOCK_WINDOW;

        if (height >= CryptoNote::parameters::CRYPTONOTE_MINED_MONEY_UNLOCK_WINDOW_V2_HEIGHT)
        {
            unlockTime = height + CryptoNote::parameters::CRYPTONOTE_MINED_MONEY_UNLOCK_WINDOW_V2;
        }

        /* Unspendable until current height + mined money unlock */
        tx.unlockTime = unlockTime;
        tx.inputs.push_back(in);
        return true;
    }

    bool Currency::isFusionTransaction(
        const std::vector<uint64_t> &inputsAmounts,
        const std::vector<uint64_t> &outputsAmounts,
        size_t size,
        uint32_t height) const
    {
        if (size > fusionTxMaxSize())
        {
            return false;
        }

        if (inputsAmounts.size() < fusionTxMinInputCount())
        {
            return false;
        }

        if (inputsAmounts.size() < outputsAmounts.size() * fusionTxMinInOutCountRatio())
        {
            return false;
        }

        uint64_t inputAmount = 0;
        for (auto amount : inputsAmounts)
        {
            if (amount < defaultFusionDustThreshold(height))    
            {
                return false;
            }

            inputAmount += amount;
        }

        if (height >= CryptoNote::parameters::FUSION_FEE_V1_HEIGHT)
        {
            inputAmount -= CryptoNote::parameters::FUSION_FEE_V1;
        }

        std::vector<uint64_t> expectedOutputsAmounts;
        expectedOutputsAmounts.reserve(outputsAmounts.size());
        decomposeAmount(inputAmount, defaultFusionDustThreshold(height), expectedOutputsAmounts);
        std::sort(expectedOutputsAmounts.begin(), expectedOutputsAmounts.end());

        return expectedOutputsAmounts == outputsAmounts;
    }

    bool Currency::isFusionTransaction(const Transaction &transaction, size_t size, uint32_t height) const
    {
        assert(getObjectBinarySize(transaction) == size);

        std::vector<uint64_t> outputsAmounts;
        outputsAmounts.reserve(transaction.outputs.size());
        for (const TransactionOutput &output : transaction.outputs)
        {
            outputsAmounts.push_back(output.amount);
        }

        return isFusionTransaction(getInputsAmounts(transaction), outputsAmounts, size, height);
    }

    bool Currency::isFusionTransaction(const Transaction &transaction, uint32_t height) const
    {
        return isFusionTransaction(transaction, getObjectBinarySize(transaction), height);
    }

    bool
        Currency::isAmountApplicableInFusionTransactionInput(uint64_t amount, uint64_t threshold, uint32_t height) const
    {
        uint8_t ignore;
        return isAmountApplicableInFusionTransactionInput(amount, threshold, ignore, height);
    }

    bool Currency::isAmountApplicableInFusionTransactionInput(
        uint64_t amount,
        uint64_t threshold,
        uint8_t &amountPowerOfTen,
        uint32_t height) const
    {
        if (amount >= threshold)
        {
            return false;
        }

        if (amount < defaultFusionDustThreshold(height))
        {
            return false;
        }

        auto it = std::lower_bound(Constants::PRETTY_AMOUNTS.begin(), Constants::PRETTY_AMOUNTS.end(), amount);
        if (it == Constants::PRETTY_AMOUNTS.end() || amount != *it)
        {
            return false;
        }

        amountPowerOfTen = static_cast<uint8_t>(std::distance(Constants::PRETTY_AMOUNTS.begin(), it) / 9);
        return true;
    }

    std::string Currency::accountAddressAsString(const AccountPublicAddress &accountPublicAddress) const
    {
        return Utilities::getAccountAddressAsStr(m_publicAddressBase58Prefix, accountPublicAddress);
    }

    bool Currency::parseAccountAddressString(const std::string &str, AccountPublicAddress &addr) const
    {
        uint64_t prefix;
        if (!Utilities::parseAccountAddressString(prefix, addr, str))
        {
            return false;
        }

        if (prefix != m_publicAddressBase58Prefix)
        {
            logger(DEBUGGING) << "Wrong address prefix: " << prefix << ", expected " << m_publicAddressBase58Prefix;
            return false;
        }

        return true;
    }

    std::string Currency::formatAmount(uint64_t amount) const
    {
        std::string s = std::to_string(amount);
        if (s.size() < m_numberOfDecimalPlaces + 1)
        {
            s.insert(0, m_numberOfDecimalPlaces + 1 - s.size(), '0');
        }
        s.insert(s.size() - m_numberOfDecimalPlaces, ".");
        return s;
    }

    std::string Currency::formatAmount(int64_t amount) const
    {
        std::string s = formatAmount(static_cast<uint64_t>(std::abs(amount)));

        if (amount < 0)
        {
            s.insert(0, "-");
        }

        return s;
    }

    bool Currency::parseAmount(const std::string &str, uint64_t &amount) const
    {
        std::string strAmount = str;
        Utilities::trim(strAmount);

        size_t pointIndex = strAmount.find_first_of('.');
        size_t fractionSize;
        if (std::string::npos != pointIndex)
        {
            fractionSize = strAmount.size() - pointIndex - 1;
            while (m_numberOfDecimalPlaces < fractionSize && '0' == strAmount.back())
            {
                strAmount.erase(strAmount.size() - 1, 1);
                --fractionSize;
            }
            if (m_numberOfDecimalPlaces < fractionSize)
            {
                return false;
            }
            strAmount.erase(pointIndex, 1);
        }
        else
        {
            fractionSize = 0;
        }

        if (strAmount.empty())
        {
            return false;
        }

        if (!std::all_of(strAmount.begin(), strAmount.end(), ::isdigit))
        {
            return false;
        }

        if (fractionSize < m_numberOfDecimalPlaces)
        {
            strAmount.append(m_numberOfDecimalPlaces - fractionSize, '0');
        }

        return Common::fromString(strAmount, amount);
    }

    uint64_t Currency::getNextDifficulty(uint8_t version, uint32_t blockIndex, std::vector<uint64_t> timestamps, std::vector<uint64_t> cumulativeDifficulties) const
    {
        return nextDifficulty(timestamps, cumulativeDifficulties, blockIndex);
    }

    bool Currency::checkProofOfWorkV1(const CachedBlock &block, uint64_t currentDifficulty) const
    {
        if (BLOCK_MAJOR_VERSION_1 != block.getBlock().majorVersion)
        {
            return false;
        }

        return check_hash(block.getBlockLongHash(), currentDifficulty);
    }

    bool Currency::checkProofOfWorkV2(const CachedBlock &cachedBlock, uint64_t currentDifficulty) const
    {
        const auto &block = cachedBlock.getBlock();
        if (block.majorVersion < BLOCK_MAJOR_VERSION_2)
        {
            return false;
        }

        if (!check_hash(cachedBlock.getBlockLongHash(), currentDifficulty))
        {
            return false;
        }

        TransactionExtraMergeMiningTag mmTag;
        if (!getMergeMiningTagFromExtra(block.parentBlock.baseTransaction.extra, mmTag))
        {
            logger(ERROR) << "merge mining tag wasn't found in extra of the parent block miner transaction";
            return false;
        }

        if (8 * sizeof(cachedGenesisBlock->getBlockHash()) < block.parentBlock.blockchainBranch.size())
        {
            return false;
        }

        Crypto::Hash auxBlocksMerkleRoot;
        Crypto::tree_hash_from_branch(
            block.parentBlock.blockchainBranch.data(),
            block.parentBlock.blockchainBranch.size(),
            cachedBlock.getAuxiliaryBlockHeaderHash(),
            &cachedGenesisBlock->getBlockHash(),
            auxBlocksMerkleRoot);

        if (auxBlocksMerkleRoot != mmTag.merkleRoot)
        {
            logger(ERROR, BRIGHT_YELLOW) << "Aux block hash wasn't found in merkle tree";
            return false;
        }

        return true;
    }

    bool Currency::checkProofOfWork(const CachedBlock &block, uint64_t currentDiffic) const
    {
        switch (block.getBlock().majorVersion)
        {
            case BLOCK_MAJOR_VERSION_1:
            {
                return checkProofOfWorkV1(block, currentDiffic);
            }
            default:
            {
                return checkProofOfWorkV2(block, currentDiffic);
            }
        }

        logger(ERROR, BRIGHT_RED) << "Unknown block major version: " << block.getBlock().majorVersion << "."
                                  << block.getBlock().minorVersion;
        return false;
    }

    Currency::Currency(Currency &&currency):
        m_maxBlockHeight(currency.m_maxBlockHeight),
        m_maxBlockBlobSize(currency.m_maxBlockBlobSize),
        m_maxTxSize(currency.m_maxTxSize),
        m_publicAddressBase58Prefix(currency.m_publicAddressBase58Prefix),
        m_timestampCheckWindow(currency.m_timestampCheckWindow),
        m_moneySupply(currency.m_moneySupply),
        m_rewardBlocksWindow(currency.m_rewardBlocksWindow),
        m_blockGrantedFullRewardZone(currency.m_blockGrantedFullRewardZone),
        m_isBlockexplorer(currency.m_isBlockexplorer),
        m_minerTxBlobReservedSize(currency.m_minerTxBlobReservedSize),
        m_numberOfDecimalPlaces(currency.m_numberOfDecimalPlaces),
        m_coin(currency.m_coin),
        m_mininumFee(currency.m_mininumFee),
        m_defaultDustThreshold(currency.m_defaultDustThreshold),
        m_difficultyWindow(currency.m_difficultyWindow),
        m_difficultyCut(currency.m_difficultyCut),
        m_maxBlockSizeInitial(currency.m_maxBlockSizeInitial),
        m_maxBlockSizeGrowthSpeedNumerator(currency.m_maxBlockSizeGrowthSpeedNumerator),
        m_maxBlockSizeGrowthSpeedDenominator(currency.m_maxBlockSizeGrowthSpeedDenominator),
        m_lockedTxAllowedDeltaSeconds(currency.m_lockedTxAllowedDeltaSeconds),
        m_lockedTxAllowedDeltaBlocks(currency.m_lockedTxAllowedDeltaBlocks),
        m_mempoolTxLiveTime(currency.m_mempoolTxLiveTime),
        m_numberOfPeriodsToForgetTxDeletedFromPool(currency.m_numberOfPeriodsToForgetTxDeletedFromPool),
        m_fusionTxMaxSize(currency.m_fusionTxMaxSize),
        m_fusionTxMinInputCount(currency.m_fusionTxMinInputCount),
        m_fusionTxMinInOutCountRatio(currency.m_fusionTxMinInOutCountRatio),
        m_upgradeHeightV2(currency.m_upgradeHeightV2),
        m_upgradeHeightV3(currency.m_upgradeHeightV3),
        m_upgradeHeightV4(currency.m_upgradeHeightV4),
        m_upgradeHeightV5(currency.m_upgradeHeightV5),
	m_upgradeHeightV6(currency.m_upgradeHeightV6),
        m_upgradeVotingThreshold(currency.m_upgradeVotingThreshold),
        m_upgradeVotingWindow(currency.m_upgradeVotingWindow),
        m_upgradeWindow(currency.m_upgradeWindow),
        m_blocksFileName(currency.m_blocksFileName),
        m_blockIndexesFileName(currency.m_blockIndexesFileName),
        m_txPoolFileName(currency.m_txPoolFileName),
        genesisBlockTemplate(std::move(currency.genesisBlockTemplate)),
        cachedGenesisBlock(new CachedBlock(genesisBlockTemplate)),
        logger(currency.logger)
    {
    }

    CurrencyBuilder::CurrencyBuilder(std::shared_ptr<Logging::ILogger> log): m_currency(log)
    {
        maxBlockNumber(parameters::CRYPTONOTE_MAX_BLOCK_NUMBER);
        maxBlockBlobSize(parameters::CRYPTONOTE_MAX_BLOCK_BLOB_SIZE);
        maxTxSize(parameters::CRYPTONOTE_MAX_TX_SIZE);
        publicAddressBase58Prefix(parameters::CRYPTONOTE_PUBLIC_ADDRESS_BASE58_PREFIX);

        timestampCheckWindow(parameters::BLOCKCHAIN_TIMESTAMP_CHECK_WINDOW);

        moneySupply(parameters::MONEY_SUPPLY);

        rewardBlocksWindow(parameters::CRYPTONOTE_REWARD_BLOCKS_WINDOW);
        blockGrantedFullRewardZone(parameters::CRYPTONOTE_BLOCK_GRANTED_FULL_REWARD_ZONE);
        minerTxBlobReservedSize(parameters::CRYPTONOTE_COINBASE_BLOB_RESERVED_SIZE);

        numberOfDecimalPlaces(parameters::CRYPTONOTE_DISPLAY_DECIMAL_POINT);

        mininumFee(parameters::MINIMUM_FEE);
        defaultDustThreshold(parameters::DEFAULT_DUST_THRESHOLD);

        difficultyWindow(parameters::DIFFICULTY_WINDOW);

        maxBlockSizeInitial(parameters::MAX_BLOCK_SIZE_INITIAL);
        maxBlockSizeGrowthSpeedNumerator(parameters::MAX_BLOCK_SIZE_GROWTH_SPEED_NUMERATOR);
        maxBlockSizeGrowthSpeedDenominator(parameters::MAX_BLOCK_SIZE_GROWTH_SPEED_DENOMINATOR);

        lockedTxAllowedDeltaSeconds(parameters::CRYPTONOTE_LOCKED_TX_ALLOWED_DELTA_SECONDS);
        lockedTxAllowedDeltaBlocks(parameters::CRYPTONOTE_LOCKED_TX_ALLOWED_DELTA_BLOCKS);

        mempoolTxLiveTime(parameters::CRYPTONOTE_MEMPOOL_TX_LIVETIME);
        mempoolTxFromAltBlockLiveTime(parameters::CRYPTONOTE_MEMPOOL_TX_FROM_ALT_BLOCK_LIVETIME);
        numberOfPeriodsToForgetTxDeletedFromPool(
            parameters::CRYPTONOTE_NUMBER_OF_PERIODS_TO_FORGET_TX_DELETED_FROM_POOL);

        fusionTxMaxSize(parameters::FUSION_TX_MAX_SIZE);
        fusionTxMinInputCount(parameters::FUSION_TX_MIN_INPUT_COUNT);
        fusionTxMinInOutCountRatio(parameters::FUSION_TX_MIN_IN_OUT_COUNT_RATIO);

        upgradeHeightV2(parameters::UPGRADE_HEIGHT_V2);
        upgradeHeightV3(parameters::UPGRADE_HEIGHT_V3);
        upgradeHeightV4(parameters::UPGRADE_HEIGHT_V4);
        upgradeHeightV5(parameters::UPGRADE_HEIGHT_V5);
	upgradeHeightV6(parameters::UPGRADE_HEIGHT_V6);
        upgradeVotingThreshold(parameters::UPGRADE_VOTING_THRESHOLD);
        upgradeVotingWindow(parameters::UPGRADE_VOTING_WINDOW);
        upgradeWindow(parameters::UPGRADE_WINDOW);

        blocksFileName(parameters::CRYPTONOTE_BLOCKS_FILENAME);
        blockIndexesFileName(parameters::CRYPTONOTE_BLOCKINDEXES_FILENAME);
        txPoolFileName(parameters::CRYPTONOTE_POOLDATA_FILENAME);

        isBlockexplorer(false);
    }

    Transaction CurrencyBuilder::generateGenesisTransaction()
    {
        CryptoNote::Transaction tx;

        const auto publicViewKey = Constants::NULL_PUBLIC_KEY;
        const auto publicSpendKey = Constants::NULL_PUBLIC_KEY;

        m_currency.constructMinerTx(
            1, 0, 0, 0, 0, 0, publicViewKey, publicSpendKey, tx
        );

        return tx;
    }

    CurrencyBuilder &CurrencyBuilder::numberOfDecimalPlaces(size_t val)
    {
        m_currency.m_numberOfDecimalPlaces = val;
        m_currency.m_coin = 1;
        for (size_t i = 0; i < m_currency.m_numberOfDecimalPlaces; ++i)
        {
            m_currency.m_coin *= 10;
        }

        return *this;
    }

    CurrencyBuilder &CurrencyBuilder::difficultyWindow(size_t val)
    {
        if (val < 2)
        {
            throw std::invalid_argument("val at difficultyWindow()");
        }
        m_currency.m_difficultyWindow = val;
        return *this;
    }

    CurrencyBuilder &CurrencyBuilder::upgradeVotingThreshold(unsigned int val)
    {
        if (val <= 0 || val > 100)
        {
            throw std::invalid_argument("val at upgradeVotingThreshold()");
        }

        m_currency.m_upgradeVotingThreshold = val;
        return *this;
    }

    CurrencyBuilder &CurrencyBuilder::upgradeWindow(uint32_t val)
    {
        if (val <= 0)
        {
            throw std::invalid_argument("val at upgradeWindow()");
        }

        m_currency.m_upgradeWindow = val;
        return *this;
    }

} // namespace CryptoNote
