// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2012 The Bitcoin developers
// Copyright (c) 2011-2013 The PPCoin developers
// Copyright (c) 2013-2014 The Slimcoin developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "main.h"
#include "wallet.h"
#include "walletdb.h"
#include "crypter.h"
#include "ui_interface.h"
#include "base58.h"
#include "coincontrol.h"
#include "kernel.h"
#include "keystore.h"
#include "smalldata.h"

#include <boost/algorithm/string/replace.hpp>
#include "base58.h"

using namespace std;


//////////////////////////////////////////////////////////////////////////////
//
// mapWallet
//

CPubKey CWallet::GenerateNewKey()
{
    bool fCompressed = CanSupportFeature(FEATURE_COMPRPUBKEY); // default to compressed public keys if we want 0.6.0 wallets

    RandAddSeedPerfmon();
    CKey key;
    key.MakeNewKey(fCompressed);

    // Compressed public keys were introduced in version 0.6.0
    if (fCompressed)
        SetMinVersion(FEATURE_COMPRPUBKEY);

    if (!AddKey(key))
        throw std::runtime_error("CWallet::GenerateNewKey() : AddKey failed");
    return key.GetPubKey();
}

bool CWallet::AddKey(const CKey& key)
{
    if (!CCryptoKeyStore::AddKey(key))
        return false;
    if (!fFileBacked)
        return true;
    if (!IsCrypted())
        return CWalletDB(strWalletFile).WriteKey(key.GetPubKey(), key.GetPrivKey());
    return true;
}

bool CWallet::AddCryptedKey(const CPubKey &vchPubKey, const vector<unsigned char> &vchCryptedSecret)
{
    if (!CCryptoKeyStore::AddCryptedKey(vchPubKey, vchCryptedSecret))
        return false;
    if (!fFileBacked)
        return true;
    {
        LOCK(cs_wallet);
        if (pwalletdbEncryption)
            return pwalletdbEncryption->WriteCryptedKey(vchPubKey, vchCryptedSecret);
        else
            return CWalletDB(strWalletFile).WriteCryptedKey(vchPubKey, vchCryptedSecret);
    }
    return false;
}

/* FIXME: Causes redefinition error
bool CWallet::LoadCryptedKey(const CPubKey &vchPubKey, const std::vector<unsigned char> &vchCryptedSecret)
{
    return CCryptoKeyStore::AddCryptedKey(vchPubKey, vchCryptedSecret);
}
*/

bool CWallet::AddCScript(const CScript& redeemScript)
{
    if (!CCryptoKeyStore::AddCScript(redeemScript))
        return false;
    if (!fFileBacked)
        return true;
    return CWalletDB(strWalletFile).WriteCScript(Hash160(redeemScript), redeemScript);
}

bool CWallet::Lock()
{
    if (IsLocked())
        return true;

    if (fDebug)
        printf("Locking wallet.\n");

    {
        LOCK(cs_wallet);
        CWalletDB wdb(strWalletFile);

    }
    return LockKeyStore();
};

// ppcoin: optional setting to unlock wallet for block minting only;
//         serves to disable the trivial sendmoney when OS account compromised
bool fWalletUnlockMintOnly = false;

bool CWallet::Unlock(const SecureString& strWalletPassphrase)
{
    if (!IsLocked())
        return false;

    CCrypter crypter;
    CKeyingMaterial vMasterKey;

    {
        LOCK(cs_wallet);
        BOOST_FOREACH(const MasterKeyMap::value_type& pMasterKey, mapMasterKeys)
        {
            if(!crypter.SetKeyFromPassphrase(strWalletPassphrase, pMasterKey.second.vchSalt, pMasterKey.second.nDeriveIterations, pMasterKey.second.nDerivationMethod))
                return false;
            if (!crypter.Decrypt(pMasterKey.second.vchCryptedKey, vMasterKey))
                return false;
            if (CCryptoKeyStore::Unlock(vMasterKey))
                return true;
        }
    }
    return false;
}

bool CWallet::ChangeWalletPassphrase(const SecureString& strOldWalletPassphrase, const SecureString& strNewWalletPassphrase)
{
    bool fWasLocked = IsLocked();

    {
        LOCK(cs_wallet);
        Lock();

        CCrypter crypter;
        CKeyingMaterial vMasterKey;
        BOOST_FOREACH(MasterKeyMap::value_type& pMasterKey, mapMasterKeys)
        {
            if(!crypter.SetKeyFromPassphrase(strOldWalletPassphrase, pMasterKey.second.vchSalt, pMasterKey.second.nDeriveIterations, pMasterKey.second.nDerivationMethod))
                return false;
            if (!crypter.Decrypt(pMasterKey.second.vchCryptedKey, vMasterKey))
                return false;
            if (CCryptoKeyStore::Unlock(vMasterKey))
            {
                int64 nStartTime = GetTimeMillis();
                crypter.SetKeyFromPassphrase(strNewWalletPassphrase, pMasterKey.second.vchSalt, pMasterKey.second.nDeriveIterations, pMasterKey.second.nDerivationMethod);
                pMasterKey.second.nDeriveIterations = pMasterKey.second.nDeriveIterations * (100 / ((double)(GetTimeMillis() - nStartTime)));

                nStartTime = GetTimeMillis();
                crypter.SetKeyFromPassphrase(strNewWalletPassphrase, pMasterKey.second.vchSalt, pMasterKey.second.nDeriveIterations, pMasterKey.second.nDerivationMethod);
                pMasterKey.second.nDeriveIterations = (pMasterKey.second.nDeriveIterations + pMasterKey.second.nDeriveIterations * 100 / ((double)(GetTimeMillis() - nStartTime))) / 2;

                if (pMasterKey.second.nDeriveIterations < 25000)
                    pMasterKey.second.nDeriveIterations = 25000;

                printf("Wallet passphrase changed to an nDeriveIterations of %i\n", pMasterKey.second.nDeriveIterations);

                if (!crypter.SetKeyFromPassphrase(strNewWalletPassphrase, pMasterKey.second.vchSalt, pMasterKey.second.nDeriveIterations, pMasterKey.second.nDerivationMethod))
                    return false;
                if (!crypter.Encrypt(vMasterKey, pMasterKey.second.vchCryptedKey))
                    return false;
                CWalletDB(strWalletFile).WriteMasterKey(pMasterKey.first, pMasterKey.second);
                if (fWasLocked)
                    Lock();
                return true;
            }
        }
    }

    return false;
}

void CWallet::SetBestChain(const CBlockLocator& loc)
{
    CWalletDB walletdb(strWalletFile);
    walletdb.WriteBestBlock(loc);
}

// This class implements an addrIncoming entry that causes pre-0.4
// clients to crash on startup if reading a private-key-encrypted wallet.
class CCorruptAddress
{
public:
    IMPLEMENT_SERIALIZE
    (
        if (nType & SER_DISK)
            READWRITE(nVersion);
    )
};

bool CWallet::SetMinVersion(enum WalletFeature nVersion, CWalletDB* pwalletdbIn, bool fExplicit)
{
    if (nWalletVersion >= nVersion)
        return true;

    // when doing an explicit upgrade, if we pass the max version permitted, upgrade all the way
    if (fExplicit && nVersion > nWalletMaxVersion)
            nVersion = FEATURE_LATEST;

    nWalletVersion = nVersion;

    if (nVersion > nWalletMaxVersion)
        nWalletMaxVersion = nVersion;

    if (fFileBacked)
    {
        CWalletDB* pwalletdb = pwalletdbIn ? pwalletdbIn : new CWalletDB(strWalletFile);
        if (nWalletVersion >= 40000)
        {
            // Versions prior to 0.4.0 did not support the "minversion" record.
            // Use a CCorruptAddress to make them crash instead.
            CCorruptAddress corruptAddress;
            pwalletdb->WriteSetting("addrIncoming", corruptAddress);
        }
        if (nWalletVersion > 40000)
            pwalletdb->WriteMinVersion(nWalletVersion);
        if (!pwalletdbIn)
            delete pwalletdb;
    }

    return true;
}

bool CWallet::SetMaxVersion(int nVersion)
{
    // cannot downgrade below current version
    if (nWalletVersion > nVersion)
        return false;

    nWalletMaxVersion = nVersion;

    return true;
}

bool CWallet::EncryptWallet(const SecureString& strWalletPassphrase)
{
    if (IsCrypted())
        return false;

    CKeyingMaterial vMasterKey;
    RandAddSeedPerfmon();

    vMasterKey.resize(WALLET_CRYPTO_KEY_SIZE);
    RAND_bytes(&vMasterKey[0], WALLET_CRYPTO_KEY_SIZE);

    CMasterKey kMasterKey;

    RandAddSeedPerfmon();
    kMasterKey.vchSalt.resize(WALLET_CRYPTO_SALT_SIZE);
    RAND_bytes(&kMasterKey.vchSalt[0], WALLET_CRYPTO_SALT_SIZE);

    CCrypter crypter;
    int64 nStartTime = GetTimeMillis();
    crypter.SetKeyFromPassphrase(strWalletPassphrase, kMasterKey.vchSalt, 25000, kMasterKey.nDerivationMethod);
    kMasterKey.nDeriveIterations = 2500000 / ((double)(GetTimeMillis() - nStartTime));

    nStartTime = GetTimeMillis();
    crypter.SetKeyFromPassphrase(strWalletPassphrase, kMasterKey.vchSalt, kMasterKey.nDeriveIterations, kMasterKey.nDerivationMethod);
    kMasterKey.nDeriveIterations = (kMasterKey.nDeriveIterations + kMasterKey.nDeriveIterations * 100 / ((double)(GetTimeMillis() - nStartTime))) / 2;

    if (kMasterKey.nDeriveIterations < 25000)
        kMasterKey.nDeriveIterations = 25000;

    printf("Encrypting Wallet with an nDeriveIterations of %i\n", kMasterKey.nDeriveIterations);

    if (!crypter.SetKeyFromPassphrase(strWalletPassphrase, kMasterKey.vchSalt, kMasterKey.nDeriveIterations, kMasterKey.nDerivationMethod))
        return false;
    if (!crypter.Encrypt(vMasterKey, kMasterKey.vchCryptedKey))
        return false;

    {
        LOCK(cs_wallet);
        mapMasterKeys[++nMasterKeyMaxID] = kMasterKey;
        if (fFileBacked)
        {
            pwalletdbEncryption = new CWalletDB(strWalletFile);
            if (!pwalletdbEncryption->TxnBegin())
                return false;
            pwalletdbEncryption->WriteMasterKey(nMasterKeyMaxID, kMasterKey);
        }

        if (!EncryptKeys(vMasterKey))
        {
            if (fFileBacked)
                pwalletdbEncryption->TxnAbort();
            exit(1); //We now probably have half of our keys encrypted in memory, and half not...die and let the user reload their unencrypted wallet.
        }

        // Encryption was introduced in version 0.4.0
        SetMinVersion(FEATURE_WALLETCRYPT, pwalletdbEncryption, true);

        if (fFileBacked)
        {
            if (!pwalletdbEncryption->TxnCommit())
                exit(1); //We now have keys encrypted in memory, but no on disk...die to avoid confusion and let the user reload their unencrypted wallet.

            delete pwalletdbEncryption;
            pwalletdbEncryption = NULL;
        }

        Lock();
        Unlock(strWalletPassphrase);
        NewKeyPool();
        Lock();

        // Need to completely rewrite the wallet file; if we don't, bdb might keep
        // bits of the unencrypted private key in slack space in the database file.
        CDB::Rewrite(strWalletFile);
    }
    /* FIXME: implement function
    NotifyStatusChanged(this);
    */

    return true;
}

void CWallet::WalletUpdateSpent(const CTransaction &tx)
{
    // Anytime a signature is successfully verified, it's proof the outpoint is spent.
    // Update the wallet spent flag if it doesn't know due to wallet.dat being
    // restored from backup or the user making copies of wallet.dat.
    {
        LOCK(cs_wallet);
        BOOST_FOREACH(const CTxIn& txin, tx.vin)
        {
            map<uint256, CWalletTx>::iterator mi = mapWallet.find(txin.prevout.hash);
            if (mi != mapWallet.end())
            {
                CWalletTx& wtx = (*mi).second;
                if (txin.prevout.n >= wtx.vout.size())
                    printf("WalletUpdateSpent: bad wtx %s\n", wtx.GetHash().ToString().c_str());
                else if (!wtx.IsSpent(txin.prevout.n) && IsMine(wtx.vout[txin.prevout.n]))
                {
                    printf("WalletUpdateSpent found spent coin %sslm %s\n", FormatMoney(wtx.GetCredit()).c_str(), wtx.GetHash().ToString().c_str());
                    wtx.MarkSpent(txin.prevout.n);
                    wtx.WriteToDisk();
                    vWalletUpdated.push_back(txin.prevout.hash);
                    NotifyTransactionChanged(this, txin.prevout.hash, CT_UPDATED);
                }
            }
        }
    }
}

