// Copyright 2021 Optiver Asia Pacific Pty. Ltd.
//
// This file is part of Ready Trader Go.
//
//     Ready Trader Go is free software: you can redistribute it and/or
//     modify it under the terms of the GNU Affero General Public License
//     as published by the Free Software Foundation, either version 3 of
//     the License, or (at your option) any later version.
//
//     Ready Trader Go is distributed in the hope that it will be useful,
//     but WITHOUT ANY WARRANTY; without even the implied warranty of
//     MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//     GNU Affero General Public License for more details.
//
//     You should have received a copy of the GNU Affero General Public
//     License along with Ready Trader Go.  If not, see
//     <https://www.gnu.org/licenses/>.
#include <array>

#include <boost/asio/io_context.hpp>

#include <ready_trader_go/logging.h>

#include "autotrader.h"

using namespace ReadyTraderGo;

RTG_INLINE_GLOBAL_LOGGER_WITH_CHANNEL(LG_AT, "AUTO")

constexpr int LOT_SIZE = 10;
constexpr int POSITION_LIMIT = 100;
constexpr int TICK_SIZE_IN_CENTS = 100;
constexpr int MIN_BID_NEARST_TICK = (MINIMUM_BID + TICK_SIZE_IN_CENTS) / TICK_SIZE_IN_CENTS * TICK_SIZE_IN_CENTS;
constexpr int MAX_ASK_NEAREST_TICK = MAXIMUM_ASK / TICK_SIZE_IN_CENTS * TICK_SIZE_IN_CENTS;

AutoTrader::AutoTrader(boost::asio::io_context& context) : BaseAutoTrader(context)
{
}

void AutoTrader::DisconnectHandler()
{
    BaseAutoTrader::DisconnectHandler();
    RLOG(LG_AT, LogLevel::LL_INFO) << "execution connection lost";
}

void AutoTrader::ErrorMessageHandler(unsigned long clientOrderId,
                                     const std::string& errorMessage)
{
    RLOG(LG_AT, LogLevel::LL_INFO) << "error with order " << clientOrderId << ": " << errorMessage;
    if (clientOrderId != 0 && ((mAsks.count(clientOrderId) == 1) || (mBids.count(clientOrderId) == 1)))
    {
        OrderStatusMessageHandler(clientOrderId, 0, 0, 0);
    }
}

void AutoTrader::HedgeFilledMessageHandler(unsigned long clientOrderId,
                                           unsigned long price,
                                           unsigned long volume)
{
    RLOG(LG_AT, LogLevel::LL_INFO) << "hedge order " << clientOrderId << " filled for " << volume
                                   << " lots at $" << price << " average price in cents";
    if (mHBids.count(clientOrderId) == 1)
    {
        mHPosition -= (long)volume;
    }
    else if (mHAsks.count(clientOrderId) == 1)
    {
        mHPosition += (long)volume;

    }
}

