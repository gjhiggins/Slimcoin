#include "transactionrecord.h"

#include "wallet.h"
#include "base58.h"

/* Return positive answer if transaction should be shown in list.
 */
bool TransactionRecord::showTransaction(const CWalletTx &wtx)
{
    if (wtx.IsCoinBase() || wtx.IsCoinStake())
    {
        // Don't show generated coin until confirmed by at least one block after it
        // so we don't get the user's hopes up until it looks like it's probably accepted.
        //
        // It is not an error when generated blocks are not accepted.  By design,
        // some percentage of blocks, like 10% or more, will end up not accepted.
        // This is the normal mechanism by which the network copes with latency.
        //
        // We display regular transactions right away before any confirmation
        // because they can always get into some block eventually.  Generated coins
        // are special because if their block is not accepted, they are not valid.
        //
        if (wtx.GetDepthInMainChain() < 2)
        {
            return false;
        }
    }
    return true;
}

/*
 * Decompose CWallet transaction to model transaction records.
 */
QList<TransactionRecord> TransactionRecord::decomposeTransaction(const CWallet *wallet, const CWalletTx &wtx, bool fBurnMint)
{
    QList<TransactionRecord> parts;
    int64 nTime = wtx.GetTxTime();
    int64 nCredit = wtx.GetCredit(true);
    int64 nDebit = wtx.GetDebit(MINE_SPENDABLE|MINE_WATCH_ONLY);
    int64 nNet = nCredit - nDebit;
    uint256 hash = wtx.GetHash();
    std::map<std::string, std::string> mapValue = wtx.mapValue;

    if (showTransaction(wtx))
    {
        if (fBurnMint && wtx.IsCoinBase()) // slimcoin: burn transaction
        {
            parts.append(TransactionRecord(hash, nTime, TransactionRecord::BurnMint, "", -nDebit, wtx.GetValueOut()));
        }
        else if (wtx.IsCoinStake()) // ppcoin: coinstake transaction
        {
            TransactionRecord sub(hash, nTime, TransactionRecord::StakeMint, "", -nDebit, wtx.GetValueOut());
            CTxDestination address;
            CTxOut txout = wtx.vout[1];

            if (ExtractDestination(txout.scriptPubKey, address) && IsMine(*wallet, address))
                sub.address = CBitcoinAddress(address).ToString();

            sub.involvesWatchAddress = wallet->IsMine(txout) == MINE_WATCH_ONLY;
            parts.append(sub);
        }
        else if (nNet > 0 || wtx.IsCoinBase())
        {
            //
            // Credit
            //
            BOOST_FOREACH(const CTxOut& txout, wtx.vout)
            {
                isminetype mine = wallet->IsMine(txout);
                if (mine)
                {
                    TransactionRecord sub(hash, nTime);
                    CTxDestination address;
                    sub.idx = parts.size(); // sequence number
                    sub.credit = txout.nValue;
                    sub.involvesWatchAddress = mine == MINE_WATCH_ONLY;

                    if (ExtractDestination(txout.scriptPubKey, address) && IsMine(*wallet, address))
                    {
                        // Received by Bitcoin Address
                        sub.type = TransactionRecord::RecvWithAddress;
                        sub.address = CBitcoinAddress(address).ToString();
                    }
                    else
                    {
                        // Received by IP connection (deprecated features), or a multisignature or other non-simple transaction
                        sub.type = TransactionRecord::RecvFromOther;
                        sub.address = mapValue["from"];
                    }
                    if (wtx.IsCoinBase())
                    {
                        // Generated
                        sub.type = TransactionRecord::Generated;
                    }

                    parts.append(sub);
                }
            }
        }
        else
        {
            bool involvesWatchAddress = false;
            isminetype fAllFromMe = MINE_SPENDABLE;
            BOOST_FOREACH(const CTxIn& txin, wtx.vin)
            {
                isminetype mine = wallet->IsMine(txin);
                if (mine == MINE_WATCH_ONLY)
                    involvesWatchAddress = true;
                if (fAllFromMe > mine)
                    fAllFromMe = mine;
            }  
            isminetype fAllToMe = MINE_SPENDABLE;
            BOOST_FOREACH(const CTxOut& txout, wtx.vout)
            {
                if ( 0 == txout.nValue )
                    // treat zero-valued txout as an OP_RETURN
                    continue;
                isminetype mine = wallet->IsMine(txout);
                if (mine == MINE_WATCH_ONLY)
                    involvesWatchAddress = true;
                if (fAllToMe > mine)
                    fAllToMe = mine;
            }

            if (fAllFromMe && fAllToMe)
            {
                // Payment to self
                int64 nChange = wtx.GetChange();

                parts.append(TransactionRecord(hash, nTime, TransactionRecord::SendToSelf, "",
                             -(nDebit - nChange), nCredit - nChange));
                parts.last().involvesWatchAddress = involvesWatchAddress;   // maybe pass to TransactionRecord as constructor argument
            }
            else if (fAllFromMe)
            {
                //
                // Debit
                //
                int64 nTxFee = nDebit - wtx.GetValueOut();

                for (int nOut = 0; nOut < wtx.vout.size(); nOut++)
                {
                    const CTxOut& txout = wtx.vout[nOut];
                    TransactionRecord sub(hash, nTime);
                    sub.idx = parts.size();
                    sub.involvesWatchAddress = involvesWatchAddress;

                    if (wallet->IsMine(txout))
                    {
                        // Ignore parts sent to self, as this is usually the change
                        // from a transaction sent back to our own address.
                        continue;
                    }

                    CBurnAddress burnAddress;
                    CTxDestination address;
                    if (ExtractDestination(txout.scriptPubKey, address))
                    {
#if BOOST_VERSION >= 158000
                        if (address != burnAddress.Get())
                            // Sent to Slimcoin Address
                            sub.type = TransactionRecord::SendToAddress;
                        else
                            sub.type = TransactionRecord::Burned; // Burned coins
#else
                        if (address == burnAddress.Get())
                            sub.type = TransactionRecord::Burned; // Burned coins
                        else
                            // Sent to Slimcoin Address
                            sub.type = TransactionRecord::SendToAddress;
#endif
                        // Perhaps reduced to:
                        // sub.type = (address == burnAddress.Get() ? TransactionRecord::Burned : TransactionRecord::SendToAddress);
                        sub.address = CBitcoinAddress(address).ToString();
                    }
                    else
                    {
                        // Sent to IP, or other non-address transaction like OP_EVAL
                        sub.type = TransactionRecord::SendToOther;
                        sub.address = mapValue["to"];
                    }

                    int64 nValue = txout.nValue;
                    /* Add fee to first output */
                    if (nTxFee > 0)
                    {
                        nValue += nTxFee;
                        nTxFee = 0;
                    }
                    sub.debit = -nValue;

                    parts.append(sub);
                }
            }
            else
            {
                //
                // Mixed debit transaction, can't break down payees
                //
                parts.append(TransactionRecord(hash, nTime, TransactionRecord::Other, "", nNet, 0));
                parts.last().involvesWatchAddress = involvesWatchAddress;
            }
        }
    }

    return parts;
}