void CWallet::MarkDirty()
{
    {
        LOCK(cs_wallet);
        BOOST_FOREACH(PAIRTYPE(const uint256, CWalletTx)& item, mapWallet)
            item.second.MarkDirty();
    }
}

bool CWallet::AddToWallet(const CWalletTx &wtxIn, bool fBurnTx)
{
    uint256 hash = wtxIn.GetHash();
    {
        LOCK(cs_wallet);
        // Inserts only if not already there, returns tx inserted or tx found
        pair<map<uint256, CWalletTx>::iterator, bool> ret = mapWallet.insert(make_pair(hash, wtxIn));

        //if it is a burn tx, add it to the wallet's its map
        if (fBurnTx)
            setBurnHashes.insert(hash);

        CWalletTx& wtx = (*ret.first).second;
        wtx.BindWallet(this);
        bool fInsertedNew = ret.second;
        if (fInsertedNew)
            wtx.nTimeReceived = GetAdjustedTime();

        bool fUpdated = false;
        if (!fInsertedNew)
        {
            // Merge
            if (wtxIn.hashBlock != 0 && wtxIn.hashBlock != wtx.hashBlock)
            {
                wtx.hashBlock = wtxIn.hashBlock;
                fUpdated = true;
            }
            if (wtxIn.nIndex != -1 && (wtxIn.vMerkleBranch != wtx.vMerkleBranch || wtxIn.nIndex != wtx.nIndex))
            {
                wtx.vMerkleBranch = wtxIn.vMerkleBranch;
                wtx.nIndex = wtxIn.nIndex;
                fUpdated = true;
            }
            if (wtxIn.fFromMe && wtxIn.fFromMe != wtx.fFromMe)
            {
                wtx.fFromMe = wtxIn.fFromMe;
                fUpdated = true;
            }
            fUpdated |= wtx.UpdateSpent(wtxIn.vfSpent);
        }

        //// debug print
        printf("AddToWallet %s  %s%s\n", wtxIn.GetHash().ToString().substr(0,10).c_str(), (fInsertedNew ? "new" : ""), (fUpdated ? "update" : ""));

        // Write to disk
        if (fInsertedNew || fUpdated)
            if (!wtx.WriteToDisk(fBurnTx))
                return false;
#ifndef QT_GUI
        // If default receiving address gets used, replace it with a new one
        CScript scriptDefaultKey;
        scriptDefaultKey.SetDestination(vchDefaultKey.GetID());
        BOOST_FOREACH(const CTxOut& txout, wtx.vout)
        {
            if (txout.scriptPubKey == scriptDefaultKey)
            {
                CPubKey newDefaultKey;
                if (GetKeyFromPool(newDefaultKey, false))
                {
                    SetDefaultKey(newDefaultKey);
                    SetAddressBookName(vchDefaultKey.GetID(), "");
                }
            }
        }
#endif
        // Notify UI
        vWalletUpdated.push_back(hash);

        // since AddToWallet is called directly for self-originating transactions, check for consumption of own coins
        WalletUpdateSpent(wtx);

        // Notify UI of new or updated transaction
        NotifyTransactionChanged(this, hash, fInsertedNew ? CT_NEW : CT_UPDATED);

        // notify an external script when a wallet transaction comes in or is updated
        std::string strCmd = GetArg("-walletnotify", "");

        if ( !strCmd.empty())
        {
            boost::replace_all(strCmd, "%s", wtxIn.GetHash().GetHex());
            boost::thread t(runCommand, strCmd); // thread runs free
        }
    }

    // Refresh UI
    MainFrameRepaint();
    return true;
}

// Add a transaction to the wallet, or update it.
// pblock is optional, but should be provided if the transaction is known to be in a block.
// If fUpdate is true, existing transactions will be updated.
bool CWallet::AddToWalletIfInvolvingMe(const CTransaction& tx, const CBlock* pblock, bool fUpdate, bool fFindBlock)
{
    uint256 hash = tx.GetHash();
    {
        LOCK(cs_wallet);
        bool fExisted = mapWallet.count(hash);
        if (fExisted && !fUpdate) return false;
        if (fExisted || IsMine(tx) || IsFromMe(tx))
        {
            CWalletTx wtx(this,tx);
            // Get merkle branch if transaction was found in a block
            if (pblock)
                wtx.SetMerkleBranch(pblock);
            return AddToWallet(wtx, wtx.IsBurnTx());
        }
        else
            WalletUpdateSpent(tx);
    }
    return false;
}

bool CWallet::EraseFromWallet(uint256 hash)
{
    if (!fFileBacked)
        return false;
    {
        LOCK(cs_wallet);
        if (mapWallet.erase(hash))
            CWalletDB(strWalletFile).EraseTx(hash);
    }
    return true;
}


bool CWallet::IsMine(const CTxIn &txin) const
{
    {
        LOCK(cs_wallet);
        map<uint256, CWalletTx>::const_iterator mi = mapWallet.find(txin.prevout.hash);
        if (mi != mapWallet.end())
        {
            const CWalletTx& prev = (*mi).second;
            if (txin.prevout.n < prev.vout.size())
                if (IsMine(prev.vout[txin.prevout.n]))
                    return true;
        }
    }
    return false;
}

int64 CWallet::GetDebit(const CTxIn &txin) const
{
    {
        LOCK(cs_wallet);
        map<uint256, CWalletTx>::const_iterator mi = mapWallet.find(txin.prevout.hash);
        if (mi != mapWallet.end())
        {
            const CWalletTx& prev = (*mi).second;
            if (txin.prevout.n < prev.vout.size())
                if (IsMine(prev.vout[txin.prevout.n]))
                    return prev.vout[txin.prevout.n].nValue;
        }
    }
    return 0;
}

bool CWallet::IsChange(const CTxOut& txout) const
{
    CTxDestination address;

    // TODO: fix handling of 'change' outputs. The assumption is that any
    // payment to a TX_PUBKEYHASH that is mine but isn't in the address book
    // is change. That assumption is likely to break when we implement multisignature
    // wallets that return change back into a multi-signature-protected address;
    // a better way of identifying which outputs are 'the send' and which are
    // 'the change' will need to be implemented (maybe extend CWalletTx to remember
    // which output, if any, was change).
    if (ExtractDestination(txout.scriptPubKey, address) && ::IsMine(*this, address))
    {
        LOCK(cs_wallet);
        if (!mapAddressBook.count(address))
            return true;
    }
    return false;
}

int64 CWalletTx::GetTxTime() const
{
    return nTimeReceived;
}

int CWalletTx::GetRequestCount() const
{
    // Returns -1 if it wasn't being tracked
    int nRequests = -1;
    {
        LOCK(pwallet->cs_wallet);
        if (IsCoinBase() || IsCoinStake())
        {
            // Generated block
            if (hashBlock != 0)
            {
                map<uint256, int>::const_iterator mi = pwallet->mapRequestCount.find(hashBlock);
                if (mi != pwallet->mapRequestCount.end())
                    nRequests = (*mi).second;
            }
        }
        else
        {
            // Did anyone request this transaction?
            map<uint256, int>::const_iterator mi = pwallet->mapRequestCount.find(GetHash());
            if (mi != pwallet->mapRequestCount.end())
            {
                nRequests = (*mi).second;

                // How about the block it's in?
                if (nRequests == 0 && hashBlock != 0)
                {
                    map<uint256, int>::const_iterator mi = pwallet->mapRequestCount.find(hashBlock);
                    if (mi != pwallet->mapRequestCount.end())
                        nRequests = (*mi).second;
                    else
                        nRequests = 1; // If it's in someone else's block it must have got out
                }
            }
        }
    }
    return nRequests;
}

void CWalletTx::GetAmounts(int64& nGeneratedImmature, int64& nGeneratedMature, list<pair<CTxDestination, int64> >& listReceived,
                           list<pair<CTxDestination, int64> >& listSent, int64& nFee, string& strSentAccount) const
{
    nGeneratedImmature = nGeneratedMature = nFee = 0;
    listReceived.clear();
    listSent.clear();
    strSentAccount = strFromAccount;

    if (IsCoinBase() || IsCoinStake())
    {
        if (GetBlocksToMaturity() > 0)
            nGeneratedImmature = pwallet->GetCredit(*this) - pwallet->GetDebit(*this);
        else
            nGeneratedMature = GetCredit() - GetDebit();
        return;
    }

    // Compute fee:
    int64 nDebit = GetDebit();
    if (nDebit > 0) // debit>0 means we signed/sent this transaction
    {
        int64 nValueOut = GetValueOut();
        nFee = nDebit - nValueOut;
    }

    // Sent/received.
    BOOST_FOREACH(const CTxOut& txout, vout)
    {
        CTxDestination address;
        vector<unsigned char> vchPubKey;
        if (!ExtractDestination(txout.scriptPubKey, address))
        {
            printf("CWalletTx::GetAmounts: Unknown transaction type found, txid %s\n",
                   this->GetHash().ToString().c_str());
        }

        // Don't report 'change' txouts
        if (nDebit > 0 && pwallet->IsChange(txout))
            continue;

        if (nDebit > 0)
            listSent.push_back(make_pair(address, txout.nValue));

        if (pwallet->IsMine(txout))
            listReceived.push_back(make_pair(address, txout.nValue));
    }

}

void CWalletTx::GetAccountAmounts(const string& strAccount, int64& nGenerated, int64& nReceived,
                                  int64& nSent, int64& nFee) const
{
    nGenerated = nReceived = nSent = nFee = 0;

    int64 allGeneratedImmature, allGeneratedMature, allFee;
    allGeneratedImmature = allGeneratedMature = allFee = 0;
    string strSentAccount;
    list<pair<CTxDestination, int64> > listReceived;
    list<pair<CTxDestination, int64> > listSent;
    GetAmounts(allGeneratedImmature, allGeneratedMature, listReceived, listSent, allFee, strSentAccount);

    if (strAccount == "")
        nGenerated = allGeneratedMature;
    if (strAccount == strSentAccount)
    {
        BOOST_FOREACH(const PAIRTYPE(CTxDestination,int64)& s, listSent)
            nSent += s.second;
        nFee = allFee;
    }
    {
        LOCK(pwallet->cs_wallet);
        BOOST_FOREACH(const PAIRTYPE(CTxDestination,int64)& r, listReceived)
        {
            if (pwallet->mapAddressBook.count(r.first))
            {
                map<CTxDestination, string>::const_iterator mi = pwallet->mapAddressBook.find(r.first);
                if (mi != pwallet->mapAddressBook.end() && (*mi).second == strAccount)
                    nReceived += r.second;
            }
            else if (strAccount.empty())
            {
                nReceived += r.second;
            }
        }
    }
}

void CWalletTx::AddSupportingTransactions(CTxDB& txdb)
{
    vtxPrev.clear();

    const int COPY_DEPTH = 3;
    if (SetMerkleBranch() < COPY_DEPTH)
    {
        vector<uint256> vWorkQueue;
        BOOST_FOREACH(const CTxIn& txin, vin)
            vWorkQueue.push_back(txin.prevout.hash);

        // This critsect is OK because txdb is already open
        {
            LOCK(pwallet->cs_wallet);
            map<uint256, const CMerkleTx*> mapWalletPrev;
            set<uint256> setAlreadyDone;
            for (unsigned int i = 0; i < vWorkQueue.size(); i++)
            {
                uint256 hash = vWorkQueue[i];
                if (setAlreadyDone.count(hash))
                    continue;
                setAlreadyDone.insert(hash);

                CMerkleTx tx;
                map<uint256, CWalletTx>::const_iterator mi = pwallet->mapWallet.find(hash);
                if (mi != pwallet->mapWallet.end())
                {
                    tx = (*mi).second;
                    BOOST_FOREACH(const CMerkleTx& txWalletPrev, (*mi).second.vtxPrev)
                        mapWalletPrev[txWalletPrev.GetHash()] = &txWalletPrev;
                }
                else if (mapWalletPrev.count(hash))
                {
                    tx = *mapWalletPrev[hash];
                }
                else if (!fClient && txdb.ReadDiskTx(hash, tx))
                {
                    ;
                }
                else
                {
                    printf("ERROR: AddSupportingTransactions() : unsupported transaction\n");
                    continue;
                }

                int nDepth = tx.SetMerkleBranch();
                vtxPrev.push_back(tx);

                if (nDepth < COPY_DEPTH)
                {
                    BOOST_FOREACH(const CTxIn& txin, tx.vin)
                        vWorkQueue.push_back(txin.prevout.hash);
                }
            }
        }
    }

    reverse(vtxPrev.begin(), vtxPrev.end());
}

bool CWalletTx::WriteToDisk(bool fBurnTx)
{
    //if it is not a burn transaction
    if (!fBurnTx)
        return CWalletDB(pwallet->strWalletFile).WriteTx(GetHash(), *this);
    else //if it is a burn transaction
        return CWalletDB(pwallet->strWalletFile).WriteTx(GetHash(), *this) &&
            CWalletDB(pwallet->strWalletFile).WriteBurnTx(GetHash(), *this);
}