void AutoTrader::OrderBookMessageHandler(Instrument instrument,
                                         unsigned long sequenceNumber,
                                         const std::array<unsigned long, TOP_LEVEL_COUNT>& askPrices,
                                         const std::array<unsigned long, TOP_LEVEL_COUNT>& askVolumes,
                                         const std::array<unsigned long, TOP_LEVEL_COUNT>& bidPrices,
                                         const std::array<unsigned long, TOP_LEVEL_COUNT>& bidVolumes)
{
    RLOG(LG_AT, LogLevel::LL_INFO) << "order book received for " << instrument << " instrument"
                                   << ": ask prices: " << askPrices[0]
                                   << "; ask volumes: " << askVolumes[0]
                                   << "; bid prices: " << bidPrices[0]
                                   << "; bid volumes: " << bidVolumes[0];

    // Store best bid and best ask of ETF LOB
    if (instrument == Instrument::ETF)
    {
        ETFbestbid = bidPrices[0];
        ETFbestask = askPrices[0];
        ETFbidvol = bidVolumes[0];
        ETFaskvol = askVolumes[0];

    }

    if(instrument == Instrument::FUTURE)
    {

        RLOG(LG_AT, LogLevel::LL_INFO) << "Last ETF LOB Update "
                                       << ": ask prices: " << ETFbestask
                                       << "; ask volumes: " << ETFaskvol
                                       << "; bid prices: " << ETFbestbid
                                       << "; bid volumes: " << ETFbidvol;

        // base algo : cancel new orders until old orders are filled/cancelled
        if (mAskId != 0 && ETFbestbid != 0 && ETFbestbid != mBidPrice)
        {
            SendCancelOrder(mAskId);
            mAskId = 0;
            mHBidId = 0;
        }
        if (mBidId != 0 && ETFbestask != 0 && ETFbestask != mAskPrice)
        {
            SendCancelOrder(mBidId);
            mBidId = 0;
            mHAskId = 0;
        }

        // Pairs Trading : Sell ETF and buy Future (hedge) simultaneously
        if (mBidId == 0 && mHAskId == 0 && mHPosition > -POSITION_LIMIT && mPosition < POSITION_LIMIT && ETFbestbid > askPrices[0])
        {
            mBidId = mNextMessageId++;
            mHAskId = mHNextMessageId++;
            mBidPrice = ETFbestbid;
            mHAskPrice = askPrices[0];
            int vol = std::min(ETFbidvol, askVolumes[0]);
            SendInsertOrder(mBidId, Side::SELL, mBidPrice, vol, Lifespan::GOOD_FOR_DAY);
            //   SendHedgeOrder(mHAskId, Side::BUY, mHAskPrice, vol);
            mBids.emplace(mBidId);
            //  mHAsks.emplace(mHAskId);
            RLOG(LG_AT, LogLevel::LL_INFO) << " ETF Sell Order sent @ " << mBidPrice << " for " << ETFbidvol << "shares <> Future Buy Order sent @ "
                                           << mHAskPrice << " for " <<  askVolumes[0] << "shares" ;
        }
        // Pairs Trading : Buy ETF and sell Future (hedge) simultaneously
        if (mAskId == 0 && mHBidId == 0 && bidPrices[0] > ETFbestask && mPosition < POSITION_LIMIT && mHPosition > -POSITION_LIMIT)
        {
            mAskId = mNextMessageId++;
            mHBidId = mHNextMessageId++;
            mAskPrice = ETFbestask;
            mHBidPrice = bidPrices[0];
            int vol = std::min(ETFaskvol, bidVolumes[0]);
            SendInsertOrder(mAskId, Side::BUY, mAskPrice, vol, Lifespan::GOOD_FOR_DAY);
            //  SendHedgeOrder(mHBidId, Side::SELL, mHBidPrice, vol);
            mAsks.emplace(mAskId);
            //mHBids.emplace(mHBidId);
            RLOG(LG_AT, LogLevel::LL_INFO) << " ETF Buy Order sent @ " << mAskPrice << " for " << ETFaskvol << "shares <> Future Sell Order sent @ "
                                           << mHBidPrice << " for " <<  bidVolumes[0] << "shares" ;
        }
    }



}

void AutoTrader::OrderFilledMessageHandler(unsigned long clientOrderId,
                                           unsigned long price,
                                           unsigned long volume)
{
    RLOG(LG_AT, LogLevel::LL_INFO) << "order " << clientOrderId << " filled for " << volume
                                   << " lots at $" << price << " cents";
    if (mBids.count(clientOrderId) == 1)
    {
        mPosition -= (long)volume;
        SendHedgeOrder(mHAskId, Side::BUY, mHAskPrice, volume);
        mHAsks.emplace(mHAskId);
    }
    else if (mAsks.count(clientOrderId) == 1)
    {
        mPosition += (long)volume;
        SendHedgeOrder(mHBidId, Side::SELL, mHBidPrice, volume);
        mHBids.emplace(mHBidId);
    }
}

void AutoTrader::OrderStatusMessageHandler(unsigned long clientOrderId,
                                           unsigned long fillVolume,
                                           unsigned long remainingVolume,
                                           signed long fees)
{
    RLOG(LG_AT, LogLevel::LL_INFO) << " order status :  " << clientOrderId << " filled for " << fillVolume
                                   << " , for fee : " << fees << " and remaining volume : " << remainingVolume;

    if (remainingVolume == 0)
    {
        if (clientOrderId == mAskId)
        {
            mAskId = 0;

        }
        else if (clientOrderId == mBidId)
        {
            mBidId = 0;

        }

        else if (clientOrderId == mHAskId)
        {
            mHAskId = 0;

        }
        else if (clientOrderId == mHBidId)
        {
            mHBidId = 0;

        }
        else {}


        mAsks.erase(clientOrderId);
        mBids.erase(clientOrderId);
        mHAsks.erase(clientOrderId);
        mHBids.erase(clientOrderId);
    }

}

void AutoTrader::TradeTicksMessageHandler(Instrument instrument,
                                          unsigned long sequenceNumber,
                                          const std::array<unsigned long, TOP_LEVEL_COUNT>& askPrices,
                                          const std::array<unsigned long, TOP_LEVEL_COUNT>& askVolumes,
                                          const std::array<unsigned long, TOP_LEVEL_COUNT>& bidPrices,
                                          const std::array<unsigned long, TOP_LEVEL_COUNT>& bidVolumes)
{
    RLOG(LG_AT, LogLevel::LL_INFO) << "trade ticks received for " << instrument << " instrument"
                                   << ": ask prices: " << askPrices[0]
                                   << "; ask volumes: " << askVolumes[0]
                                   << "; bid prices: " << bidPrices[0]
                                   << "; bid volumes: " << bidVolumes[0];

}