void TransactionRecord::updateStatus(const CWalletTx &wtx)
{
    // Determine transaction status

    // Find the block the tx is in
    CBlockIndex* pindex = NULL;
    std::map<uint256, CBlockIndex*>::iterator mi = mapBlockIndex.find(wtx.hashBlock);
    if (mi != mapBlockIndex.end())
        pindex = (*mi).second;

    // Sort order, unrecorded transactions sort to the top
    status.sortKey = strprintf("%010d-%01d-%010u-%03d",
        (pindex ? pindex->nHeight : std::numeric_limits<int>::max()),
        (wtx.IsCoinBase() ? 1 : 0),
        wtx.nTimeReceived,
        idx);
    status.confirmed = wtx.IsConfirmed();
    status.depth = wtx.GetDepthInMainChain();

    //burn transactions need a bit more information
    if (wtx.IsBurnTx())
    {
        //burn transactions mature differently
        status.burnIsMature = wtx.IsBurnTxMature();
        status.burnDepth = wtx.GetBurnDepthInMainChain();
    }

    status.cur_num_blocks = nBestHeight;

    if (!wtx.IsFinal())
    {
        if (wtx.nLockTime < LOCKTIME_THRESHOLD)
        {
            status.status = TransactionStatus::OpenUntilBlock;
            status.open_for = nBestHeight - wtx.nLockTime;
        }
        else
        {
            status.status = TransactionStatus::OpenUntilDate;
            status.open_for = wtx.nLockTime;
        }
    }
    else
    {
        if (GetAdjustedTime() - wtx.nTimeReceived > 2 * 60 && wtx.GetRequestCount() == 0)
        {
            status.status = TransactionStatus::Offline;
        }
        else if (status.depth < NumConfirmations)
        {
            status.status = TransactionStatus::Unconfirmed;
        }
        else
        {
            status.status = TransactionStatus::HaveConfirmations;
        }
    }

    // For generated transactions, determine maturity
    if (type == TransactionRecord::Generated || type == TransactionRecord::StakeMint || type == TransactionRecord::BurnMint)
    {
        int64 nCredit = wtx.GetCredit(true);
        if (nCredit == 0)
        {
            status.maturity = TransactionStatus::Immature;

            if (wtx.IsInMainChain())
            {
                status.matures_in = wtx.GetBlocksToMaturity();

                // Check if the block was requested by anyone
                if (GetAdjustedTime() - wtx.nTimeReceived > 2 * 60 && wtx.GetRequestCount() == 0)
                    status.maturity = TransactionStatus::MaturesWarning;
            }
            else
            {
                status.maturity = TransactionStatus::NotAccepted;
            }
        }
        else
        {
            status.maturity = TransactionStatus::Mature;
        }
    }
}

bool TransactionRecord::statusUpdateNeeded()
{
    return status.cur_num_blocks != nBestHeight;
}

std::string TransactionRecord::getTxID()
{
    return hash.ToString() + strprintf("-%03d", idx);
}