// Scan the block chain (starting in pindexStart) for transactions
// from or to us. If fUpdate is true, found transactions that already
// exist in the wallet will be updated.
int CWallet::ScanForWalletTransactions(CBlockIndex* pindexStart, bool fUpdate)
{
    int ret = 0;

    //if fUpdate is true, it will do a full update, so also clear the setBurnHashes
    if (fUpdate)
        setBurnHashes.clear();

    CBlockIndex* pindex = pindexStart;
    {
        LOCK(cs_wallet);
        while (pindex)
        {
            CBlock block;
            block.ReadFromDisk(pindex, true, false);
            BOOST_FOREACH(CTransaction& tx, block.vtx)
            {
                if (AddToWalletIfInvolvingMe(tx, &block, fUpdate))
                    ret++;
            }
            pindex = pindex->pnext;
        }
    }
    return ret;
}

int CWallet::ScanForWalletTransaction(const uint256& hashTx)
{
    CTransaction tx;
    tx.ReadFromDisk(COutPoint(hashTx, 0));
    if (AddToWalletIfInvolvingMe(tx, NULL, true, true))
        return 1;
    return 0;
}

void CWallet::ReacceptWalletTransactions()
{
    CTxDB txdb("r");
    bool fRepeat = true;
    while (fRepeat)
    {
        LOCK(cs_wallet);
        fRepeat = false;
        vector<CDiskTxPos> vMissingTx;
        BOOST_FOREACH(PAIRTYPE(const uint256, CWalletTx)& item, mapWallet)
        {
            CWalletTx& wtx = item.second;
            if ((wtx.IsCoinBase() && wtx.IsSpent(0)) || (wtx.IsCoinStake() && wtx.IsSpent(1)))
                continue;

            CTxIndex txindex;
            bool fUpdated = false;
            if (txdb.ReadTxIndex(wtx.GetHash(), txindex))
            {
                // Update fSpent if a tx got spent somewhere else by a copy of wallet.dat
                if (txindex.vSpent.size() != wtx.vout.size())
                {
                    printf("ERROR: ReacceptWalletTransactions() : txindex.vSpent.size() %d != wtx.vout.size() %d\n", txindex.vSpent.size(), wtx.vout.size());
                    continue;
                }
                for (unsigned int i = 0; i < txindex.vSpent.size(); i++)
                {
                    if (wtx.IsSpent(i))
                        continue;
                    if (!txindex.vSpent[i].IsNull() && IsMine(wtx.vout[i]))
                    {
                        wtx.MarkSpent(i);
                        fUpdated = true;
                        vMissingTx.push_back(txindex.vSpent[i]);
                    }
                }
                if (fUpdated)
                {
                    printf("ReacceptWalletTransactions found spent coin %sslm %s\n", FormatMoney(wtx.GetCredit()).c_str(), wtx.GetHash().ToString().c_str());
                    wtx.MarkDirty();
                    wtx.WriteToDisk();
                }
            }
            else
            {
                // Reaccept any txes of ours that aren't already in a block
                if (!(wtx.IsCoinBase() || wtx.IsCoinStake()))
                    wtx.AcceptWalletTransaction(txdb, false);
            }
        }
        if (!vMissingTx.empty())
        {
            // TODO: optimize this to scan just part of the block chain?
            if (ScanForWalletTransactions(pindexGenesisBlock))
                fRepeat = true;  // Found missing transactions: re-do Reaccept.
        }
    }
}

void CWalletTx::RelayWalletTransaction(CTxDB& txdb)
{
    BOOST_FOREACH(const CMerkleTx& tx, vtxPrev)
    {
        if (!(tx.IsCoinBase() || tx.IsCoinStake()))
        {
            uint256 hash = tx.GetHash();
            if (!txdb.ContainsTx(hash))
                RelayMessage(CInv(MSG_TX, hash), (CTransaction)tx);
        }
    }
    if (!(IsCoinBase() || IsCoinStake()))
    {
        uint256 hash = GetHash();
        if (!txdb.ContainsTx(hash))
        {
            printf("Relaying wtx %s\n", hash.ToString().substr(0,10).c_str());
            RelayMessage(CInv(MSG_TX, hash), (CTransaction)*this);
        }
    }
}

void CWalletTx::RelayWalletTransaction()
{
   CTxDB txdb("r");
   RelayWalletTransaction(txdb);
}

void CWallet::ResendWalletTransactions()
{
    // Do this infrequently and randomly to avoid giving away
    // that these are our transactions.
    static int64 nNextTime;
    if (GetTime() < nNextTime)
        return;
    bool fFirst = (nNextTime == 0);
    nNextTime = GetTime() + GetRand(30 * 60);
    if (fFirst)
        return;

    // Only do it if there's been a new block since last time
    static int64 nLastTime;
    if (nTimeBestReceived < nLastTime)
        return;
    nLastTime = GetTime();

    // Rebroadcast any of our txes that aren't in a block yet
    printf("ResendWalletTransactions()\n");
    CTxDB txdb("r");
    {
        LOCK(cs_wallet);
        // Sort them in chronological order
        multimap<unsigned int, CWalletTx*> mapSorted;
        BOOST_FOREACH(PAIRTYPE(const uint256, CWalletTx)& item, mapWallet)
        {
            CWalletTx& wtx = item.second;
            // Don't rebroadcast until it's had plenty of time that
            // it should have gotten in already by now.
            if (nTimeBestReceived - (int64)wtx.nTimeReceived > 5 * 60)
                mapSorted.insert(make_pair(wtx.nTimeReceived, &wtx));
        }
        BOOST_FOREACH(PAIRTYPE(const unsigned int, CWalletTx*)& item, mapSorted)
        {
            CWalletTx& wtx = *item.second;
            if (wtx.CheckTransaction())
                wtx.RelayWalletTransaction(txdb);
            else
                printf("ResendWalletTransactions() : CheckTransaction failed for transaction %s\n", wtx.GetHash().ToString().c_str());
        }
    }
}

/* This is how CLAMs does it ... */

void CWallet::SearchOPRETURNTransactions(uint256 hash, std::vector<std::pair<std::string, int> >& vTxResults)
{
    int blockstogoback = pindexBest->nHeight - 362500;
    std::string matchingHash = "face " + hash.GetHex();

    const CBlockIndex* pindexFirst = pindexBest;
    for (int i = 0; pindexFirst && i < blockstogoback; i++) {

        CBlock block;
        block.ReadFromDisk(pindexFirst, true);

        BOOST_FOREACH (const CTransaction& tx, block.vtx)
        {
            std::string txmsg;
            std::string addr;
            bool isBroadcast;
            CTransaction ctx = tx;
            if ( GetTxMessage(ctx, txmsg, addr, isBroadcast) ) {
                if (txmsg == matchingHash) {
                    vTxResults.push_back( std::make_pair(tx.GetHash().GetHex(), tx.nTime/*pindexFirst->nHeight*/) );
                }
            }
        }

        pindexFirst = pindexFirst->pprev;
    }
    return;
}

void CWallet::GetTxMessages(std::vector<std::pair<std::string, int> >& vTxResults)
{
    // int blockstogoback = pindexBest->nHeight - /*36*/2500;
    int blockstogoback = pindexBest->nHeight - 362500;

    const CBlockIndex* pindexFirst = pindexBest;
    for (int i = 0; pindexFirst && i < blockstogoback; i++) {

        CBlock block;
        block.ReadFromDisk(pindexFirst, true, false);

        BOOST_FOREACH (const CTransaction& tx, block.vtx)
        {
            std::string txmsg;
            std::string addr;
            bool isBroadcast;
            CTransaction ctx = tx;
            if ( GetTxMessage(ctx, txmsg, addr, isBroadcast) ) {
                vTxResults.push_back( std::make_pair(txmsg, tx.nTime) );
            }
        }

        pindexFirst = pindexFirst->pprev;
    }
    return;
}


void CWallet::GetMyTxMessages(std::vector<std::pair<std::string, int> >& vTxResults)
{
    LOCK(cs_wallet);
    for(std::map<uint256, CWalletTx>::iterator it = this->mapWallet.begin(); it != this->mapWallet.end(); ++it)
    {
        CWalletTx &tx = it->second;
        if(!mapBlockIndex.count(tx.hashBlock))
            continue;

        CBlockIndex *pindex = mapBlockIndex[tx.hashBlock];
        if(!pindex)
            continue;

        std::string txmsg;
        std::string addr;
        bool isBroadcast;
        CTransaction ctx = tx;
        if ( GetTxMessage(ctx, txmsg, addr, isBroadcast) ) {
            vTxResults.push_back( std::make_pair(txmsg, pindex->nTime) );
        }
    }
}



//////////////////////////////////////////////////////////////////////////////
//
// Actions
//


int64 CWallet::GetBalance() const
{
    int64 nTotal = 0;
    {
        LOCK(cs_wallet);
        for (map<uint256, CWalletTx>::const_iterator it = mapWallet.begin(); it != mapWallet.end(); ++it)
        {
            const CWalletTx* pcoin = &(*it).second;
            if (!pcoin->IsFinal() || !pcoin->IsConfirmed())
                continue;
            nTotal += pcoin->GetAvailableCredit();
        }
    }

    return nTotal;
}

int64 CWallet::GetReserveBalance() const
{
    int64 nReserveBalance = 0;
    if (mapArgs.count("-reservebalance") && !ParseMoney(mapArgs["-reservebalance"], nReserveBalance))
        return error("CWallet:GetReserveBalance : invalid reserve balance amount");
    return nReserveBalance;
}

int64 CWallet::GetUnconfirmedBalance() const
{
    int64 nTotal = 0;
    {
        LOCK(cs_wallet);
        for (map<uint256, CWalletTx>::const_iterator it = mapWallet.begin(); it != mapWallet.end(); ++it)
        {
            const CWalletTx* pcoin = &(*it).second;
            if (pcoin->IsFinal() && pcoin->IsConfirmed())
                continue;
            nTotal += pcoin->GetAvailableCredit();
        }
    }
    return nTotal;
}

int64 CWallet::GetImmatureBalance() const
{
    int64 nTotal = 0;
    {
        LOCK(cs_wallet);
        for (map<uint256, CWalletTx>::const_iterator it = mapWallet.begin(); it != mapWallet.end(); ++it)
        {
            const CWalletTx& pcoin = (*it).second;
            if (pcoin.IsCoinBase() && pcoin.GetBlocksToMaturity() > 0 && pcoin.GetDepthInMainChain() >= 2)
                nTotal += GetCredit(pcoin);
        }
    }
    return nTotal;
}

// ppcoin: total coins staked (non-spendable until maturity)
int64 CWallet::GetStake() const
{
    int64 nTotal = 0;
    LOCK(cs_wallet);
    for (map<uint256, CWalletTx>::const_iterator it = mapWallet.begin(); it != mapWallet.end(); ++it)
    {
        const CWalletTx* pcoin = &(*it).second;
        if (pcoin->IsCoinStake() && pcoin->GetBlocksToMaturity() > 0 && pcoin->GetDepthInMainChain() > 0)
            nTotal += CWallet::GetCredit(*pcoin);
    }
    return nTotal;
}

int64 CWallet::GetNewMint() const
{
    int64 nTotal = 0;
    LOCK(cs_wallet);
    for (map<uint256, CWalletTx>::const_iterator it = mapWallet.begin(); it != mapWallet.end(); ++it)
    {
        const CWalletTx* pcoin = &(*it).second;
        if (pcoin->IsCoinBase() && pcoin->GetBlocksToMaturity() > 0 && pcoin->GetDepthInMainChain() > 0)
            nTotal += CWallet::GetCredit(*pcoin);
    }
    return nTotal;
}

// populate vCoins with vector of spendable (age, (value, (transaction, output_number))) outputs
void CWallet::AvailableCoins(unsigned int nSpendTime, vector<COutput>& vCoins, bool fOnlyConfirmed, const CCoinControl *coinControl) const
{
    vCoins.clear();

    {
        LOCK(cs_wallet);
        for (map<uint256, CWalletTx>::const_iterator it = mapWallet.begin(); it != mapWallet.end(); ++it)
        {
            const CWalletTx* pcoin = &(*it).second;

            if (!pcoin->IsFinal())
                continue;

            if (fOnlyConfirmed && !pcoin->IsConfirmed())
                continue;

            if ((pcoin->IsCoinBase() || pcoin->IsCoinStake()) && pcoin->GetBlocksToMaturity() > 0)
                continue;

            for (int i = 0; i < pcoin->vout.size(); i++)
            {
                if (pcoin->nTime > nSpendTime)
                    continue;  // ppcoin: timestamp must not exceed spend time

                if (!(pcoin->IsSpent(i)) && IsMine(pcoin->vout[i]) && pcoin->vout[i].nValue > 0 && (!coinControl || !coinControl->HasSelected() || coinControl->IsSelected((*it).first, i)))
                    vCoins.push_back(COutput(pcoin, i, pcoin->GetDepthInMainChain()));
            }
        }
    }
}

