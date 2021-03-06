/*
 *  Copyright (c) 2014, Airbitz
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms are permitted provided that
 *  the following conditions are met:
 *
 *  1. Redistributions of source code must retain the above copyright notice, this
 *  list of conditions and the following disclaimer.
 *  2. Redistributions in binary form must reproduce the above copyright notice,
 *  this list of conditions and the following disclaimer in the documentation
 *  and/or other materials provided with the distribution.
 *  3. Redistribution or use of modified source code requires the express written
 *  permission of Airbitz Inc.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 *  ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 *  WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 *  DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
 *  ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 *  (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 *  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 *  ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *  (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 *  SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 *  The views and conclusions contained in the software and documentation are those
 *  of the authors and should not be interpreted as representing official policies,
 *  either expressed or implied, of the Airbitz Project.
 */

#include "Wallet.hpp"
#include "Tx.hpp"
#include "account/Account.hpp"
#include "bitcoin/WatcherBridge.hpp"
#include "crypto/Crypto.hpp"
#include "crypto/Encoding.hpp"
#include "crypto/Random.hpp"
#include "login/Login.hpp"
#include "login/LoginServer.hpp"
#include "util/FileIO.hpp"
#include "util/Json.hpp"
#include "util/Mutex.hpp"
#include "util/Sync.hpp"
#include "util/Util.hpp"
#include "util/URL.hpp"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

