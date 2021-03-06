#ifndef K_PG_H_
#define K_PG_H_

namespace K {
  class PG: public Klass,
            public Wallet { public: PG() { wallet = this; };
    private:
      mWallets balance;
      mProfits profits;
      map<mPrice, mTrade> buys,
                          sells;
      string sideAPRDiff = "!=";
    protected:
      void load() {
        sqlite->backup(
          INTO profits
          THEN "loaded % historical Profits"
        );
        sqlite->backup(
          INTO target
          THEN "loaded TBP = % " + gw->base
        );
      };
      void waitData() {
        gw->WRITEME(mWallets, read);
      };
      void waitWebAdmin() {
        client->welcome(position);
        client->welcome(safety);
        client->welcome(target);
      };
    public:
      void calcSafety() {
        if (position.empty() or !market->stats.fairValue.fv) return;
        safety.send_ratelimit(nextSafety());
      };
      void calcTargetBasePos() {                                    PRETTY_DEBUG
        if (position.empty()) return screen->logWar("PG", "Unable to calculate TBP, missing wallet data");
        mAmount baseValue = position.baseValue;
        mAmount next = qp.autoPositionMode == mAutoPositionMode::Manual
          ? (qp.percentageValues
            ? qp.targetBasePositionPercentage * baseValue / 1e+2
            : qp.targetBasePosition)
          : ((1 + market->targetPosition) / 2) * baseValue;
        if (target.targetBasePosition and abs(target.targetBasePosition - next) < 1e-4 and sideAPRDiff == target.sideAPR) return;
        target.targetBasePosition = next;
        sideAPRDiff = target.sideAPR;
        calcPDiv(baseValue);
        target.send_push();
        if (!args.debugWallet) return;
        screen->log("PG", "TBP: "
          + to_string((int)(target.targetBasePosition / baseValue * 1e+2)) + "% = " + FN::str8(target.targetBasePosition)
          + " " + gw->base + ", pDiv: "
          + to_string((int)(target.positionDivergence  / baseValue * 1e+2)) + "% = " + FN::str8(target.positionDivergence)
          + " " + gw->base);
      };
      void calcWallet() {
        if (balance.empty() or !market->stats.fairValue.fv) return;
        if (args.maxWallet) applyMaxWallet();
        mPosition pos(
          FN::d8(balance.base.amount),
          FN::d8(balance.quote.amount),
          balance.quote.amount / market->stats.fairValue.fv,
          FN::d8(balance.base.held),
          FN::d8(balance.quote.held),
          balance.base.amount + balance.base.held,
          (balance.quote.amount + balance.quote.held) / market->stats.fairValue.fv,
          FN::d8((balance.quote.amount + balance.quote.held) / market->stats.fairValue.fv + balance.base.amount + balance.base.held),
          FN::d8((balance.base.amount + balance.base.held) * market->stats.fairValue.fv + balance.quote.amount + balance.quote.held),
          position.profitBase,
          position.profitQuote,
          mPair(gw->base, gw->quote)
        );
        calcPositionProfit(&pos);
        position.send_ratelimit(pos);
        screen->log(pos);
        calcTargetBasePos();
      };
      void calcWalletAfterOrder(const mSide &side) {
        if (position.empty()) return;
        mAmount heldAmount = 0;
        mAmount amount = side == mSide::Ask
          ? position.baseAmount + position.baseHeldAmount
          : position.quoteAmount + position.quoteHeldAmount;
        for (map<mRandId, mOrder>::value_type &it : broker->orders.orders)
          if (it.second.side == side and it.second.orderStatus == mStatus::Working) {
            mAmount held = it.second.quantity;
            if (it.second.side == mSide::Bid)
              held *= it.second.price;
            if (amount >= held) {
              amount -= held;
              heldAmount += held;
            }
          }
        (side == mSide::Ask
          ? balance.base
          : balance.quote
        ).reset(amount, heldAmount);
        calcWallet();
      };
      void calcSafetyAfterTrade(const mTrade &k) {
        (k.side == mSide::Bid
          ? buys : sells
        )[k.price] = k;
        calcSafety();
      };
    private:
      void read(const mWallets &rawdata) {                          PRETTY_DEBUG
        if (!rawdata.empty()) balance = rawdata;
        calcWallet();
      };
      mSafety nextSafety() {
        mAmount buySize = qp.percentageValues
          ? qp.buySizePercentage * position.baseValue / 100
          : qp.buySize;
        mAmount sellSize = qp.percentageValues
          ? qp.sellSizePercentage * position.baseValue / 100
          : qp.sellSize;
        map<mPrice, mTrade> tradesBuy;
        map<mPrice, mTrade> tradesSell;
        for (mTrade &it: broker->tradesHistory) {
          (it.side == mSide::Bid ? tradesBuy : tradesSell)[it.price] = it;
          if (qp.safety == mQuotingSafety::PingPong)
            (it.side == mSide::Ask ? buySize : sellSize) = it.quantity;
        }
        mAmount totalBasePosition = position.baseAmount + position.baseHeldAmount;
        if (qp.aggressivePositionRebalancing != mAPR::Off) {
          if (qp.buySizeMax) buySize = fmax(buySize, target.targetBasePosition - totalBasePosition);
          if (qp.sellSizeMax) sellSize = fmax(sellSize, totalBasePosition - target.targetBasePosition);
        }
        mPrice widthPong = qp.widthPercentage
          ? qp.widthPongPercentage * market->stats.fairValue.fv / 100
          : qp.widthPong;
        mPrice buyPing = 0,
               sellPing = 0;
        mAmount buyQty = 0,
                sellQty = 0;
        if (qp.pongAt == mPongAt::ShortPingFair or qp.pongAt == mPongAt::ShortPingAggressive) {
          matchBestPing(&tradesBuy, &buyPing, &buyQty, sellSize, widthPong, true);
          matchBestPing(&tradesSell, &sellPing, &sellQty, buySize, widthPong);
          if (!buyQty) matchFirstPing(&tradesBuy, &buyPing, &buyQty, sellSize, widthPong*-1, true);
          if (!sellQty) matchFirstPing(&tradesSell, &sellPing, &sellQty, buySize, widthPong*-1);
        } else if (qp.pongAt == mPongAt::LongPingFair or qp.pongAt == mPongAt::LongPingAggressive) {
          matchLastPing(&tradesBuy, &buyPing, &buyQty, sellSize, widthPong);
          matchLastPing(&tradesSell, &sellPing, &sellQty, buySize, widthPong, true);
        } else if (qp.pongAt == mPongAt::AveragePingFair or qp.pongAt == mPongAt::AveragePingAggressive) {
          matchAllPing(&tradesBuy, &buyPing, &buyQty, sellSize, widthPong);
          matchAllPing(&tradesSell, &sellPing, &sellQty, buySize, widthPong);
        }
        if (buyQty) buyPing /= buyQty;
        if (sellQty) sellPing /= sellQty;
        clean();
        mAmount sumBuys = sum(&buys);
        mAmount sumSells = sum(&sells);
        return mSafety(
          sumBuys / buySize,
          sumSells / sellSize,
          (sumBuys + sumSells) / (buySize + sellSize),
          buyPing,
          sellPing,
          buySize,
          sellSize
        );
      };
      void matchFirstPing(map<mPrice, mTrade> *trades, mPrice *ping, mAmount *qty, mAmount qtyMax, mPrice width, bool reverse = false) {
        matchPing(true, true, trades, ping, qty, qtyMax, width, reverse);
      };
      void matchBestPing(map<mPrice, mTrade> *trades, mPrice *ping, mAmount *qty, mAmount qtyMax, mPrice width, bool reverse = false) {
        matchPing(true, false, trades, ping, qty, qtyMax, width, reverse);
      };
      void matchLastPing(map<mPrice, mTrade> *trades, mPrice *ping, mAmount *qty, mAmount qtyMax, mPrice width, bool reverse = false) {
        matchPing(false, true, trades, ping, qty, qtyMax, width, reverse);
      };
      void matchAllPing(map<mPrice, mTrade> *trades, mPrice *ping, mAmount *qty, mAmount qtyMax, mPrice width) {
        matchPing(false, false, trades, ping, qty, qtyMax, width);
      };
      void matchPing(bool _near, bool _far, map<mPrice, mTrade> *trades, mPrice *ping, mAmount *qty, mAmount qtyMax, mPrice width, bool reverse = false) {
        int dir = width > 0 ? 1 : -1;
        if (reverse) for (map<mPrice, mTrade>::reverse_iterator it = trades->rbegin(); it != trades->rend(); ++it) {
          if (matchPing(_near, _far, ping, qty, qtyMax, width, dir * market->stats.fairValue.fv, dir * it->second.price, it->second.quantity, it->second.price, it->second.Kqty, reverse))
            break;
        } else for (map<mPrice, mTrade>::iterator it = trades->begin(); it != trades->end(); ++it)
          if (matchPing(_near, _far, ping, qty, qtyMax, width, dir * market->stats.fairValue.fv, dir * it->second.price, it->second.quantity, it->second.price, it->second.Kqty, reverse))
            break;
      };
      bool matchPing(bool _near, bool _far, mPrice *ping, mAmount *qty, mAmount qtyMax, mPrice width, mPrice fv, mPrice price, mAmount qtyTrade, mPrice priceTrade, mAmount KqtyTrade, bool reverse) {
        if (reverse) { fv *= -1; price *= -1; width *= -1; }
        if (((!_near and !_far) or *qty < qtyMax)
          and (_far ? fv > price : true)
          and (_near ? (reverse ? fv - width : fv + width) < price : true)
          and (!qp._matchPings or KqtyTrade < qtyTrade)
        ) {
          mAmount qty_ = qtyTrade;
          if (_near or _far)
            qty_ = fmin(qtyMax - *qty, qty_);
          *ping += priceTrade * qty_;
          *qty += qty_;
        }
        return *qty >= qtyMax and (_near or _far);
      };
      void clean() {
        if (buys.size()) expire(&buys);
        if (sells.size()) expire(&sells);
        skip();
      };
      void expire(map<mPrice, mTrade> *k) {
        mClock now = Tstamp;
        for (map<mPrice, mTrade>::iterator it = k->begin(); it != k->end();)
          if (it->second.time + qp.tradeRateSeconds * 1e+3 > now) ++it;
          else it = k->erase(it);
      };
      void skip() {
        while (buys.size() and sells.size()) {
          mTrade buy = buys.rbegin()->second;
          mTrade sell = sells.begin()->second;
          if (sell.price < buy.price) break;
          mAmount buyQty = buy.quantity;
          buy.quantity = buyQty - sell.quantity;
          sell.quantity = sell.quantity - buyQty;
          if (buy.quantity < gw->minSize)
            buys.erase(--buys.rbegin().base());
          if (sell.quantity < gw->minSize)
            sells.erase(sells.begin());
        }
      };
      mAmount sum(map<mPrice, mTrade> *k) {
        mAmount sum = 0;
        for (map<mPrice, mTrade>::value_type &it : *k)
          sum += it.second.quantity;
        return sum;
      };
      void calcPDiv(mAmount baseValue) {
        mAmount pDiv = qp.percentageValues
          ? qp.positionDivergencePercentage * baseValue / 1e+2
          : qp.positionDivergence;
        if (qp.autoPositionMode == mAutoPositionMode::Manual or mPDivMode::Manual == qp.positionDivergenceMode)
          target.positionDivergence = pDiv;
        else {
          mAmount pDivMin = qp.percentageValues
            ? qp.positionDivergencePercentageMin * baseValue / 1e+2
            : qp.positionDivergenceMin;
          double divCenter = 1 - abs((target.targetBasePosition / baseValue * 2) - 1);
          if (mPDivMode::Linear == qp.positionDivergenceMode) target.positionDivergence = pDivMin + (divCenter * (pDiv - pDivMin));
          else if (mPDivMode::Sine == qp.positionDivergenceMode) target.positionDivergence = pDivMin + (sin(divCenter*M_PI_2) * (pDiv - pDivMin));
          else if (mPDivMode::SQRT == qp.positionDivergenceMode) target.positionDivergence = pDivMin + (sqrt(divCenter) * (pDiv - pDivMin));
          else if (mPDivMode::Switch == qp.positionDivergenceMode) target.positionDivergence = divCenter < 1e-1 ? pDivMin : pDiv;
        }
      };
      void calcPositionProfit(mPosition *k) {
        if (profits.ratelimit()) return;
        profits.push_back(mProfit(k->baseValue, k->quoteValue));
        k->profitBase = profits.calcBase();
        k->profitQuote = profits.calcQuote();
      };
      void applyMaxWallet() {
        mAmount maxWallet = args.maxWallet;
        maxWallet -= balance.quote.held / market->stats.fairValue.fv;
        if (maxWallet > 0 and balance.quote.amount / market->stats.fairValue.fv > maxWallet) {
          balance.quote.amount = maxWallet * market->stats.fairValue.fv;
          maxWallet = 0;
        } else maxWallet -= balance.quote.amount / market->stats.fairValue.fv;
        maxWallet -= balance.base.held;
        if (maxWallet > 0 and balance.base.amount > maxWallet)
          balance.base.amount = maxWallet;
      };
  };
}

#endif