bool CWallet::SelectCoinsMinConf(int64 nTargetValue, int nConfMine, int nConfTheirs, vector<COutput> vCoins,
                                 vector<pair<const CWalletTx*,unsigned int> >& vCoinsRet, int64& nValueRet) const
{
    vCoinsRet.clear();
    nValueRet = 0;

    // List of values less than target
    pair<int64, pair<const CWalletTx*,unsigned int> > coinLowestLarger;
    coinLowestLarger.first = std::numeric_limits<int64>::max();
    coinLowestLarger.second.first = NULL;
    vector<pair<int64, pair<const CWalletTx*,unsigned int> > > vValue;
    int64 nTotalLower = 0;

    BOOST_FOREACH(COutput output, vCoins)
    {
        const CWalletTx *pcoin = output.tx;

        if (output.nDepth < (pcoin->IsFromMe() ? nConfMine : nConfTheirs))
            continue;

        int i = output.i;
        int64 n = pcoin->vout[i].nValue;

        pair<int64,pair<const CWalletTx*,unsigned int> > coin = make_pair(n,make_pair(pcoin, i));

        if (n == nTargetValue)
        {
            vCoinsRet.push_back(coin.second);
            nValueRet += coin.first;
            return true;
        }
        else if (n < nTargetValue + CENT)
        {
            vValue.push_back(coin);
            nTotalLower += n;
        }
        else if (n < coinLowestLarger.first)
        {
            coinLowestLarger = coin;
        }
    }

    if (nTotalLower == nTargetValue || nTotalLower == nTargetValue + CENT)
    {
        for (unsigned int i = 0; i < vValue.size(); ++i)
        {
            vCoinsRet.push_back(vValue[i].second);
            nValueRet += vValue[i].first;
        }
        return true;
    }

    if (nTotalLower < nTargetValue + (coinLowestLarger.second.first ? CENT : 0))
    {
        if (coinLowestLarger.second.first == NULL)
            return false;
        vCoinsRet.push_back(coinLowestLarger.second);
        nValueRet += coinLowestLarger.first;
        return true;
    }

    if (nTotalLower >= nTargetValue + CENT)
        nTargetValue += CENT;

    // Solve subset sum by stochastic approximation
    sort(vValue.rbegin(), vValue.rend());
    vector<char> vfIncluded;
    vector<char> vfBest(vValue.size(), true);
    int64 nBest = nTotalLower;

    for (int nRep = 0; nRep < 1000 && nBest != nTargetValue; nRep++)
    {
        vfIncluded.assign(vValue.size(), false);
        int64 nTotal = 0;
        bool fReachedTarget = false;
        for (int nPass = 0; nPass < 2 && !fReachedTarget; nPass++)
        {
            for (unsigned int i = 0; i < vValue.size(); i++)
            {
                if (nPass == 0 ? rand() % 2 : !vfIncluded[i])
                {
                    nTotal += vValue[i].first;
                    vfIncluded[i] = true;
                    if (nTotal >= nTargetValue)
                    {
                        fReachedTarget = true;
                        if (nTotal < nBest)
                        {
                            nBest = nTotal;
                            vfBest = vfIncluded;
                        }
                        nTotal -= vValue[i].first;
                        vfIncluded[i] = false;
                    }
                }
            }
        }
    }

    // If the next larger is still closer, return it
    if (coinLowestLarger.second.first && coinLowestLarger.first - nTargetValue <= nBest - nTargetValue)
    {
        vCoinsRet.push_back(coinLowestLarger.second);
        nValueRet += coinLowestLarger.first;
    }
    else {
        for (unsigned int i = 0; i < vValue.size(); i++)
            if (vfBest[i])
            {
                vCoinsRet.push_back(vValue[i].second);
                nValueRet += vValue[i].first;
            }

#ifdef DEBUG
        //// debug print
        printf("SelectCoins() best subset: ");
        for (unsigned int i = 0; i < vValue.size(); i++)
            if (vfBest[i])
                printf("%s ", FormatMoney(vValue[i].first).c_str());
        printf("total %s\n", FormatMoney(nBest).c_str());
#endif
    }

    return true;
}

bool CWallet::SelectCoins(int64 nTargetValue, unsigned int nSpendTime, set<pair<const CWalletTx*,unsigned int> >& setCoinsRet, int64& nValueRet, const CCoinControl* coinControl) const
{
    vector<COutput> vCoins;
    AvailableCoins(nSpendTime, vCoins, true, coinControl);

    vector<pair<const CWalletTx*,unsigned int> > vCoinsRet;

    // coin control -> return all selected outputs (we want all selected to go into the transaction for sure)
    if (coinControl && coinControl->HasSelected())
    {
        BOOST_FOREACH(const PAIRTYPE(const CWalletTx*,unsigned int)& c, vCoinsRet)
        {
            setCoinsRet.insert(c);
        }
        return (nValueRet >= nTargetValue);
    }
    // default selection algorithm without CoinControl
    bool result = (SelectCoinsMinConf(nTargetValue, 1, 6, vCoins, vCoinsRet, nValueRet) ||
                   SelectCoinsMinConf(nTargetValue, 1, 1, vCoins, vCoinsRet, nValueRet) ||
                   SelectCoinsMinConf(nTargetValue, 0, 1, vCoins, vCoinsRet, nValueRet));

    BOOST_FOREACH(const PAIRTYPE(const CWalletTx*,unsigned int)& c, vCoinsRet)
    {
        setCoinsRet.insert(c);
    }
    return result;
}

bool CWallet::SelectCoinsForPoS(int64 nTargetValue, unsigned int nSpendTime, vector<pair<const CWalletTx*,unsigned int> >& vCoinsRet, int64& nValueRet, const CCoinControl *coinControl) const
{
    vector<COutput> vCoins;
    AvailableCoins(nSpendTime, vCoins, true, coinControl);

    return SelectCoinsMinConf(nTargetValue, 1, 1, vCoins, vCoinsRet, nValueRet);
}


bool CWallet::CheckStakeKernelHashWithCacheV03(unsigned int nBits, const CBlock& blockFrom, unsigned int nTxPrevOffset, const CTransaction& txPrev, const COutPoint& prevout, unsigned int nTimeTx, uint256& hashProofOfStake, bool fPrintProofOfStake)
{
    uint64 nStakeModifier = 0;
    int nStakeModifierHeight = 0;
    int64 nStakeModifierTime = 0;

    uint256 blockFastHash = blockFrom.GetFastHash();
    std::map<uint256, StakeModifierCacheEntry>::iterator it = mapStakeModifierCacheV03.find(blockFastHash);
    bool stakeModifierCached = it != mapStakeModifierCacheV03.end();

    if(stakeModifierCached)
    {
        // fill in values from cache
        StakeModifierCacheEntry& cacheEntry = it->second;
        nStakeModifier = cacheEntry.stakeModifier;
        nStakeModifierHeight = cacheEntry.nStakeModifierHeight;
        nStakeModifierTime = cacheEntry.nStakeModifierTime;
        cacheEntry.entryAge = 0;
    }
    else
    {
        if (!GetKernelStakeModifier(blockFrom.GetHash(), nTimeTx, nStakeModifier, nStakeModifierHeight, nStakeModifierTime, fPrintProofOfStake))
            return false;

        // add cache entry
        StakeModifierCacheEntry cacheEntry;
        cacheEntry.stakeModifier = nStakeModifier;
        cacheEntry.nStakeModifierHeight = nStakeModifierHeight;
        cacheEntry.nStakeModifierTime = nStakeModifierTime;
        cacheEntry.entryAge = 0;
        mapStakeModifierCacheV03[blockFastHash] = cacheEntry;
    }

    return CheckStakeKernelHashWithStakeModifier(nBits, blockFrom, nTxPrevOffset, txPrev, prevout, nTimeTx, nStakeModifier, nStakeModifierHeight, nStakeModifierTime, hashProofOfStake, fPrintProofOfStake);
}




bool CWallet::CreateTransaction(const vector<pair<CScript, int64> >& vecSend, CWalletTx& wtxNew, CReserveKey& reservekey, int64& nFeeRet, const CCoinControl* coinControl)
{
    int64 nValue = 0;
    BOOST_FOREACH (const PAIRTYPE(CScript, int64)& s, vecSend)
    {
        if (nValue < 0)
            return false;
        nValue += s.second;
    }
    if (vecSend.empty() || nValue < 0)
        return false;

    wtxNew.BindWallet(this);

    {
        LOCK2(cs_main, cs_wallet);
        // txdb must be opened before the mapWallet lock
        CTxDB txdb("r");
        {
            nFeeRet = nTransactionFee;
            for(;;)
            {
                wtxNew.vin.clear();
                wtxNew.vout.clear();
                wtxNew.fFromMe = true;

                int64 nTotalValue = nValue + nFeeRet;
                double dPriority = 0;
                // vouts to the payees
                BOOST_FOREACH (const PAIRTYPE(CScript, int64)& s, vecSend)
                    wtxNew.vout.push_back(CTxOut(s.second, s.first));

                // Choose coins to use
                set<pair<const CWalletTx*,unsigned int> > setCoins;
                int64 nValueIn = 0;
                if (!SelectCoins(nTotalValue, wtxNew.nTime, setCoins, nValueIn, coinControl))
                    return false;
                CScript scriptChange;
                BOOST_FOREACH(PAIRTYPE(const CWalletTx*, unsigned int) pcoin, setCoins)
                {
                    int64 nCredit = pcoin.first->vout[pcoin.second].nValue;
                    dPriority += (double)nCredit * pcoin.first->GetDepthInMainChain();
                    scriptChange = pcoin.first->vout[pcoin.second].scriptPubKey;
                }

                int64 nChange = nValueIn - nValue - nFeeRet;
                // if sub-cent change is required, the fee must be raised to at least MIN_TX_FEE
                // or until nChange becomes zero
                // NOTE: this depends on the exact behaviour of GetMinFee
                if (nFeeRet < MIN_TX_FEE && nChange > 0 && nChange < CENT)
                {
                    int64 nMoveToFee = min(nChange, MIN_TX_FEE - nFeeRet);
                    nChange -= nMoveToFee;
                    nFeeRet += nMoveToFee;
                }

                // ppcoin: sub-cent change is moved to fee
                if (nChange > 0 && nChange < MIN_TXOUT_AMOUNT)
                {
                    nFeeRet += nChange;
                    nChange = 0;
                }

                if (nChange > 0)
                {
                    if (!GetBoolArg("-avatar")) // ppcoin: not avatar mode
                    {
                        // Fill a vout to ourself
                        // TODO: pass in scriptChange instead of reservekey so
                        // change transaction isn't always pay-to-bitcoin-address

                        // coin control: send change to custom address
                        if (coinControl && !boost::get<CNoDestination>(&coinControl->destChange))
                            scriptChange.SetDestination(coinControl->destChange);

                        // no coin control: send change to newly generated address
                        else
                        {
                            // Note: We use a new key here to keep it from being obvious which side is the change.
                            //  The drawback is that by not reusing a previous key, the change may be lost if a
                            //  backup is restored, if the backup doesn't have the new private key for the change.
                            //  If we reused the old key, it would be possible to add code to look for and
                            //  rediscover unknown transactions that were written with keys of ours to recover
                            //  post-backup change.

                            // Reserve a new key pair from key pool
                            CPubKey vchPubKey = reservekey.GetReservedKey();

                            scriptChange.SetDestination(vchPubKey.GetID());
                        }
                    }
                    // Insert change txn at random position:
                    vector<CTxOut>::iterator position = wtxNew.vout.begin()+GetRandInt(wtxNew.vout.size());
                    wtxNew.vout.insert(position, CTxOut(nChange, scriptChange));
                }
                else
                    reservekey.ReturnKey();

                // Fill vin
                BOOST_FOREACH(const PAIRTYPE(const CWalletTx*,unsigned int)& coin, setCoins)
                    wtxNew.vin.push_back(CTxIn(coin.first->GetHash(),coin.second));

                // Sign
                int nIn = 0;
                BOOST_FOREACH(const PAIRTYPE(const CWalletTx*,unsigned int)& coin, setCoins)
                    if (!SignSignature(*this, *coin.first, wtxNew, nIn++))
                        return false;

                // Limit size
                unsigned int nBytes = ::GetSerializeSize(*(CTransaction*)&wtxNew, SER_NETWORK, PROTOCOL_VERSION);
                if (nBytes >= MAX_BLOCK_SIZE_GEN/5)
                    return false;
                dPriority /= nBytes;

                // Check that enough fee is included
                int64 nPayFee = nTransactionFee * (1 + (int64)nBytes / 1000);
                int64 nMinFee = wtxNew.GetMinFee(1, false, GMF_SEND, nBytes);

                if (nFeeRet < max(nPayFee, nMinFee))
                {
                    nFeeRet = max(nPayFee, nMinFee);
                    continue;
                }

                // Fill vtxPrev by copying from previous transactions vtxPrev
                wtxNew.AddSupportingTransactions(txdb);
                wtxNew.fTimeReceivedIsTxTime = true;

                break;
            }
        }
    }
    return true;
}