namespace abcd {

#define WALLET_KEY_LENGTH                       AES_256_KEY_LENGTH

#define WALLET_BITCOIN_PRIVATE_SEED_LENGTH      32

#define WALLET_DIR                              "Wallets"
#define WALLET_SYNC_DIR                         "sync"
#define WALLET_TX_DIR                           "Transactions"
#define WALLET_ADDR_DIR                         "Addresses"
#define WALLET_ACCOUNTS_WALLETS_FILENAME        "Wallets.json"
#define WALLET_NAME_FILENAME                    "WalletName.json"
#define WALLET_CURRENCY_FILENAME                "Currency.json"
#define WALLET_ACCOUNTS_FILENAME                "Accounts.json"

#define JSON_WALLET_WALLETS_FIELD               "wallets"
#define JSON_WALLET_NAME_FIELD                  "walletName"
#define JSON_WALLET_CURRENCY_NUM_FIELD          "num"
#define JSON_WALLET_ACCOUNTS_FIELD              "accounts"

// holds wallet data (including keys) for a given wallet
typedef struct sWalletData
{
    char            *szUUID;
    char            *szName;
    char            *szWalletDir;
    char            *szWalletSyncDir;
    char            *szWalletAcctKey;
    int             currencyNum;
    unsigned int    numAccounts;
    char            **aszAccounts;
    tABC_U08Buf     MK;
    tABC_U08Buf     BitcoinPrivateSeed;
    unsigned        archived;
    bool            balanceDirty;
    int64_t         balance;
} tWalletData;

// this holds all the of the currently cached wallets
static unsigned int gWalletsCacheCount = 0;
static tWalletData **gaWalletsCacheArray = NULL;

static tABC_CC ABC_WalletSetCurrencyNum(tABC_WalletID self, int currencyNum, tABC_Error *pError);
static tABC_CC ABC_WalletAddAccount(tABC_WalletID self, const char *szAccount, tABC_Error *pError);
static tABC_CC ABC_WalletCreateRootDir(tABC_Error *pError);
static tABC_CC ABC_WalletGetRootDirName(char **pszRootDir, tABC_Error *pError);
static tABC_CC ABC_WalletGetSyncDirName(char **pszDir, const char *szWalletUUID, tABC_Error *pError);
static tABC_CC ABC_WalletCacheData(tABC_WalletID self, tWalletData **ppData, tABC_Error *pError);
static tABC_CC ABC_WalletAddToCache(tWalletData *pData, tABC_Error *pError);
static tABC_CC ABC_WalletGetFromCacheByUUID(const char *szUUID, tWalletData **ppData, tABC_Error *pError);
static void    ABC_WalletFreeData(tWalletData *pData);

/**
 * Initializes the members of a tABC_WalletID structure.
 */
tABC_WalletID ABC_WalletID(const Login &login,
                           const char *szUUID)
{
    tABC_WalletID out;
    out.login = &login;
    out.szUUID = szUUID;
    return out;
}

/**
 * Copies the strings from one tABC_WalletID struct to another.
 */
tABC_CC ABC_WalletIDCopy(tABC_WalletID *out,
                         tABC_WalletID in,
                         tABC_Error *pError)
{
    tABC_CC cc = ABC_CC_Ok;

    out->login = in.login;
    ABC_STRDUP(out->szUUID, in.szUUID);

exit:
    return cc;
}

/**
 * Frees the strings inside a tABC_WalletID struct.
 *
 * This is normally not necessary, except in cases where ABC_WalletIDCopy
 * has been used.
 */
void ABC_WalletIDFree(tABC_WalletID in)
{
    char *szUUID     = (char *)in.szUUID;
    ABC_FREE_STR(szUUID);
}

/**
 * Creates the wallet with the given info.
 *
 * @param pszUUID Pointer to hold allocated pointer to UUID string
 */
tABC_CC ABC_WalletCreate(const Login &login,
                         tABC_U08Buf L1,
                         tABC_U08Buf LP1,
                         const char *szUserName,
                         const char *szWalletName,
                         int  currencyNum,
                         char                  **pszUUID,
                         tABC_Error            *pError)
{
    tABC_CC cc = ABC_CC_Ok;

    char *szFilename       = NULL;
    char *szJSON           = NULL;
    char *szWalletDir      = NULL;
    json_t *pJSON_Data     = NULL;
    json_t *pJSON_Wallets  = NULL;
    std::string uuid;
    DataChunk dataKey;
    DataChunk syncKey;
    DataChunk bitcoinKey;

    tWalletData *pData = NULL;

    ABC_CHECK_NULL(pszUUID);

    // create a new wallet data struct
    ABC_NEW(pData, tWalletData);
    pData->archived = 0;

    // create wallet guid
    ABC_CHECK_NEW(randomUuid(uuid), pError);
    ABC_STRDUP(pData->szUUID, uuid.c_str());
    ABC_STRDUP(*pszUUID, uuid.c_str());

    // generate the master key for this wallet - MK_<Wallet_GUID1>
    ABC_CHECK_NEW(randomData(dataKey, WALLET_KEY_LENGTH), pError);
    ABC_BUF_DUP(pData->MK, toU08Buf(dataKey));

    // create and set the bitcoin private seed for this wallet
    ABC_CHECK_NEW(randomData(bitcoinKey, WALLET_BITCOIN_PRIVATE_SEED_LENGTH), pError);
    ABC_BUF_DUP(pData->BitcoinPrivateSeed, toU08Buf(bitcoinKey));

    // Create Wallet Repo key
    ABC_CHECK_NEW(randomData(syncKey, SYNC_KEY_LENGTH), pError);
    ABC_STRDUP(pData->szWalletAcctKey, base16Encode(syncKey).c_str());

    // create the wallet root directory if necessary
    ABC_CHECK_RET(ABC_WalletCreateRootDir(pError));

    // create the wallet directory - <Wallet_UUID1>  <- All data in this directory encrypted with MK_<Wallet_UUID1>
    ABC_CHECK_RET(ABC_WalletGetDirName(&(pData->szWalletDir), pData->szUUID, pError));
    ABC_CHECK_NEW(fileEnsureDir(pData->szWalletDir), pError);
    ABC_STRDUP(szWalletDir, pData->szWalletDir);

    // create the wallet sync dir under the main dir
    ABC_CHECK_RET(ABC_WalletGetSyncDirName(&(pData->szWalletSyncDir), pData->szUUID, pError));
    ABC_CHECK_NEW(fileEnsureDir(pData->szWalletSyncDir), pError);

    // we now have a new wallet so go ahead and cache its data
    ABC_CHECK_RET(ABC_WalletAddToCache(pData, pError));

    // all the functions below assume the wallet is in the cache or can be loaded into the cache
    // set the wallet name
    ABC_CHECK_RET(ABC_WalletSetName(ABC_WalletID(login, pData->szUUID), szWalletName, pError));

    // set the currency
    ABC_CHECK_RET(ABC_WalletSetCurrencyNum(ABC_WalletID(login, pData->szUUID), currencyNum, pError));

    // Request remote wallet repo
    ABC_CHECK_NEW(LoginServerWalletCreate(L1, LP1, pData->szWalletAcctKey), pError);

    // set this account for the wallet's first account
    ABC_CHECK_RET(ABC_WalletAddAccount(ABC_WalletID(login, pData->szUUID), szUserName, pError));

    // TODO: should probably add the creation date to optimize wallet export (assuming it is even used)

    // Init the git repo and sync it
    int dirty;
    ABC_CHECK_RET(ABC_SyncMakeRepo(pData->szWalletSyncDir, pError));
    ABC_CHECK_RET(ABC_SyncRepo(pData->szWalletSyncDir, pData->szWalletAcctKey, &dirty, pError));

    // Actiate the remote wallet
    ABC_CHECK_NEW(LoginServerWalletActivate(L1, LP1, pData->szWalletAcctKey), pError);

    // If everything worked, add the wallet to the account:
    tABC_AccountWalletInfo info; // No need to free this
    info.szUUID = pData->szUUID;
    info.MK = pData->MK;
    info.BitcoinSeed = pData->BitcoinPrivateSeed;
    info.SyncKey = toU08Buf(syncKey);
    info.archived = 0;
    ABC_CHECK_RET(ABC_AccountWalletList(login, NULL, &info.sortIndex, pError));
    ABC_CHECK_RET(ABC_AccountWalletSave(login, &info, pError));

    // Now the wallet is written to disk, generate some addresses
    ABC_CHECK_RET(ABC_TxCreateInitialAddresses(ABC_WalletID(login, pData->szUUID), pError));

    // After wallet is created, sync the account, ignoring any errors
    tABC_Error Error;
    ABC_CHECK_RET(ABC_SyncRepo(login.syncDir().c_str(), login.syncKey().c_str(), &dirty, &Error));

    pData = NULL; // so we don't free what we just added to the cache
exit:
    if (cc != ABC_CC_Ok)
    {
        if (!uuid.empty())
        {
            ABC_WalletRemoveFromCache(uuid.c_str(), NULL);
        }
        if (szWalletDir)
        {
            ABC_FileIODeleteRecursive(szWalletDir, NULL);
        }
    }
    ABC_FREE_STR(szWalletDir);
    ABC_FREE_STR(szFilename);
    ABC_FREE_STR(szJSON);
    if (pJSON_Data)         json_decref(pJSON_Data);
    if (pJSON_Wallets)      json_decref(pJSON_Wallets);
    if (pData)              ABC_WalletFreeData(pData);

    return cc;
}

/**
 * Sync the wallet's data
 */
tABC_CC ABC_WalletSyncData(tABC_WalletID self, int *pDirty, tABC_Error *pError)
{
    tABC_CC cc = ABC_CC_Ok;
    ABC_SET_ERR_CODE(pError, ABC_CC_Ok);

    tABC_GeneralInfo *pInfo = NULL;
    char *szDirectory       = NULL;
    char *szSyncDirectory   = NULL;
    tWalletData *pData      = NULL;
    bool bExists            = false;
    bool bNew               = false;

    // Fetch general info
    ABC_CHECK_RET(ABC_GeneralGetInfo(&pInfo, pError));

    // create the wallet root directory if necessary
    ABC_CHECK_RET(ABC_WalletCreateRootDir(pError));

    // create the wallet directory - <Wallet_UUID1>  <- All data in this directory encrypted with MK_<Wallet_UUID1>
    ABC_CHECK_RET(ABC_WalletGetDirName(&szDirectory, self.szUUID, pError));
    ABC_CHECK_NEW(fileEnsureDir(szDirectory), pError);

    // create the wallet sync dir under the main dir
    ABC_CHECK_RET(ABC_WalletGetSyncDirName(&szSyncDirectory, self.szUUID, pError));
    ABC_CHECK_RET(ABC_FileIOFileExists(szSyncDirectory, &bExists, pError));
    if (!bExists)
    {
        ABC_CHECK_RET(ABC_SyncMakeRepo(szSyncDirectory, pError));
        bNew = true;
    }

    // load the wallet data into the cache
    ABC_CHECK_RET(ABC_WalletCacheData(self, &pData, pError));
    ABC_CHECK_ASSERT(NULL != pData->szWalletAcctKey, ABC_CC_Error, "Expected to find RepoAcctKey in key cache");

    // Sync
    ABC_CHECK_RET(ABC_SyncRepo(pData->szWalletSyncDir, pData->szWalletAcctKey, pDirty, pError));
    if (*pDirty || bNew)
    {
        *pDirty = 1;
        ABC_WalletClearCache();
    }
exit:
    ABC_FREE_STR(szSyncDirectory);
    ABC_FREE_STR(szDirectory);
    ABC_GeneralFreeInfo(pInfo);
    return cc;
}

/**
 * Sets the name of a wallet
 */
tABC_CC ABC_WalletSetName(tABC_WalletID self, const char *szName, tABC_Error *pError)
{
    tABC_CC cc = ABC_CC_Ok;

    tWalletData *pData = NULL;
    char *szFilename = NULL;
    char *szJSON = NULL;

    // load the wallet data into the cache
    ABC_CHECK_RET(ABC_WalletCacheData(self, &pData, pError));

    // set the new name
    ABC_FREE_STR(pData->szName);
    ABC_STRDUP(pData->szName, szName);

    // create the json for the wallet name
    ABC_CHECK_RET(ABC_UtilCreateValueJSONString(szName, JSON_WALLET_NAME_FIELD, &szJSON, pError));
    //printf("name:\n%s\n", szJSON);

    // write the name out to the file
    ABC_STR_NEW(szFilename, ABC_FILEIO_MAX_PATH_LENGTH);
    sprintf(szFilename, "%s/%s", pData->szWalletSyncDir, WALLET_NAME_FILENAME);
    tABC_U08Buf Data; // Do not free
    ABC_BUF_SET_PTR(Data, (unsigned char *)szJSON, strlen(szJSON) + 1);
    ABC_CHECK_RET(ABC_CryptoEncryptJSONFile(Data, pData->MK, ABC_CryptoType_AES256, szFilename, pError));

exit:
    ABC_FREE_STR(szFilename);
    ABC_FREE_STR(szJSON);

    return cc;
}

/**
 * Sets the currency number of a wallet
 */
static
tABC_CC ABC_WalletSetCurrencyNum(tABC_WalletID self, int currencyNum, tABC_Error *pError)
{
    tABC_CC cc = ABC_CC_Ok;

    tWalletData *pData = NULL;
    char *szFilename = NULL;
    char *szJSON = NULL;

    // load the wallet data into the cache
    ABC_CHECK_RET(ABC_WalletCacheData(self, &pData, pError));

    // set the currency number
    pData->currencyNum = currencyNum;

    // create the json for the currency number
    ABC_CHECK_RET(ABC_UtilCreateIntJSONString(currencyNum, JSON_WALLET_CURRENCY_NUM_FIELD, &szJSON, pError));
    //printf("currency num:\n%s\n", szJSON);

    // write the name out to the file
    ABC_STR_NEW(szFilename, ABC_FILEIO_MAX_PATH_LENGTH);
    sprintf(szFilename, "%s/%s", pData->szWalletSyncDir, WALLET_CURRENCY_FILENAME);
    tABC_U08Buf Data; // Do not free
    ABC_BUF_SET_PTR(Data, (unsigned char *)szJSON, strlen(szJSON) + 1);
    ABC_CHECK_RET(ABC_CryptoEncryptJSONFile(Data, pData->MK, ABC_CryptoType_AES256, szFilename, pError));

exit:
    ABC_FREE_STR(szFilename);
    ABC_FREE_STR(szJSON);

    return cc;
}

/**
 * Adds the given account to the list of accounts that uses this wallet
 */
static
tABC_CC ABC_WalletAddAccount(tABC_WalletID self, const char *szAccount, tABC_Error *pError)
{
    tABC_CC cc = ABC_CC_Ok;

    tWalletData *pData = NULL;
    char *szFilename = NULL;
    json_t *dataJSON = NULL;

    // load the wallet data into the cache
    ABC_CHECK_RET(ABC_WalletCacheData(self, &pData, pError));

    // if there are already accounts in the list
    if ((pData->aszAccounts) && (pData->numAccounts > 0))
    {
        ABC_ARRAY_RESIZE(pData->aszAccounts, pData->numAccounts + 1, char*);
    }
    else
    {
        pData->numAccounts = 0;
        ABC_ARRAY_NEW(pData->aszAccounts, 1, char*);
    }
    ABC_STRDUP(pData->aszAccounts[pData->numAccounts], szAccount);
    pData->numAccounts++;

    // create the json for the accounts
    ABC_CHECK_RET(ABC_UtilCreateArrayJSONObject(pData->aszAccounts, pData->numAccounts, JSON_WALLET_ACCOUNTS_FIELD, &dataJSON, pError));

    // write the name out to the file
    ABC_STR_NEW(szFilename, ABC_FILEIO_MAX_PATH_LENGTH);
    sprintf(szFilename, "%s/%s", pData->szWalletSyncDir, WALLET_ACCOUNTS_FILENAME);
    ABC_CHECK_RET(ABC_CryptoEncryptJSONFileObject(dataJSON, pData->MK, ABC_CryptoType_AES256, szFilename, pError));

exit:
    ABC_FREE_STR(szFilename);
    if (dataJSON)       json_decref(dataJSON);

    return cc;
}

/**
 * creates the wallet directory if needed
 */
static
tABC_CC ABC_WalletCreateRootDir(tABC_Error *pError)
{
    tABC_CC cc = ABC_CC_Ok;

    char *szWalletRoot = NULL;

    // create the account directory string
    ABC_CHECK_RET(ABC_WalletGetRootDirName(&szWalletRoot, pError));

    // if it doesn't exist
    ABC_CHECK_NEW(fileEnsureDir(szWalletRoot), pError);

exit:
    ABC_FREE_STR(szWalletRoot);

    return cc;
}

/**
 * Gets the root directory for the wallets
 * the string is allocated so it is up to the caller to free it
 */
static
tABC_CC ABC_WalletGetRootDirName(char **pszRootDir, tABC_Error *pError)
{
    tABC_CC cc = ABC_CC_Ok;

    std::string out = getRootDir() + WALLET_DIR;

    ABC_CHECK_NULL(pszRootDir);
    ABC_STRDUP(*pszRootDir, out.c_str());

exit:
    return cc;
}

/**
 * Gets the directory for the given wallet UUID
 * the string is allocated so it is up to the caller to free it
 */
tABC_CC ABC_WalletGetDirName(char **pszDir, const char *szWalletUUID, tABC_Error *pError)
{
    tABC_CC cc = ABC_CC_Ok;

    char *szWalletRootDir = NULL;

    ABC_CHECK_NULL(pszDir);

    ABC_CHECK_RET(ABC_WalletGetRootDirName(&szWalletRootDir, pError));

    // create the account directory string
    ABC_STR_NEW(*pszDir, ABC_FILEIO_MAX_PATH_LENGTH);
    ABC_CHECK_NULL(*pszDir);
    sprintf(*pszDir, "%s/%s", szWalletRootDir, szWalletUUID);

exit:
    ABC_FREE_STR(szWalletRootDir);

    return cc;
}

/**
 * Gets the sync directory for the given wallet UUID.
 *
 * @param pszDir the output directory name. The caller must free this.
 */
static
tABC_CC ABC_WalletGetSyncDirName(char **pszDir, const char *szWalletUUID, tABC_Error *pError)
{
    tABC_CC cc = ABC_CC_Ok;

    char *szWalletDir = NULL;

    ABC_CHECK_NULL(pszDir);
    ABC_CHECK_NULL(szWalletUUID);

    ABC_CHECK_RET(ABC_WalletGetDirName(&szWalletDir, szWalletUUID, pError));

    ABC_STR_NEW(*pszDir, ABC_FILEIO_MAX_PATH_LENGTH);
    ABC_CHECK_NULL(*pszDir);
    sprintf(*pszDir, "%s/%s", szWalletDir, WALLET_SYNC_DIR);

exit:
    ABC_FREE_STR(szWalletDir);

    return cc;
}

/**
 * Gets the transaction directory for the given wallet UUID.
 *
 * @param pszDir the output directory name. The caller must free this.
 */
tABC_CC ABC_WalletGetTxDirName(char **pszDir, const char *szWalletUUID, tABC_Error *pError)
{
    tABC_CC cc = ABC_CC_Ok;

    char *szWalletSyncDir = NULL;

    ABC_CHECK_NULL(pszDir);
    ABC_CHECK_NULL(szWalletUUID);

    ABC_CHECK_RET(ABC_WalletGetSyncDirName(&szWalletSyncDir, szWalletUUID, pError));

    ABC_STR_NEW(*pszDir, ABC_FILEIO_MAX_PATH_LENGTH);
    ABC_CHECK_NULL(*pszDir);
    sprintf(*pszDir, "%s/%s", szWalletSyncDir, WALLET_TX_DIR);

exit:
    ABC_FREE_STR(szWalletSyncDir);

    return cc;
}

/**
 * Gets the address directory for the given wallet UUID.
 *
 * @param pszDir the output directory name. The caller must free this.
 */
tABC_CC ABC_WalletGetAddressDirName(char **pszDir, const char *szWalletUUID, tABC_Error *pError)
{
    tABC_CC cc = ABC_CC_Ok;

    char *szWalletSyncDir = NULL;

    ABC_CHECK_NULL(pszDir);
    ABC_CHECK_NULL(szWalletUUID);

    ABC_CHECK_RET(ABC_WalletGetSyncDirName(&szWalletSyncDir, szWalletUUID, pError));

    ABC_STR_NEW(*pszDir, ABC_FILEIO_MAX_PATH_LENGTH);
    ABC_CHECK_NULL(*pszDir);
    sprintf(*pszDir, "%s/%s", szWalletSyncDir, WALLET_ADDR_DIR);

exit:
    ABC_FREE_STR(szWalletSyncDir);

    return cc;
}

/**
 * Adds the wallet data to the cache
 * If the wallet is not currently in the cache it is added
 */
static
tABC_CC ABC_WalletCacheData(tABC_WalletID self, tWalletData **ppData, tABC_Error *pError)
{
    tABC_CC cc = ABC_CC_Ok;
    AutoCoreLock lock(gCoreMutex);

    tWalletData *pData = NULL;
    char *szFilename = NULL;
    AutoAccountWalletInfo info;
    memset(&info, 0, sizeof(tABC_AccountWalletInfo));

    // see if it is already in the cache
    ABC_CHECK_RET(ABC_WalletGetFromCacheByUUID(self.szUUID, &pData, pError));

    // if it is already cached
    if (NULL != pData)
    {
        // hang on to this data
        *ppData = pData;

        // if we don't do this, we'll free it below but it isn't ours, it is from the cache
        pData = NULL;
    }
    else
    {
        // we need to add it

        // create a new wallet data struct
        ABC_NEW(pData, tWalletData);
        ABC_STRDUP(pData->szUUID, self.szUUID);
        ABC_CHECK_RET(ABC_WalletGetDirName(&(pData->szWalletDir), self.szUUID, pError));
        ABC_CHECK_RET(ABC_WalletGetSyncDirName(&(pData->szWalletSyncDir), self.szUUID, pError));

        // Get the wallet info from the account:
        ABC_CHECK_RET(ABC_AccountWalletLoad(*self.login, self.szUUID, &info, pError));
        pData->archived = info.archived;

        // Steal the wallet info into our struct:
        pData->MK = info.MK;
        info.MK.p = NULL;

        // Steal the bitcoin seed into our struct:
        pData->BitcoinPrivateSeed = info.BitcoinSeed;
        info.BitcoinSeed.p = NULL;

        // Encode the sync key into our struct:
        ABC_STRDUP(pData->szWalletAcctKey, base16Encode(info.SyncKey).c_str());

        // make sure this wallet exists, if it doesn't leave fields empty
        bool bExists = false;
        ABC_CHECK_RET(ABC_FileIOFileExists(pData->szWalletSyncDir, &bExists, pError));
        if (!bExists)
        {
            ABC_STRDUP(pData->szName, "");
            pData->currencyNum = -1;
            pData->numAccounts = 0;
            pData->aszAccounts = NULL;
        }
        else
        {
            // get the name
            ABC_STR_NEW(szFilename, ABC_FILEIO_MAX_PATH_LENGTH);
            sprintf(szFilename, "%s/%s", pData->szWalletSyncDir, WALLET_NAME_FILENAME);
            bExists = false;
            ABC_CHECK_RET(ABC_FileIOFileExists(szFilename, &bExists, pError));
            if (true == bExists)
            {
                AutoU08Buf Data;
                ABC_CHECK_RET(ABC_CryptoDecryptJSONFile(szFilename, pData->MK, &Data, pError));
                ABC_CHECK_RET(ABC_UtilGetStringValueFromJSONString((char *)ABC_BUF_PTR(Data), JSON_WALLET_NAME_FIELD, &(pData->szName), pError));
            }
            else
            {
                ABC_STRDUP(pData->szName, "");
            }

            // get the currency num
            sprintf(szFilename, "%s/%s", pData->szWalletSyncDir, WALLET_CURRENCY_FILENAME);
            bExists = false;
            ABC_CHECK_RET(ABC_FileIOFileExists(szFilename, &bExists, pError));
            if (true == bExists)
            {
                AutoU08Buf Data;
                ABC_CHECK_RET(ABC_CryptoDecryptJSONFile(szFilename, pData->MK, &Data, pError));
                ABC_CHECK_RET(ABC_UtilGetIntValueFromJSONString((char *)ABC_BUF_PTR(Data), JSON_WALLET_CURRENCY_NUM_FIELD, (int *) &(pData->currencyNum), pError));
            }
            else
            {
                pData->currencyNum = -1;
            }

            // get the accounts
            sprintf(szFilename, "%s/%s", pData->szWalletSyncDir, WALLET_ACCOUNTS_FILENAME);
            bExists = false;
            ABC_CHECK_RET(ABC_FileIOFileExists(szFilename, &bExists, pError));
            if (true == bExists)
            {
                AutoU08Buf Data;
                ABC_CHECK_RET(ABC_CryptoDecryptJSONFile(szFilename, pData->MK, &Data, pError));
                ABC_CHECK_RET(ABC_UtilGetArrayValuesFromJSONString((char *)ABC_BUF_PTR(Data), JSON_WALLET_ACCOUNTS_FIELD, &(pData->aszAccounts), &(pData->numAccounts), pError));
            }
            else
            {
                pData->numAccounts = 0;
                pData->aszAccounts = NULL;
            }

        }
        pData->balance = 0;
        pData->balanceDirty = true;

        // Add to cache
        ABC_CHECK_RET(ABC_WalletAddToCache(pData, pError));

        // hang on to this data
        *ppData = pData;

        // if we don't do this, we'll free it below but it isn't ours, it is from the cache
        pData = NULL;
    }

exit:
    if (pData)
    {
        ABC_WalletFreeData(pData);
        ABC_CLEAR_FREE(pData, sizeof(tWalletData));
    }
    ABC_FREE_STR(szFilename);

    return cc;
}

/**
 * Clears all the data from the cache
 */
void ABC_WalletClearCache()
{
    AutoCoreLock lock(gCoreMutex);

    if ((gWalletsCacheCount > 0) && (NULL != gaWalletsCacheArray))
    {
        for (unsigned i = 0; i < gWalletsCacheCount; i++)
        {
            tWalletData *pData = gaWalletsCacheArray[i];
            ABC_WalletFreeData(pData);
        }

        ABC_FREE(gaWalletsCacheArray);
        gWalletsCacheCount = 0;
    }
}

/**
 * Adds the given WalletDAta to the array of cached wallets
 */
static
tABC_CC ABC_WalletAddToCache(tWalletData *pData, tABC_Error *pError)
{
    tABC_CC cc = ABC_CC_Ok;
    AutoCoreLock lock(gCoreMutex);

    tWalletData *pExistingWalletData = NULL;

    ABC_CHECK_NULL(pData);

    // see if it exists first
    ABC_CHECK_RET(ABC_WalletGetFromCacheByUUID(pData->szUUID, &pExistingWalletData, pError));

    // if it doesn't currently exist in the array
    if (pExistingWalletData == NULL)
    {
        // if we don't have an array yet
        if ((gWalletsCacheCount == 0) || (NULL == gaWalletsCacheArray))
        {
            // create a new one
            gWalletsCacheCount = 0;
            ABC_ARRAY_NEW(gaWalletsCacheArray, 1, tWalletData*);
        }
        else
        {
            // extend the current one
            ABC_ARRAY_RESIZE(gaWalletsCacheArray, gWalletsCacheCount + 1, tWalletData*);
        }
        gaWalletsCacheArray[gWalletsCacheCount] = pData;
        gWalletsCacheCount++;
    }
    else
    {
        cc = ABC_CC_WalletAlreadyExists;
    }

exit:
    return cc;
}

/**
 * Remove from cache
 */
tABC_CC ABC_WalletRemoveFromCache(const char *szUUID, tABC_Error *pError)
{
    tABC_CC cc = ABC_CC_Ok;
    AutoCoreLock lock(gCoreMutex);
    bool bExists = false;
    unsigned i;

    ABC_CHECK_NULL(szUUID);

    for (i = 0; i < gWalletsCacheCount; ++i)
    {
        tWalletData *pWalletInfo = gaWalletsCacheArray[i];
        if (strcmp(pWalletInfo->szUUID, szUUID) == 0)
        {
            // Delete this element
            ABC_WalletFreeData(pWalletInfo);
            pWalletInfo = NULL;

            // put the last element in this elements place
            gaWalletsCacheArray[i] = gaWalletsCacheArray[gWalletsCacheCount - 1];
            gaWalletsCacheArray[gWalletsCacheCount - 1] = NULL;
            bExists = true;
            break;
        }
    }
    if (bExists)
    {
        // Reduce the count of cache
        gWalletsCacheCount--;
        // and resize
        ABC_ARRAY_RESIZE(gaWalletsCacheArray, gWalletsCacheCount, tWalletData*);
    }

exit:
    return cc;
}

/**
 * Searches for a wallet in the cached by UUID
 * if it is not found, the wallet data will be set to NULL
 */
static
tABC_CC ABC_WalletGetFromCacheByUUID(const char *szUUID, tWalletData **ppData, tABC_Error *pError)
{
    tABC_CC cc = ABC_CC_Ok;

    ABC_CHECK_NULL(szUUID);
    ABC_CHECK_NULL(ppData);

    // assume we didn't find it
    *ppData = NULL;

    if ((gWalletsCacheCount > 0) && (NULL != gaWalletsCacheArray))
    {
        for (unsigned i = 0; i < gWalletsCacheCount; i++)
        {
            tWalletData *pData = gaWalletsCacheArray[i];
            if (0 == strcmp(szUUID, pData->szUUID))
            {
                // found it
                *ppData = pData;
                break;
            }
        }
    }

exit:

    return cc;
}

/**
 * Free's the given wallet data elements
 */
static
void ABC_WalletFreeData(tWalletData *pData)
{
    if (pData)
    {
        ABC_FREE_STR(pData->szUUID);

        ABC_FREE_STR(pData->szName);

        ABC_FREE_STR(pData->szWalletDir);

        ABC_FREE_STR(pData->szWalletSyncDir);

        pData->archived = 0;
        pData->currencyNum = -1;

        ABC_UtilFreeStringArray(pData->aszAccounts, pData->numAccounts);
        pData->aszAccounts = NULL;

        ABC_BUF_FREE(pData->MK);

        ABC_BUF_FREE(pData->BitcoinPrivateSeed);
    }
}

tABC_CC ABC_WalletDirtyCache(tABC_WalletID self,
                             tABC_Error *pError)
{
    tABC_CC cc = ABC_CC_Ok;
    AutoCoreLock lock(gCoreMutex);

    tWalletData     *pData = NULL;

    ABC_CHECK_RET(ABC_WalletCacheData(self, &pData, pError));
    pData->balanceDirty = true;
exit:
    return cc;
}

/**
 * Gets information on the given wallet.
 *
 * This function allocates and fills in an wallet info structure with the information
 * associated with the given wallet UUID
 *
 * @param ppWalletInfo          Pointer to store the pointer of the allocated wallet info struct
 * @param pError                A pointer to the location to store the error if there is one
 */
tABC_CC ABC_WalletGetInfo(tABC_WalletID self,
                          tABC_WalletInfo **ppWalletInfo,
                          tABC_Error *pError)
{
    tABC_CC cc = ABC_CC_Ok;
    AutoCoreLock lock(gCoreMutex);

    tWalletData     *pData = NULL;
    tABC_WalletInfo *pInfo = NULL;
    tABC_TxInfo     **aTransactions = NULL;
    unsigned int    nTxCount = 0;

    // load the wallet data into the cache
    ABC_CHECK_RET(ABC_WalletCacheData(self, &pData, pError));

    // create the wallet info struct
    ABC_NEW(pInfo, tABC_WalletInfo);

    // copy data from what was cachqed
    ABC_STRDUP(pInfo->szUUID, self.szUUID);
    if (pData->szName != NULL)
    {
        ABC_STRDUP(pInfo->szName, pData->szName);
    }
    pInfo->currencyNum = pData->currencyNum;
    pInfo->archived  = pData->archived;

    if (pData->balanceDirty == true)
    {
        ABC_CHECK_RET(
            ABC_TxGetTransactions(self,
                                  ABC_GET_TX_ALL_TIMES, ABC_GET_TX_ALL_TIMES,
                                  &aTransactions, &nTxCount, pError));
        ABC_CHECK_RET(ABC_BridgeFilterTransactions(self.szUUID, aTransactions, &nTxCount, pError));
        pData->balance = 0;
        for (unsigned i = 0; i < nTxCount; i++)
        {
            tABC_TxInfo *pTxInfo = aTransactions[i];
            pData->balance += pTxInfo->pDetails->amountSatoshi;
        }
        pData->balanceDirty = false;
    }
    pInfo->balanceSatoshi = pData->balance;


    // assign it to the user's pointer
    *ppWalletInfo = pInfo;
    pInfo = NULL;

exit:
    ABC_CLEAR_FREE(pInfo, sizeof(tABC_WalletInfo));
    if (nTxCount > 0) ABC_FreeTransactions(aTransactions, nTxCount);

    return cc;
}

/**
 * Free the wallet info.
 *
 * This function frees the info struct returned from ABC_WalletGetInfo.
 *
 * @param pWalletInfo   Wallet info to be free'd
 */
void ABC_WalletFreeInfo(tABC_WalletInfo *pWalletInfo)
{
    if (pWalletInfo != NULL)
    {
        ABC_FREE_STR(pWalletInfo->szUUID);
        ABC_FREE_STR(pWalletInfo->szName);

        ABC_CLEAR_FREE(pWalletInfo, sizeof(sizeof(tABC_WalletInfo)));
    }
}

/**
 * Gets the master key for the specified wallet
 *
 * @param pMK Pointer to store the master key
 *            (note: this is not allocated and should not be free'ed by the caller)
 */
tABC_CC ABC_WalletGetMK(tABC_WalletID self, tABC_U08Buf *pMK, tABC_Error *pError)
{
    tABC_CC cc = ABC_CC_Ok;
    AutoCoreLock lock(gCoreMutex);

    tWalletData *pData = NULL;

    // load the wallet data into the cache
    ABC_CHECK_RET(ABC_WalletCacheData(self, &pData, pError));

    // assign the address
    ABC_BUF_SET(*pMK, pData->MK);

exit:
    return cc;
}

/**
 * Gets the bitcoin private seed for the specified wallet
 *
 * @param pSeed Pointer to store the bitcoin private seed
 *            (note: this is not allocated and should not be free'ed by the caller)
 */
tABC_CC ABC_WalletGetBitcoinPrivateSeed(tABC_WalletID self, tABC_U08Buf *pSeed, tABC_Error *pError)
{
    tABC_CC cc = ABC_CC_Ok;
    AutoCoreLock lock(gCoreMutex);

    tWalletData *pData = NULL;

    // load the wallet data into the cache
    ABC_CHECK_RET(ABC_WalletCacheData(self, &pData, pError));

    // assign the address
    ABC_BUF_SET(*pSeed, pData->BitcoinPrivateSeed);

exit:
    return cc;
}

tABC_CC ABC_WalletGetBitcoinPrivateSeedDisk(tABC_WalletID self, tABC_U08Buf *pSeed, tABC_Error *pError)
{
    tABC_CC cc = ABC_CC_Ok;
    AutoCoreLock lock(gCoreMutex);

    AutoAccountWalletInfo info;

    ABC_CHECK_RET(ABC_AccountWalletLoad(*self.login, self.szUUID, &info, pError));

    // assign the address
    ABC_BUF_DUP(*pSeed, info.BitcoinSeed);

exit:
    return cc;
}

} // namespace abcds
