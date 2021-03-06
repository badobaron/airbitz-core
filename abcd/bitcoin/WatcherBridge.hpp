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
/**
 * @file
 * Caching and utility wrapper layer around the bitcoin `watcher` class.
 *
 * There was a time when `watcher` was part of libbitcoin-watcher,
 * and the AirBitz software was plain C.
 * This module used to bridge the gap between those two worlds,
 * but now it is less useful.
 */

#ifndef ABC_Bridge_h
#define ABC_Bridge_h

#include "../Tx.hpp"
#include "../util/Data.hpp"
#include "../util/Status.hpp"

namespace abcd {

tABC_CC ABC_BridgeSweepKey(tABC_WalletID self,
                           tABC_U08Buf key,
                           bool compressed,
                           tABC_Sweep_Done_Callback fCallback,
                           void *pData,
                           tABC_Error *pError);

tABC_CC ABC_BridgeWatcherStart(tABC_WalletID self,
                               tABC_Error *pError);

tABC_CC ABC_BridgeWatcherLoop(const char *szWalletUUID,
                              tABC_BitCoin_Event_Callback fAsyncCallback,
                              void *pData,
                              tABC_Error *pError);

tABC_CC ABC_BridgeWatcherConnect(const char *szWalletUUID, tABC_Error *pError);

tABC_CC ABC_BridgeWatcherDisconnect(const char *szWalletUUID, tABC_Error *pError);

tABC_CC ABC_BridgeWatcherStop(const char *szWalletUUID, tABC_Error *pError);

tABC_CC ABC_BridgeWatcherDelete(const char *szWalletUUID, tABC_Error *pError);

tABC_CC ABC_BridgeWatchAddr(const char *szWalletUUID, const char *address,
                            tABC_Error *pError);

tABC_CC ABC_BridgeWatchPath(const char *szWalletUUID, char **szPath,
                            tABC_Error *pError);

tABC_CC ABC_BridgePrioritizeAddress(const char *szWalletUUID,
                                    const char *szAddress,
                                    tABC_Error *pError);

tABC_CC ABC_BridgeTxMake(tABC_TxSendInfo *pSendInfo,
                         char **addresses, int addressCount,
                         char *changeAddress,
                         tABC_UnsignedTx *pUtx,
                         tABC_Error *pError);

tABC_CC ABC_BridgeTxSignSend(tABC_TxSendInfo *pSendInfo,
                             char **paPrivKey,
                             unsigned int keyCount,
                             tABC_UnsignedTx *pUtx,
                             tABC_Error *pError);

tABC_CC ABC_BridgeMaxSpendable(tABC_WalletID self,
                               const char *szDestAddress,
                               bool bTransfer,
                               uint64_t *pMaxSatoshi,
                               tABC_Error *pError);

tABC_CC ABC_BridgeTxHeight(const char *szWalletUUID, const char *szTxId, unsigned int *height, tABC_Error *pError);

tABC_CC ABC_BridgeTxBlockHeight(const char *szWalletUUID, unsigned int *height, tABC_Error *pError);

tABC_CC ABC_BridgeTxDetails(const char *szWalletUUID, const char *szTxID,
                            tABC_TxOutput ***paOutputs, unsigned int *pCount,
                            int64_t *pAmount, int64_t *pFees, tABC_Error *pError);

tABC_CC ABC_BridgeTxDetailsSplit(const char *szWalletUUID, const char *szTxID,
                                 tABC_TxOutput ***iarr, unsigned int *pInCount,
                                 tABC_TxOutput ***oarr, unsigned int *pOutCount,
                                 int64_t *pAmount, int64_t *pFees,
                                 tABC_Error *pError);

tABC_CC ABC_BridgeFilterTransactions(const char *szWalletUUID,
                                     tABC_TxInfo **aTransactions,
                                     unsigned int *pCount,
                                     tABC_Error *pError);

/**
 * Pulls a raw transaction out of the watcher database.
 */
Status
watcherBridgeRawTx(const char *szWalletUUID, const char *szTxID,
    DataChunk &result);

} // namespace abcd

#endif