bool CWallet::CreateTransaction(CScript scriptPubKey, int64 nValue, CWalletTx& wtxNew, CReserveKey& reservekey, int64& nFeeRet, const CCoinControl* coinControl)
{
    vector< pair<CScript, int64> > vecSend;
    vecSend.push_back(make_pair(scriptPubKey, nValue));
    return CreateTransaction(vecSend, wtxNew, reservekey, nFeeRet, coinControl);
}


bool CWallet::PrecomputeCoinStakeCandidates(unsigned int nBits, unsigned int nSecondsToPrecompute)
{
    // Use an easier target for candidates so we don't have to recompute them when the diff goes down a bit.
    CBigNum target;
    target.SetCompact(nBits);
    target <<= 2;
    unsigned int nCandidateBits = target.GetCompact();
    
    unsigned int nTimeNow = GetAdjustedTime();
    unsigned int nTimeBegin = std::max(nTimeNow - 30, nPrecomputedCandidateTime + 1); // look back for up to 30 secs
    unsigned int nTimeEnd = nTimeNow + nSecondsToPrecompute; // 1 second past the last timestamp to compute

    int nSecondsToScan = int(nTimeEnd - nTimeBegin);
    if(nSecondsToScan <= 0) return true; // nothing to do
    for (map<pair<uint256, unsigned int>, StakeCandidate >::iterator it = mapStakeCandidates.begin(); it != mapStakeCandidates.end() ; ++it)
    {
        StakeCandidate& candidate = it->second;
        ScanCoinStakeCandidate(candidate, nCandidateBits, nTimeBegin, (unsigned int)nSecondsToScan);
    }

    nPrecomputedCandidateTime = nTimeNow;

    printf("[TurboStake] Precompute: Scanned %u candidates, schedule has %u candidates now.\n", mapStakeCandidates.size(), stakingSchedule.size() );

    return true;
}

bool CWallet::ScanCoinStakeCandidate(const StakeCandidate& candidate, unsigned int nBits, unsigned int nTimeFrom, unsigned int nSecondsToScan)
{
    COutPoint prevoutStake = COutPoint(candidate.txHash, candidate.idxTxout);
    std::vector<unsigned int> results;

    ScanStakeKernelHashWithStakeModifier(
                nBits,
                candidate.blockFrom,
                candidate.txPosInBlock,
                candidate.tx,
                prevoutStake,
                nTimeFrom,
                nSecondsToScan,
                candidate.stakeModifier,
                results);

    BOOST_FOREACH(unsigned int nTime, results)
    {
        stakingSchedule.insert(pair<unsigned int, std::pair<uint256, unsigned int> >(
                                   nTime,
                                   pair<uint256, unsigned int> (
                                       candidate.txHash,
                                       candidate.idxTxout)));
    }

    return true;
}


bool CWallet::UpdateCoinStakeCandidatesFromWallet()
{
    vector<pair<CTransaction, unsigned int> > selectedCoins;

    // Select the coins and put them into the selectedCoins vector.
    // We store them instead of processing them right away in order to minimize the time
    // we have to keep cs_main and cs_wallet locked.
    {
        LOCK2(cs_main, cs_wallet);

        // Choose coins to use
        int64 nBalance = GetBalance();
        int64 nReserveBalance = 0;
        if (mapArgs.count("-reservebalance") && !ParseMoney(mapArgs["-reservebalance"], nReserveBalance))
            return error("CreateCoinStake : invalid reserve balance amount");
        if (fDebug && GetBoolArg("-printcoinstake"))
            printf("CreateCoinStake reservebalance %u\n", nReserveBalance);
        if (nBalance <= nReserveBalance)
            return false;
        vector<pair<const CWalletTx*,unsigned int> > vCoins;
        int64 nValueIn = 0;
        if (!SelectCoinsForPoS(nBalance - nReserveBalance, GetAdjustedTime(), vCoins, nValueIn))
            return false;
        if (vCoins.empty())
            return false;

        selectedCoins.reserve(vCoins.size());
        double totalEffectiveStakeWeight = 0.0;
        int64 totalEligibleBalance = 0;
        unsigned int nSmallCoins = 0;
        int64 nTimeTx = GetAdjustedTime();

        BOOST_FOREACH(PAIRTYPE(const CWalletTx*, unsigned int) pcoin, vCoins)
        {
            int64 nTimeWeight = min(nTimeTx - (int64)pcoin.first->nTime, (int64)STAKE_MAX_AGE) - nStakeMinAge;
            double txEffectiveStakeWeight = double(nTimeWeight) / double(STAKE_MAX_AGE-nStakeMinAge);

            int64 txValue = pcoin.first->vout[pcoin.second].nValue;
            if(txValue < 5*COIN) ++nSmallCoins;
            totalEligibleBalance += txValue;
            txEffectiveStakeWeight *= txValue;
            totalEffectiveStakeWeight += txEffectiveStakeWeight;

            selectedCoins.push_back(pair<CTransaction, unsigned int>(*(pcoin.first), pcoin.second));

//            if(nTimeTx - (int64)pcoin.first->nTime >= (int64)STAKE_MAX_AGE)
//                vCoinsEligibleForCombining.push_back(pcoin);
        }

        printf("[TurboStake] Effective stake weight is %0.2lf SLM.\n"
               "             Total staking balance is %0.2lf SLM.\n"
               "             Average stake weight: %0.1lf%%\n"
               "             Small/total txouts: %u/%u\n",
               totalEffectiveStakeWeight/COIN,
               double(totalEligibleBalance)/COIN,
               100.0*totalEffectiveStakeWeight/totalEligibleBalance,
               nSmallCoins, vCoins.size());
    }

    // Collect all selected coins into a map
    map<pair<uint256, unsigned int>, StakeCandidate > selectedStakeCandidates;
    BOOST_FOREACH(PAIRTYPE(CTransaction, unsigned int) pcoin, selectedCoins)
    {
        StakeCandidate candidate;
        candidate.tx = pcoin.first;
        candidate.idxTxout = pcoin.second;
        candidate.txHash = pcoin.first.GetHash();
        selectedStakeCandidates[pair<uint256, unsigned int>(candidate.txHash, candidate.idxTxout)] = candidate;
    }

    // Delete obsolete coins from mapStakeCandidates
    for (map<pair<uint256, unsigned int>, StakeCandidate >::iterator it = mapStakeCandidates.begin(); it != mapStakeCandidates.end() ; /*nop*/)
    {
        if (selectedStakeCandidates.find(it->first) == selectedStakeCandidates.end())
            mapStakeCandidates.erase(it++);
        else ++it;
    }

    // Put all NEW coins into mapStakeCandidates along with all required meta-information-
    for (map<pair<uint256, unsigned int>, StakeCandidate >::iterator it = selectedStakeCandidates.begin(); it != selectedStakeCandidates.end() ; ++it)
    {
        if(mapStakeCandidates.find(it->first) != mapStakeCandidates.end()) continue; // we already know this coin, no need to do anything
        StakeCandidate candidate = it->second;

        // --- Compute all the metadata for the new candidate ---
        CTxIndex txindex;
        {
            CTxDB txdb("r");
            if (!txdb.ReadTxIndex(candidate.txHash, txindex))
                continue;
            candidate.txPosInBlock = txindex.pos.nTxPos - txindex.pos.nBlockPos;
        }

        CBlock block; // read block header only
        if (!block.ReadFromDisk(txindex.pos.nFile, txindex.pos.nBlockPos, false))
            continue;

        candidate.blockFrom = block;
        candidate.blockHash = block.GetHash();

        {
            int nStakeModifierHeight;
            int64 nStakeModifierTime;

            LOCK(cs_main);
            if (!GetKernelStakeModifier(candidate.blockHash, candidate.tx.nTime, candidate.stakeModifier, nStakeModifierHeight, nStakeModifierTime, false))
                continue;
        }

        // TODO: Precompute PoS hashes for the new candidate in order to keep up with the existing ones.

        // Store candidate entry for new coin
        mapStakeCandidates[it->first] = candidate;
    }


    return true;
}



bool CWallet::CreateCoinStakeWithSchedule(const CKeyStore& keystore, unsigned int nBits, CTransaction& txNew)
{
    boost::chrono::time_point<boost::chrono::steady_clock> timeStart, timeEnd, timeStartTotal, timeEndTotal;
    timeStartTotal = boost::chrono::steady_clock::now();
    // Delete obsolete schedule entries
    {
        std::multimap<unsigned int, std::pair<uint256, unsigned int> > ::iterator itScheduleObsoleteEnd = stakingSchedule.upper_bound(txNew.nTime - 120);
        if(itScheduleObsoleteEnd != stakingSchedule.begin())
        {
            printf("CreateCoinStakeWithSchedule: deleting obsolete schedule entries. ");
            stakingSchedule.erase(stakingSchedule.begin(), itScheduleObsoleteEnd);
        }
    }

    // Get all potential minting opportunities until the time of the coinstake transaction
    std::multimap<unsigned int, std::pair<uint256, unsigned int> > ::iterator itScheduleEnd = stakingSchedule.upper_bound(txNew.nTime);
    if(itScheduleEnd == stakingSchedule.begin()) return false; // No candidate available => exit early.


    // The following split & combine thresholds are important to security
    // Should not be adjusted if you don't understand the consequences
    int64 nCombineThreshold = 36 * COIN; // Max combined value of txins added to the PoS tx.
    int64 nSmallCoinThreshold = 450 * CENT; // Txouts below this value are always added to the tx, ignoring nCombineThreshold and maturity
    int64 nDesiredValuePerOutput = 40 * COIN; // Target value of the PoS txouts. If the total output value exceeds this, multiple outputs will be created.
    static unsigned int stakeMaxCombineCount = 100; // Max allowed number of txins of the PoS tx.
    static unsigned int stakeMaxTxoutCount = 100; // Max allowed number of txouts of the PoS tx.

    CBigNum bnTargetPerCoinDay;
    bnTargetPerCoinDay.SetCompact(nBits);

    LOCK2(cs_main, cs_wallet);
    txNew.vin.clear();
    txNew.vout.clear();
    // Mark coin stake transaction
    CScript scriptEmpty;
    scriptEmpty.clear();
    txNew.vout.push_back(CTxOut(0, scriptEmpty));
    // Choose coins to use
    int64 nBalance = GetBalance();
    int64 nReserveBalance = 0;
    if (mapArgs.count("-reservebalance") && !ParseMoney(mapArgs["-reservebalance"], nReserveBalance))
        return error("CreateCoinStake : invalid reserve balance amount");
    if (fDebug && GetBoolArg("-printcoinstake"))
        printf("CreateCoinStake reservebalance %u\n", nReserveBalance);
    if (nBalance <= nReserveBalance)
        return false;

    vector<const CWalletTx*> vwtxPrev;
    int64 nCredit = 0;
    CScript scriptPubKeyKernel, scriptPubKeyOut;
    //BOOST_FOREACH(PAIRTYPE(const CWalletTx*, unsigned int) pcoin, setCoins)
    std::multimap<unsigned int, std::pair<uint256, unsigned int> >::iterator itSchedule;
    for(itSchedule = stakingSchedule.begin();
        itSchedule != itScheduleEnd;
        ++itSchedule)
    {
        PAIRTYPE(const CWalletTx*, unsigned int) pcoin;
        {
            std::map<uint256, CWalletTx>::iterator itWallet = mapWallet.find(itSchedule->second.first);
            if(itWallet == mapWallet.end()) continue; // we don't have that transaction anymore...
            pcoin.first = &(itWallet->second);
            pcoin.second = itSchedule->second.second;
        }


        CTxDB txdb("r");
        CTxIndex txindex;
        if (!txdb.ReadTxIndex(pcoin.first->GetHash(), txindex))
            continue;

        // Read block header
        CBlock block;
        if (!block.ReadFromDisk(txindex.pos.nFile, txindex.pos.nBlockPos, false))
            continue;
        /* FIXME: reported as unused
        CBlockIndex *pindex = mapBlockIndex[pcoin.first->hashBlock];
        */

        bool fKernelFound = false;
        uint256 hashProofOfStake = 0;
        COutPoint prevoutStake = COutPoint(pcoin.first->GetHash(), pcoin.second);

        bool fKernelCandidateFound = false;
        unsigned int nTimeCandidateTx = itSchedule->first;

        if (block.GetBlockTime() + nStakeMinAge > nTimeCandidateTx)
            continue; // only count coins meeting min age requirement

        fKernelCandidateFound = CheckStakeKernelHash(nBits, block, txindex.pos.nTxPos - txindex.pos.nBlockPos, *pcoin.first, prevoutStake, nTimeCandidateTx, hashProofOfStake);

        scriptPubKeyOut = CScript();
        if (fKernelCandidateFound)
        {
            bool fPrintCoinStake = (fDebug && GetBoolArg("-printcoinstake"));
            // Found a kernel
            if (fPrintCoinStake)
                printf("CreateCoinStake : kernel found\n");
            vector<valtype> vSolutions;
            txnouttype whichType;
            scriptPubKeyKernel = pcoin.first->vout[pcoin.second].scriptPubKey;
            if (!Solver(scriptPubKeyKernel, whichType, vSolutions))
            {
                if (fPrintCoinStake)
                    printf("CreateCoinStake : failed to parse kernel\n", whichType);
                break;
            }
            if (fPrintCoinStake)
                printf("CreateCoinStake : parsed kernel type=%d\n", whichType);
            if (whichType != TX_PUBKEY && whichType != TX_PUBKEYHASH)
            {
                if (fPrintCoinStake)
                    printf("CreateCoinStake : no support for kernel type=%d\n", whichType);
                break;  // only support pay to public key and pay to address
            }
            if (whichType == TX_PUBKEYHASH) // pay to address type
            {
                // convert to pay to public key type
                CKey key;
                if (!keystore.GetKey(uint160(vSolutions[0]), key))
                {
                    if (fPrintCoinStake)
                        printf("CreateCoinStake : failed to get key for kernel type=%d\n", whichType);
                    break;  // unable to find corresponding public key
                }
                scriptPubKeyOut << key.GetPubKey() << OP_CHECKSIG;
            }
            else
                scriptPubKeyOut = scriptPubKeyKernel;

            txNew.nTime = nTimeCandidateTx;
            txNew.vin.push_back(CTxIn(pcoin.first->GetHash(), pcoin.second));
            nCredit += pcoin.first->vout[pcoin.second].nValue;
            vwtxPrev.push_back(pcoin.first);
            if (fDebug && GetBoolArg("-printcoinstake"))
                printf("CreateCoinStake : added kernel type=%d\n", whichType);
            fKernelFound = true;
            break;
        }
        if (fKernelFound || fShutdown)
            break; // if kernel is found stop searching
    }

    // erase all candidates that did not yield a valid kernel from the schedule.
    stakingSchedule.erase(stakingSchedule.begin(), itSchedule);


    if (nCredit == 0 || nCredit > nBalance - nReserveBalance)
        return false;

    timeStart = boost::chrono::steady_clock::now();
    set<pair<const CWalletTx*,unsigned int> > setCoins;
    int64 nValueIn = 0;
    if (!SelectCoins(nBalance - nReserveBalance, txNew.nTime, setCoins, nValueIn))
        return false;
    if (setCoins.empty())
        return false;
    timeEnd = boost::chrono::steady_clock::now();
    printf("[Stakeperf] Minting: SelectCoins for adding more inputs took %0.2lf seconds.\n", boost::chrono::duration<float>(timeEnd-timeStart).count());

    timeStart = boost::chrono::steady_clock::now();
    int nInputsScanned = 0;
    int nInputsAdded = 0;
    int nInputsSkippedWrongAddr = 0;
    int nInputsSkippedValue = 0;
    int nInputsSkippedAge = 0;
    BOOST_FOREACH(PAIRTYPE(const CWalletTx*, unsigned int) pcoin, setCoins)
    {
        ++nInputsScanned;
        // Attempt to add more inputs
        // Only add coins of the same key/address as kernel
        if (((pcoin.first->vout[pcoin.second].scriptPubKey == scriptPubKeyKernel || pcoin.first->vout[pcoin.second].scriptPubKey == txNew.vout[1].scriptPubKey))
            && pcoin.first->GetHash() != txNew.vin[0].prevout.hash)
        {
            // Small coins ignore age and nCombineThreshold limits.
            // Processing them takes too much CPU time so we need to get rid of them.
            bool isSmallCoin = pcoin.first->vout[pcoin.second].nValue < nSmallCoinThreshold;

            // Stop adding more inputs if already too many inputs
            if (txNew.vin.size() >= stakeMaxCombineCount)
                break;
            // Stop adding more inputs if value is already pretty significant
            if (!isSmallCoin && nCredit > nCombineThreshold)
                continue;
            // Stop adding inputs if reached reserve limit
            if (nCredit + pcoin.first->vout[pcoin.second].nValue > nBalance - nReserveBalance)
                break;
            // Do not add additional significant input
            if (pcoin.first->vout[pcoin.second].nValue > nCombineThreshold)
            {
                ++nInputsSkippedValue;
                continue;
            }
            // Do not add input that is still too young
            if (!isSmallCoin && pcoin.first->nTime + STAKE_MAX_AGE > txNew.nTime)
            {
                ++nInputsSkippedAge;
                continue;
            }
            ++nInputsAdded;
            txNew.vin.push_back(CTxIn(pcoin.first->GetHash(), pcoin.second));
            nCredit += pcoin.first->vout[pcoin.second].nValue;
            vwtxPrev.push_back(pcoin.first);
        }
        else ++nInputsSkippedWrongAddr;
    }
    timeEnd = boost::chrono::steady_clock::now();
    printf("[Stakeperf] Minting: Adding more inputs took %0.2lf seconds.\n", boost::chrono::duration<float>(timeEnd-timeStart).count());
    printf("[Stakeperf] Minting: Added %i/%i inputs. Skipped: %i addr | %i value | %i age\n",
           nInputsAdded,nInputsScanned,nInputsSkippedWrongAddr,nInputsSkippedValue,nInputsSkippedAge);
    // Calculate coin age reward
    int64 nCreditIn = nCredit;
    {
        uint64 nCoinAge;
        CTxDB txdb("r");
        if (!txNew.GetCoinAge(txdb, nCoinAge))
            return error("CreateCoinStake : failed to calculate coin age");
        nCredit += GetProofOfStakeReward(nCoinAge, txNew.nTime);
    }

    timeStart = boost::chrono::steady_clock::now();
    int64 nMinFee = 0;
    for(;;)
    {
        // Set outputs
        {
            txNew.vout.clear();
            txNew.vout.push_back(CTxOut(0, scriptEmpty));
            int64 valueRemaining = nCredit - nMinFee;
            int64 nTotalOutputs = std::min((valueRemaining-1)/nDesiredValuePerOutput + 1, // ceil(valueRemaining/nDesiredValuePerOutput)
                                           (int64)stakeMaxTxoutCount);

            int64 nValuePerOutput = valueRemaining / nTotalOutputs;

            printf("[Stakeperf] Minting: Adding %li outputs with value %li. Totals: %li in | %li out\n",
                   nTotalOutputs,nValuePerOutput,nCreditIn,valueRemaining);

            for(int64 nOutput = 0; nOutput < nTotalOutputs-1 && valueRemaining >= nValuePerOutput; ++nOutput)
            {
                txNew.vout.push_back(CTxOut(nValuePerOutput, scriptPubKeyOut));
                valueRemaining -= nValuePerOutput;
            }
            txNew.vout.push_back(CTxOut(valueRemaining, scriptPubKeyOut));
        }


        // Sign
        int nIn = 0;
        BOOST_FOREACH(const CWalletTx* pcoin, vwtxPrev)
        {
            if (!SignSignature(*this, *pcoin, txNew, nIn++))
                return error("CreateCoinStake : failed to sign coinstake");
        }

        // Limit size
        unsigned int nBytes = ::GetSerializeSize(txNew, SER_NETWORK, PROTOCOL_VERSION);
        if (nBytes >= MAX_BLOCK_SIZE_GEN/5)
            return error("CreateCoinStake : exceeded coinstake size limit");

        // Check enough fee is paid
        if (nMinFee < txNew.GetMinFee() - MIN_TX_FEE)
        {
            nMinFee = txNew.GetMinFee() - MIN_TX_FEE;
            continue; // try signing again
        }
        else
        {
            if (fDebug && GetBoolArg("-printfee"))
                printf("CreateCoinStake : fee for coinstake %s\n", FormatMoney(nMinFee).c_str());
            break;
        }
    }

    timeEnd = boost::chrono::steady_clock::now();
    printf("[Stakeperf] Minting: Adding outputs took %0.2lf seconds.\n", boost::chrono::duration<float>(timeEnd-timeStart).count());

    // Erase the successful PoS candidate from the schedule so it doesn't get selected again.
    std::pair<uint256, unsigned int> candidateToDelete = itSchedule->second;
    stakingSchedule.erase(itSchedule);
    for(std::multimap<unsigned int, std::pair<uint256, unsigned int> >::iterator it = stakingSchedule.begin();
        it != stakingSchedule.end();
        /*nop*/)
    {
        if(it->second == candidateToDelete) stakingSchedule.erase(it++);
        else ++it;
    }

    // Delete the successful PoS candidate from the stake candidate cache too
    mapStakeCandidates.erase(candidateToDelete);

    timeEndTotal = boost::chrono::steady_clock::now();
    printf("[Stakeperf] Minting: Entire process took %0.2lf seconds.\n", boost::chrono::duration<float>(timeEndTotal-timeStartTotal).count());

    // Successfully generated coinstake
    return true;
}




// ppcoin: create coin stake transaction
bool CWallet::CreateCoinStake(const CKeyStore& keystore, unsigned int nBits, int64 nSearchInterval, CTransaction& txNew)
{
    // The following split & combine thresholds are important to security
    // Should not be adjusted if you don't understand the consequences
    static unsigned int nStakeSplitAge = (60 * 60 * 24 * 90);
    int64 nCombineThreshold = 20 * COIN; //GetProofOfWorkReward(GetLastBlockIndex(pindexBest, false)->nBits) / 3;
    static unsigned int stakeMaxCombineCount = 100;

    CBigNum bnTargetPerCoinDay;
    bnTargetPerCoinDay.SetCompact(nBits);

    LOCK2(cs_main, cs_wallet);
    txNew.vin.clear();
    txNew.vout.clear();
    // Mark coin stake transaction
    CScript scriptEmpty;
    scriptEmpty.clear();
    txNew.vout.push_back(CTxOut(0, scriptEmpty));
    // Choose coins to use
    int64 nBalance = GetBalance();
    int64 nReserveBalance = 0;
    if (mapArgs.count("-reservebalance") && !ParseMoney(mapArgs["-reservebalance"], nReserveBalance))
        return error("CreateCoinStake : invalid reserve balance amount");
    if (fDebug && GetBoolArg("-printcoinstake"))
        printf("CreateCoinStake reservebalance %u\n", nReserveBalance);
    if (nBalance <= nReserveBalance)
        return false;
    set<pair<const CWalletTx*,unsigned int> > setCoins;
    vector<const CWalletTx*> vwtxPrev;
    int64 nValueIn = 0;
    if (!SelectCoins(nBalance - nReserveBalance, txNew.nTime, setCoins, nValueIn))
        return false;
    if (setCoins.empty())
        return false;

    // delete old stake modifiers from cache
    {
        std::map<uint256, StakeModifierCacheEntry>::iterator it = mapStakeModifierCacheV03.begin();
        while(it != mapStakeModifierCacheV03.end())
        {
            StakeModifierCacheEntry& entry = it->second;
            ++entry.entryAge;
            if(entry.entryAge > 1000)
            {
                mapStakeModifierCacheV03.erase(it++);
            }
            else ++it;
        }
    }


    int64 nCredit = 0;
    CScript scriptPubKeyKernel;
    BOOST_FOREACH(PAIRTYPE(const CWalletTx*, unsigned int) pcoin, setCoins)
    {
        CTxDB txdb("r");
        CTxIndex txindex;
        if (!txdb.ReadTxIndex(pcoin.first->GetHash(), txindex))
            continue;

        // Read block header
        CBlock block;
        if (!block.ReadFromDisk(txindex.pos.nFile, txindex.pos.nBlockPos, false))
            continue;
        /* FIXME: reported as unused
        CBlockIndex *pindex = mapBlockIndex[pcoin.first->hashBlock];
        */
        static int nMaxStakeSearchInterval = 2;
        if (block.GetBlockTime() + nStakeMinAge > txNew.nTime - nMaxStakeSearchInterval)
            continue; // only count coins meeting min age requirement

        bool fKernelFound = false;
        for (unsigned int n=0; n<min(nSearchInterval,(int64)nMaxStakeSearchInterval) && !fKernelFound && !fShutdown; n++)
        {
            // Search backward in time from the given txNew timestamp
            // Search nSearchInterval seconds back up to nMaxStakeSearchInterval
            uint256 hashProofOfStake = 0;
            COutPoint prevoutStake = COutPoint(pcoin.first->GetHash(), pcoin.second);

            bool fKernelCandidateFound = false;
            unsigned int nTimeCandidateTx = txNew.nTime - n;
            if(IsProtocolV05(nTimeCandidateTx))
            {
                // There is no optimized implementation for V05 yet => default to the old implementation
                fKernelCandidateFound = CheckStakeKernelHash(nBits, block, txindex.pos.nTxPos - txindex.pos.nBlockPos, *pcoin.first, prevoutStake, nTimeCandidateTx, hashProofOfStake);
            }
            else
            {
                fKernelCandidateFound = CheckStakeKernelHashWithCacheV03(nBits, block, txindex.pos.nTxPos - txindex.pos.nBlockPos, *pcoin.first, prevoutStake, nTimeCandidateTx, hashProofOfStake);
            }

            if (fKernelCandidateFound)
            {
                bool fPrintCoinStake = (fDebug && GetBoolArg("-printcoinstake"));
                // Found a kernel
                if (fPrintCoinStake)
                    printf("CreateCoinStake : kernel found\n");
                vector<valtype> vSolutions;
                txnouttype whichType;
                CScript scriptPubKeyOut;
                scriptPubKeyKernel = pcoin.first->vout[pcoin.second].scriptPubKey;
                if (!Solver(scriptPubKeyKernel, whichType, vSolutions))
                {
                    if (fPrintCoinStake)
                        printf("CreateCoinStake : failed to parse kernel\n", whichType);
                    break;
                }
                if (fPrintCoinStake)
                    printf("CreateCoinStake : parsed kernel type=%d\n", whichType);
                if (whichType != TX_PUBKEY && whichType != TX_PUBKEYHASH)
                {
                    if (fPrintCoinStake)
                        printf("CreateCoinStake : no support for kernel type=%d\n", whichType);
                    break;  // only support pay to public key and pay to address
                }
                if (whichType == TX_PUBKEYHASH) // pay to address type
                {
                    // convert to pay to public key type
                    CKey key;
                    if (!keystore.GetKey(uint160(vSolutions[0]), key))
                    {
                        if (fPrintCoinStake)
                            printf("CreateCoinStake : failed to get key for kernel type=%d\n", whichType);
                        break;  // unable to find corresponding public key
                    }
                    scriptPubKeyOut << key.GetPubKey() << OP_CHECKSIG;
                }
                else
                    scriptPubKeyOut = scriptPubKeyKernel;

                txNew.nTime -= n;
                txNew.vin.push_back(CTxIn(pcoin.first->GetHash(), pcoin.second));
                nCredit += pcoin.first->vout[pcoin.second].nValue;
                vwtxPrev.push_back(pcoin.first);
                txNew.vout.push_back(CTxOut(0, scriptPubKeyOut));
                if (block.GetBlockTime() + nStakeSplitAge > txNew.nTime)
                    txNew.vout.push_back(CTxOut(0, scriptPubKeyOut)); //split stake
                if (fDebug && GetBoolArg("-printcoinstake"))
                    printf("CreateCoinStake : added kernel type=%d\n", whichType);
                fKernelFound = true;
                break;
            }
        }
        if (fKernelFound || fShutdown)
            break; // if kernel is found stop searching
    }
    if (nCredit == 0 || nCredit > nBalance - nReserveBalance)
        return false;
    BOOST_FOREACH(PAIRTYPE(const CWalletTx*, unsigned int) pcoin, setCoins)
    {
        // Attempt to add more inputs
        // Only add coins of the same key/address as kernel
        if (txNew.vout.size() == 2 && ((pcoin.first->vout[pcoin.second].scriptPubKey == scriptPubKeyKernel || pcoin.first->vout[pcoin.second].scriptPubKey == txNew.vout[1].scriptPubKey))
            && pcoin.first->GetHash() != txNew.vin[0].prevout.hash)
        {
            // Stop adding more inputs if already too many inputs
            if (txNew.vin.size() >= stakeMaxCombineCount)
                break;
            // Stop adding more inputs if value is already pretty significant
            if (nCredit > nCombineThreshold)
                break;
            // Stop adding inputs if reached reserve limit
            if (nCredit + pcoin.first->vout[pcoin.second].nValue > nBalance - nReserveBalance)
                break;
            // Do not add additional significant input
            if (pcoin.first->vout[pcoin.second].nValue > nCombineThreshold)
                continue;
            // Do not add input that is still too young
            if (pcoin.first->nTime + STAKE_MAX_AGE > txNew.nTime)
                continue;
            txNew.vin.push_back(CTxIn(pcoin.first->GetHash(), pcoin.second));
            nCredit += pcoin.first->vout[pcoin.second].nValue;
            vwtxPrev.push_back(pcoin.first);
        }
    }
    // Calculate coin age reward
    {
        uint64 nCoinAge;
        CTxDB txdb("r");
        if (!txNew.GetCoinAge(txdb, nCoinAge))
            return error("CreateCoinStake : failed to calculate coin age");
        nCredit += GetProofOfStakeReward(nCoinAge, txNew.nTime);
    }

    int64 nMinFee = 0;
    for(;;)
    {
        // Set output amount
        if (txNew.vout.size() == 3)
        {
            txNew.vout[1].nValue = ((nCredit - nMinFee) / 2 / CENT) * CENT;
            txNew.vout[2].nValue = nCredit - nMinFee - txNew.vout[1].nValue;
        }
        else
            txNew.vout[1].nValue = nCredit - nMinFee;

        // Sign
        int nIn = 0;
        BOOST_FOREACH(const CWalletTx* pcoin, vwtxPrev)
        {
            if (!SignSignature(*this, *pcoin, txNew, nIn++))
                return error("CreateCoinStake : failed to sign coinstake");
        }

        // Limit size
        unsigned int nBytes = ::GetSerializeSize(txNew, SER_NETWORK, PROTOCOL_VERSION);
        if (nBytes >= MAX_BLOCK_SIZE_GEN/5)
            return error("CreateCoinStake : exceeded coinstake size limit");

        // Check enough fee is paid
        if (nMinFee < txNew.GetMinFee() - MIN_TX_FEE)
        {
            nMinFee = txNew.GetMinFee() - MIN_TX_FEE;
            continue; // try signing again
        }
        else
        {
            if (fDebug && GetBoolArg("-printfee"))
                printf("CreateCoinStake : fee for coinstake %s\n", FormatMoney(nMinFee).c_str());
            break;
        }
    }

    // Successfully generated coinstake
    return true;
}

// Call after CreateTransaction unless you want to abort
bool CWallet::CommitTransaction(CWalletTx& wtxNew, CReserveKey& reservekey, bool fBurnTx)
{
    {
        LOCK2(cs_main, cs_wallet);

        //CommitTransaction will not let a burn transaction not pass if they do not match
        if (wtxNew.IsBurnTx() != fBurnTx)
            return false;

        printf("CommitTransaction:\n%s", wtxNew.ToString().c_str());
        {
            // This is only to keep the database open to defeat the auto-flush for the
            // duration of this scope.  This is the only place where this optimization
            // maybe makes sense; please don't do it anywhere else.
            CWalletDB* pwalletdb = fFileBacked ? new CWalletDB(strWalletFile,"r") : NULL;

            // Take key pair from key pool so it won't be used again
            reservekey.KeepKey();

            // Add tx to wallet, because if it has change it's also ours,
            // otherwise just for transaction history.
            AddToWallet(wtxNew, fBurnTx);

            // Mark old coins as spent
            set<CWalletTx*> setCoins;
            BOOST_FOREACH(const CTxIn& txin, wtxNew.vin)
            {
                CWalletTx &coin = mapWallet[txin.prevout.hash];
                coin.BindWallet(this);
                coin.MarkSpent(txin.prevout.n);
                coin.WriteToDisk();
                vWalletUpdated.push_back(coin.GetHash());
            }

            if (fFileBacked)
                delete pwalletdb;
        }

        // Track how many getdata requests our transaction gets
        mapRequestCount[wtxNew.GetHash()] = 0;

        // Broadcast
        if (!wtxNew.AcceptToMemoryPool())
        {
            // This must not fail. The transaction has already been signed and recorded.
            printf("CommitTransaction() : Error: Transaction not valid");
            return false;
        }
        wtxNew.RelayWalletTransaction();
    }
    MainFrameRepaint();
    return true;
}




string CWallet::SendMoney(CScript scriptPubKey, int64 nValue, CWalletTx& wtxNew, bool fAskFee, bool fBurnTx)
{
    CReserveKey reservekey(this);
    int64 nFeeRequired;

    if (IsLocked())
    {
        string strError = _("Error: Wallet locked, unable to create transaction  ");
        printf("SendMoney() : %s", strError.c_str());
        return strError;
    }
    if (fWalletUnlockMintOnly)
    {
        string strError = _("Error: Wallet unlocked for block minting only, unable to create transaction.");
        printf("SendMoney() : %s", strError.c_str());
        return strError;
    }
    if (!CreateTransaction(scriptPubKey, nValue, wtxNew, reservekey, nFeeRequired))
    {
        string strError;
        if (nValue + nFeeRequired > GetBalance())
            strError = strprintf(_("Error: This transaction requires a transaction fee of at least %s because of its amount, complexity, or use of recently received funds  "), FormatMoney(nFeeRequired).c_str());
        else
            strError = _("Error: Transaction creation failed  ");
        printf("SendMoney() : %s", strError.c_str());
        return strError;
    }

    if (fAskFee && !ThreadSafeAskFee(nFeeRequired, _("Sending...")))
        return "ABORTED";

    if (!CommitTransaction(wtxNew, reservekey, fBurnTx))
    {
        //both error messages have this text
        string strError = "if some of the coins in your wallet were already spent, such as if you used a copy of wallet.dat and coins were spent in the copy but not marked as spent here.";

        if (fBurnTx)
            return _("Error: The transaction was rejected. This might happen if this burn transaction "
                             "was not properly created or ") + strError;
        else
            return _("Error: The transaction was rejected. This might happen ")  + strError;

    }

    MainFrameRepaint();
    return "";
}



string CWallet::SendMoneyToDestination(const CTxDestination& address, int64 nValue, CWalletTx& wtxNew, bool fAskFee, bool fBurnTx)
{
    // Check amount
    if (nValue <= 0)
        return _("Invalid amount");
    if (nValue + nTransactionFee > GetBalance())
        return _("Insufficient funds");

    // Parse bitcoin address
    CScript scriptPubKey;
    scriptPubKey.SetDestination(address);

    return SendMoney(scriptPubKey, nValue, wtxNew, fAskFee, fBurnTx);
}




int CWallet::LoadWallet(bool& fFirstRunRet)
{
    if (!fFileBacked)
        return false;
    fFirstRunRet = false;
    int nLoadWalletRet = CWalletDB(strWalletFile,"cr+").LoadWallet(this);
    if (nLoadWalletRet == 5)
    {
        if (CDB::Rewrite(strWalletFile, "\x04pool"))
        {
            setKeyPool.clear();
            // Note: can't top-up keypool here, because wallet is locked.
            // User will be prompted to unlock wallet the next operation
            // the requires a new key.
        }
        nLoadWalletRet = 5;
    }

    if (nLoadWalletRet != 0)
        return nLoadWalletRet;
    fFirstRunRet = !vchDefaultKey.IsValid();

    CreateThread(ThreadFlushWalletDB, &strWalletFile);
    return DB_LOAD_OK;
}

int CWallet::ZapWalletTx(std::vector<CWalletTx>& vWtx)
{
    if (!fFileBacked)
        return 0;
    int nZapWalletTxRet = CWalletDB(strWalletFile,"cr+").ZapWalletTx(this, vWtx);
    if (nZapWalletTxRet == 5)
    {
        if (CDB::Rewrite(strWalletFile, "\x04pool"))
        {
            LOCK(cs_wallet);
            setKeyPool.clear();
            // Note: can't top-up keypool here, because wallet is locked.
            // User will be prompted to unlock wallet the next operation
            // that requires a new key.
        }
    }

    if (nZapWalletTxRet != 0)
        return nZapWalletTxRet;

    return 0;
}


bool CWallet::SetAddressBookName(const CTxDestination& address, const string& strName)
{
    mapAddressBook[address] = strName;
    AddressBookRepaint();
    if (!fFileBacked)
        return false;
    return CWalletDB(strWalletFile).WriteName(CBitcoinAddress(address).ToString(), strName);
}

bool CWallet::DelAddressBookName(const CTxDestination& address)
{
    mapAddressBook.erase(address);
    AddressBookRepaint();
    if (!fFileBacked)
        return false;
    return CWalletDB(strWalletFile).EraseName(CBitcoinAddress(address).ToString());
}


void CWallet::PrintWallet(const CBlock& block)
{
    {
        LOCK(cs_wallet);
        if (block.IsProofOfWork() && mapWallet.count(block.vtx[0].GetHash()))
        {
            CWalletTx& wtx = mapWallet[block.vtx[0].GetHash()];
            printf("    mine:  %d  %d  %s", wtx.GetDepthInMainChain(), wtx.GetBlocksToMaturity(), FormatMoney(wtx.GetCredit()).c_str());
        }
        if (block.IsProofOfStake() && mapWallet.count(block.vtx[1].GetHash()))
        {
            CWalletTx& wtx = mapWallet[block.vtx[1].GetHash()];
            printf("    stake: %d  %d  %s", wtx.GetDepthInMainChain(), wtx.GetBlocksToMaturity(), FormatMoney(wtx.GetCredit()).c_str());
        }
    }
    printf("\n");
}

bool CWallet::GetTransaction(const uint256 &hashTx, CWalletTx& wtx)
{
    {
        LOCK(cs_wallet);
        map<uint256, CWalletTx>::iterator mi = mapWallet.find(hashTx);
        if (mi != mapWallet.end())
        {
            wtx = (*mi).second;
            return true;
        }
    }
    return false;
}

bool CWallet::SetDefaultKey(const CPubKey &vchPubKey)
{
    if (fFileBacked)
    {
        if (!CWalletDB(strWalletFile).WriteDefaultKey(vchPubKey))
            return false;
    }
    vchDefaultKey = vchPubKey;
    return true;
}

bool GetWalletFile(CWallet* pwallet, string &strWalletFileOut)
{
    if (!pwallet->fFileBacked)
        return false;
    strWalletFileOut = pwallet->strWalletFile;
    return true;
}

//
// Mark old keypool keys as used,
// and generate all new keys
//
bool CWallet::NewKeyPool()
{
    {
        LOCK(cs_wallet);
        CWalletDB walletdb(strWalletFile);
        BOOST_FOREACH(int64 nIndex, setKeyPool)
            walletdb.ErasePool(nIndex);
        setKeyPool.clear();

        if (IsLocked())
            return false;

        int64 nKeys = max(GetArg("-keypool", 100), (int64)0);
        for (int i = 0; i < nKeys; i++)
        {
            int64 nIndex = i+1;
            walletdb.WritePool(nIndex, CKeyPool(GenerateNewKey()));
            setKeyPool.insert(nIndex);
        }
        printf("CWallet::NewKeyPool wrote %lld new keys\n", nKeys);
    }
    return true;
}

bool CWallet::TopUpKeyPool()
{
    {
        LOCK(cs_wallet);

        if (IsLocked())
            return false;

        CWalletDB walletdb(strWalletFile);

        // Top up key pool
        unsigned int nTargetSize = max(GetArg("-keypool", 100), 0LL);
        while (setKeyPool.size() < (nTargetSize + 1))
        {
            int64 nEnd = 1;
            if (!setKeyPool.empty())
                nEnd = *(--setKeyPool.end()) + 1;
            if (!walletdb.WritePool(nEnd, CKeyPool(GenerateNewKey())))
                throw runtime_error("TopUpKeyPool() : writing generated key failed");
            setKeyPool.insert(nEnd);
            printf("keypool added key %lld, size=%d\n", nEnd, setKeyPool.size());
        }
    }
    return true;
}

void CWallet::ReserveKeyFromKeyPool(int64& nIndex, CKeyPool& keypool)
{
    nIndex = -1;
    keypool.vchPubKey = CPubKey();
    {
        LOCK(cs_wallet);

        if (!IsLocked())
            TopUpKeyPool();

        // Get the oldest key
        if(setKeyPool.empty())
            return;

        CWalletDB walletdb(strWalletFile);

        nIndex = *(setKeyPool.begin());
        setKeyPool.erase(setKeyPool.begin());
        if (!walletdb.ReadPool(nIndex, keypool))
            throw runtime_error("ReserveKeyFromKeyPool() : read failed");
        if (!HaveKey(keypool.vchPubKey.GetID()))
            throw runtime_error("ReserveKeyFromKeyPool() : unknown key in key pool");
        assert(keypool.vchPubKey.IsValid());
        if (fDebug && GetBoolArg("-printkeypool"))
            printf("keypool reserve %lld\n", nIndex);
    }
}

int64 CWallet::AddReserveKey(const CKeyPool& keypool)
{
    {
        LOCK2(cs_main, cs_wallet);
        CWalletDB walletdb(strWalletFile);

        int64 nIndex = 1 + *(--setKeyPool.end());
        if (!walletdb.WritePool(nIndex, keypool))
            throw runtime_error("AddReserveKey() : writing added key failed");
        setKeyPool.insert(nIndex);
        return nIndex;
    }
    return -1;
}

void CWallet::KeepKey(int64 nIndex)
{
    // Remove from key pool
    if (fFileBacked)
    {
        CWalletDB walletdb(strWalletFile);
        walletdb.ErasePool(nIndex);
    }
    printf("keypool keep %lld\n", nIndex);
}

void CWallet::ReturnKey(int64 nIndex)
{
    // Return to key pool
    {
        LOCK(cs_wallet);
        setKeyPool.insert(nIndex);
    }
    if (fDebug && GetBoolArg("-printkeypool"))
        printf("keypool return %d\n", nIndex);
}

bool CWallet::GetKeyFromPool(CPubKey& result, bool fAllowReuse)
{
    int64 nIndex = 0;
    CKeyPool keypool;
    {
        LOCK(cs_wallet);
        ReserveKeyFromKeyPool(nIndex, keypool);
        if (nIndex == -1)
        {
            if (fAllowReuse && vchDefaultKey.IsValid())
            {
                result = vchDefaultKey;
                return true;
            }
            if (IsLocked()) return false;
            result = GenerateNewKey();
            return true;
        }
        KeepKey(nIndex);
        result = keypool.vchPubKey;
    }
    return true;
}

int64 CWallet::GetOldestKeyPoolTime()
{
    int64 nIndex = 0;
    CKeyPool keypool;
    ReserveKeyFromKeyPool(nIndex, keypool);
    if (nIndex == -1)
        return GetTime();
    ReturnKey(nIndex);
    return keypool.nTime;
}

CPubKey CReserveKey::GetReservedKey()
{
    if (nIndex == -1)
    {
        CKeyPool keypool;
        pwallet->ReserveKeyFromKeyPool(nIndex, keypool);
        if (nIndex != -1)
            vchPubKey = keypool.vchPubKey;
        else
        {
            printf("CReserveKey::GetReservedKey(): Warning: using default key instead of a new key, top up your keypool.");
            vchPubKey = pwallet->vchDefaultKey;
        }
    }
    assert(vchPubKey.IsValid());
    return vchPubKey;
}

void CReserveKey::KeepKey()
{
    if (nIndex != -1)
        pwallet->KeepKey(nIndex);
    nIndex = -1;
    vchPubKey = CPubKey();
}

void CReserveKey::ReturnKey()
{
    if (nIndex != -1)
        pwallet->ReturnKey(nIndex);
    nIndex = -1;
    vchPubKey = CPubKey();
}

void CWallet::GetAllReserveKeys(set<CKeyID>& setAddress)
{
    setAddress.clear();

    CWalletDB walletdb(strWalletFile);

    LOCK2(cs_main, cs_wallet);
    BOOST_FOREACH(const int64& id, setKeyPool)
    {
        CKeyPool keypool;
        if (!walletdb.ReadPool(id, keypool))
            throw runtime_error("GetAllReserveKeyHashes() : read failed");
        assert(keypool.vchPubKey.IsValid());
        CKeyID keyID = keypool.vchPubKey.GetID();
        if (!HaveKey(keyID))
            throw runtime_error("GetAllReserveKeyHashes() : unknown key in key pool");
        setAddress.insert(keyID);
    }
}

// ppcoin: check 'spent' consistency between wallet and txindex
// ppcoin: fix wallet spent state according to txindex
void CWallet::FixSpentCoins(int& nMismatchFound, int64& nBalanceInQuestion, bool fCheckOnly)
{
    nMismatchFound = 0;
    nBalanceInQuestion = 0;

    LOCK(cs_wallet);
    vector<CWalletTx*> vCoins;
    vCoins.reserve(mapWallet.size());
    for (map<uint256, CWalletTx>::iterator it = mapWallet.begin(); it != mapWallet.end(); ++it)
        vCoins.push_back(&(*it).second);

    CTxDB txdb("r");
    BOOST_FOREACH(CWalletTx* pcoin, vCoins)
    {
        // Find the corresponding transaction index
        CTxIndex txindex;
        if (!txdb.ReadTxIndex(pcoin->GetHash(), txindex))
            continue;
        for (int n=0; n < pcoin->vout.size(); n++)
        {
            if (IsMine(pcoin->vout[n]) && pcoin->IsSpent(n) && (txindex.vSpent.size() <= n || txindex.vSpent[n].IsNull()))
            {
                printf("FixSpentCoins found lost coin %sslm %s[%d], %s\n",
                    FormatMoney(pcoin->vout[n].nValue).c_str(), pcoin->GetHash().ToString().c_str(), n, fCheckOnly? "repair not attempted" : "repairing");
                nMismatchFound++;
                nBalanceInQuestion += pcoin->vout[n].nValue;
                if (!fCheckOnly)
                {
                    pcoin->MarkUnspent(n);
                    pcoin->WriteToDisk();
                }
            }
            else if (IsMine(pcoin->vout[n]) && !pcoin->IsSpent(n) && (txindex.vSpent.size() > n && !txindex.vSpent[n].IsNull()))
            {
                printf("FixSpentCoins found spent coin %sslm %s[%d], %s\n",
                    FormatMoney(pcoin->vout[n].nValue).c_str(), pcoin->GetHash().ToString().c_str(), n, fCheckOnly? "repair not attempted" : "repairing");
                nMismatchFound++;
                nBalanceInQuestion += pcoin->vout[n].nValue;
                if (!fCheckOnly)
                {
                    pcoin->MarkSpent(n);
                    pcoin->WriteToDisk();
                }
            }
        }
    }
}

// ppcoin: disable transaction (only for coinstake)
void CWallet::DisableTransaction(const CTransaction &tx)
{
    if (!tx.IsCoinStake() || !IsFromMe(tx))
        return; // only disconnecting coinstake requires marking input unspent

    LOCK(cs_wallet);
    BOOST_FOREACH(const CTxIn& txin, tx.vin)
    {
        map<uint256, CWalletTx>::iterator mi = mapWallet.find(txin.prevout.hash);
        if (mi != mapWallet.end())
        {
            CWalletTx& prev = (*mi).second;
            if (txin.prevout.n < prev.vout.size() && IsMine(prev.vout[txin.prevout.n]))
            {
                prev.MarkUnspent(txin.prevout.n);
                prev.WriteToDisk();
            }
        }
    }
}

// wallet check/repair
// check 'spent' consistency between wallet and txindex
// fix wallet spent state according to txindex
// remove orphan Coinbase and Coinstake
void CWallet::Fix_SpentCoins(int& nMismatchFound, int64& nBalanceInQuestion, int& nOrphansFound, bool fCheckOnly)
{
    nMismatchFound = 0;
    nBalanceInQuestion = 0;
    nOrphansFound = 0;

    LOCK(cs_wallet);
    vector<CWalletTx*> vCoins;
    vCoins.reserve(mapWallet.size());
    for (map<uint256, CWalletTx>::iterator it = mapWallet.begin(); it != mapWallet.end(); ++it)
        vCoins.push_back(&(*it).second);

    CTxDB txdb("r");
    BOOST_FOREACH(CWalletTx* pcoin, vCoins)
    {
        uint256 hash = pcoin->GetHash();
        // Find the corresponding transaction index
        CTxIndex txindex;
        if (!txdb.ReadTxIndex(hash, txindex) && !(pcoin->IsCoinBase() || pcoin->IsCoinStake()))
            continue;

        for (unsigned int n=0; n < pcoin->vout.size(); n++)
        {
            bool fUpdated = false;
            if (IsMine(pcoin->vout[n]) && pcoin->IsSpent(n) && (txindex.vSpent.size() <= n || txindex.vSpent[n].IsNull()))
            {
                printf("FixSpentCoins found lost coin %scap %s[%d], %s\n",
                    FormatMoney(pcoin->vout[n].nValue).c_str(), hash.ToString().c_str(), n, fCheckOnly? "repair not attempted" : "repairing");
                nMismatchFound++;
                nBalanceInQuestion += pcoin->vout[n].nValue;
                if (!fCheckOnly)
                {
                    fUpdated = true;
                    pcoin->MarkUnspent(n);
                    pcoin->WriteToDisk();
                }
            }
            else if (IsMine(pcoin->vout[n]) && !pcoin->IsSpent(n) && (txindex.vSpent.size() > n && !txindex.vSpent[n].IsNull()))
            {
                printf("FixSpentCoins found spent coin %scap %s[%d], %s\n",
                    FormatMoney(pcoin->vout[n].nValue).c_str(), hash.ToString().c_str(), n, fCheckOnly? "repair not attempted" : "repairing");
                nMismatchFound++;
                nBalanceInQuestion += pcoin->vout[n].nValue;
                if (!fCheckOnly)
                {
                    fUpdated = true;
                    pcoin->MarkSpent(n);
                    pcoin->WriteToDisk();
                }
            }
            if (fUpdated)
                NotifyTransactionChanged(this, hash, CT_UPDATED);
        }

        if((pcoin->IsCoinBase() || pcoin->IsCoinStake()) && pcoin->GetDepthInMainChain() < 0)
        {
           nOrphansFound++;
           if (!fCheckOnly)
           {
             EraseFromWallet(hash);
             NotifyTransactionChanged(this, hash, CT_DELETED);
           }
           printf("FixSpentCoins %s orphaned generation tx %s\n", fCheckOnly ? "found" : "removed", hash.ToString().c_str());
        }
     }
}

void CWallet::ClearOrphans()
{
    list<uint256> orphans;

    LOCK(cs_wallet);
    for(map<uint256, CWalletTx>::const_iterator it = mapWallet.begin(); it != mapWallet.end(); ++it)
    {
        const CWalletTx *wtx = &(*it).second;
        if((wtx->IsCoinBase() || wtx->IsCoinStake()) && !wtx->IsInMainChain())
        {
          orphans.push_back(wtx->GetHash());
        }
    }

    for(list<uint256>::const_iterator it = orphans.begin(); it != orphans.end(); ++it)
    {
        EraseFromWallet(*it);
        UpdatedTransaction(*it);
    }
}
