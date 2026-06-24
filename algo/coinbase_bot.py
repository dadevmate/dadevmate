#!/usr/bin/env python3
"""
Coinbase Sandbox Algorithmic Day Trader
API     : Coinbase Exchange (Pro) sandbox
Strategy: EMA(9/21) crossover filtered by RSI(14)
Timeframe: 5-minute bars
Risk    : Hard stop-loss, take-profit, daily loss limit, circuit breaker
"""

import os, sys, time, hmac, hashlib, base64, json, logging
import requests
from datetime import datetime, timezone

# ── Configuration ───────────────────────────────────────────────────────────
BASE_URL       = "https://api-public.sandbox.exchange.coinbase.com"
API_KEY        = os.environ.get("CB_SANDBOX_KEY", "")
API_SECRET     = os.environ.get("CB_SANDBOX_SECRET", "")
API_PASSPHRASE = os.environ.get("CB_SANDBOX_PASSPHRASE", "")

PRODUCT        = "BTC-USD"
FAST_EMA_LEN   = 9
SLOW_EMA_LEN   = 21
RSI_LEN        = 14
GRANULARITY    = 300      # 5-min candles
LOOP_INTERVAL  = 60       # evaluate every 60 s

CAPITAL_FRAC   = 0.80     # deploy 80% of available USD per trade
TAKE_PROFIT    = 0.015    # exit at +1.5%
STOP_LOSS      = 0.0075   # exit at -0.75%  (2:1 reward/risk)
DAILY_LOSS_CAP = 5.00     # halt if equity drops $5 from session open
MIN_ORDER_USD  = 10.00    # don't trade if deployable cash < $10

# ── Logging ─────────────────────────────────────────────────────────────────
logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s  %(levelname)-7s  %(message)s",
    datefmt="%H:%M:%S",
    handlers=[
        logging.StreamHandler(sys.stdout),
        logging.FileHandler("trade_log.txt"),
    ],
)
log = logging.getLogger("cb_bot")

# ── Auth ────────────────────────────────────────────────────────────────────
def _headers(method: str, path: str, body: str = "") -> dict:
    ts  = str(time.time())
    msg = (ts + method.upper() + path + body).encode("ascii")
    key = base64.b64decode(API_SECRET)
    sig = base64.b64encode(hmac.new(key, msg, hashlib.sha256).digest()).decode()
    return {
        "CB-ACCESS-KEY":        API_KEY,
        "CB-ACCESS-SIGN":       sig,
        "CB-ACCESS-TIMESTAMP":  ts,
        "CB-ACCESS-PASSPHRASE": API_PASSPHRASE,
        "Content-Type":         "application/json",
    }

def _get(path: str) -> any:
    r = requests.get(BASE_URL + path, headers=_headers("GET", path), timeout=10)
    r.raise_for_status()
    return r.json()

def _post(path: str, body: dict) -> dict:
    b = json.dumps(body)
    r = requests.post(BASE_URL + path, headers=_headers("POST", path, b), data=b, timeout=10)
    r.raise_for_status()
    return r.json()

def _delete(path: str):
    r = requests.delete(BASE_URL + path, headers=_headers("DELETE", path), timeout=10)
    r.raise_for_status()

# ── Market data ─────────────────────────────────────────────────────────────
def fetch_closes(n: int = 55) -> list:
    """Return list of 5-min close prices, oldest first."""
    path = f"/products/{PRODUCT}/candles?granularity={GRANULARITY}"
    raw  = _get(path)
    data = sorted(raw, key=lambda x: x[0])   # oldest first
    return [float(c[4]) for c in data][-n:]  # close prices

def mid_price() -> float:
    t = _get(f"/products/{PRODUCT}/ticker")
    return (float(t["bid"]) + float(t["ask"])) / 2

def get_balance(currency: str) -> float:
    for a in _get("/accounts"):
        if a["currency"] == currency:
            return float(a["available"])
    return 0.0

# ── Indicators ──────────────────────────────────────────────────────────────
def ema(prices: list, n: int) -> float:
    k, e = 2.0 / (n + 1), prices[0]
    for p in prices[1:]:
        e = p * k + e * (1 - k)
    return e

def rsi(prices: list, n: int = 14) -> float:
    d = [prices[i+1] - prices[i] for i in range(len(prices)-1)]
    ag = sum(max(x, 0) for x in d[:n]) / n
    al = sum(max(-x, 0) for x in d[:n]) / n
    for i in range(n, len(d)):
        ag = (ag * (n-1) + max(d[i], 0)) / n
        al = (al * (n-1) + max(-d[i], 0)) / n
    return 100.0 if al == 0 else 100 - 100 / (1 + ag / al)

# ── Order helpers ────────────────────────────────────────────────────────────
def limit_buy(size: float, price: float) -> str:
    r = _post("/orders", {
        "type": "limit", "side": "buy", "product_id": PRODUCT,
        "size": f"{size:.6f}", "price": f"{price:.2f}", "post_only": True,
    })
    return r["id"]

def limit_sell(size: float, price: float) -> str:
    r = _post("/orders", {
        "type": "limit", "side": "sell", "product_id": PRODUCT,
        "size": f"{size:.6f}", "price": f"{price:.2f}", "post_only": True,
    })
    return r["id"]

def stop_sell(size: float, price: float) -> str:
    r = _post("/orders", {
        "type": "stop", "side": "sell", "product_id": PRODUCT,
        "size": f"{size:.6f}", "price": f"{price:.2f}",
    })
    return r["id"]

def market_sell(size: float) -> str:
    r = _post("/orders", {
        "type": "market", "side": "sell", "product_id": PRODUCT,
        "size": f"{size:.6f}",
    })
    return r["id"]

def cancel(oid: str):
    try:
        _delete(f"/orders/{oid}")
    except Exception as e:
        log.warning(f"Cancel {oid[:8]}… failed: {e}")

def order_status(oid: str) -> dict:
    return _get(f"/orders/{oid}")

# ── Bot state ────────────────────────────────────────────────────────────────
class S:
    FLAT, PENDING_BUY, IN_TRADE, EXITING = range(4)
    names = ["FLAT", "PENDING_BUY", "IN_TRADE", "EXITING"]

state         = S.FLAT
buy_oid       = None
tp_oid        = None
sl_oid        = None
entry_price   = 0.0
pos_size      = 0.0

session_start_equity = None
realized_pnl         = 0.0
trade_count          = 0
halted               = False

def _reset():
    global state, buy_oid, tp_oid, sl_oid, entry_price, pos_size
    state = S.FLAT; buy_oid = tp_oid = sl_oid = None
    entry_price = pos_size = 0.0

# ── Main evaluation loop ─────────────────────────────────────────────────────
def evaluate():
    global state, buy_oid, tp_oid, sl_oid, entry_price, pos_size
    global session_start_equity, realized_pnl, trade_count, halted

    if halted:
        log.warning("Bot halted (daily loss cap). Restart to resume.")
        return

    # Gather market state
    closes = fetch_closes(55)
    if len(closes) < SLOW_EMA_LEN + RSI_LEN + 5:
        log.warning("Insufficient candle history — waiting.")
        return

    price    = mid_price()
    usd      = get_balance("USD")
    btc      = get_balance("BTC")
    equity   = usd + btc * price
    fast_e   = ema(closes, FAST_EMA_LEN)
    slow_e   = ema(closes, SLOW_EMA_LEN)
    r        = rsi(closes, RSI_LEN)
    unrealized = (price - entry_price) * pos_size if state == S.IN_TRADE else 0.0

    if session_start_equity is None:
        session_start_equity = equity
        log.info(f"Session open equity : ${equity:.2f}")

    daily_pnl = equity - session_start_equity

    log.info(
        f"[{S.names[state]:^11}]  "
        f"BTC=${price:,.2f}  "
        f"EMA9={fast_e:.2f}  EMA21={slow_e:.2f}  RSI={r:.1f}  "
        f"USD={usd:.2f}  BTC={btc:.6f}  "
        f"DayPnL={daily_pnl:+.2f}  "
        + (f"Unreal={unrealized:+.4f}" if state == S.IN_TRADE else "")
    )

    # Circuit breaker
    if daily_pnl <= -DAILY_LOSS_CAP:
        log.error(f"Daily loss cap hit (${daily_pnl:.2f}). Halting all trading.")
        if state == S.IN_TRADE:
            cancel(tp_oid); cancel(sl_oid)
            market_sell(pos_size)
        halted = True
        return

    # ── FLAT: look for entry signal ──────────────────────────────────────
    if state == S.FLAT:
        bull_cross = fast_e > slow_e
        rsi_ok     = 40 < r < 65    # not overbought, momentum building

        if bull_cross and rsi_ok:
            deploy = usd * CAPITAL_FRAC
            if deploy < MIN_ORDER_USD:
                log.info(f"Signal present but insufficient USD (${usd:.2f}). Skipping.")
                return

            bid  = round(price * 0.9998, 2)   # small discount to stay maker
            size = round(deploy / bid, 6)

            log.info(f">>> ENTRY SIGNAL  BUY {size} BTC @ ${bid:.2f}  "
                     f"(EMA9>{fast_e:.1f} > EMA21={slow_e:.1f}, RSI={r:.1f})")
            try:
                buy_oid     = limit_buy(size, bid)
                entry_price = bid
                pos_size    = size
                state       = S.PENDING_BUY
                log.info(f"    Buy order placed: {buy_oid}")
            except requests.HTTPError as e:
                log.error(f"Entry order failed: {e.response.text}")
        else:
            cross_str = "BULL" if fast_e > slow_e else "BEAR"
            log.info(f"    No signal  cross={cross_str}  RSI={r:.1f}")

    # ── PENDING_BUY: wait for fill ───────────────────────────────────────
    elif state == S.PENDING_BUY:
        o = order_status(buy_oid)
        status = o.get("status")

        if status == "done" and o.get("done_reason") == "filled":
            filled_val  = float(o["executed_value"])
            filled_size = float(o["filled_size"])
            fill_px     = filled_val / filled_size if filled_size else entry_price
            pos_size    = filled_size
            entry_price = fill_px
            tp_px       = round(fill_px * (1 + TAKE_PROFIT), 2)
            sl_px       = round(fill_px * (1 - STOP_LOSS), 2)

            log.info(f">>> FILLED @ ${fill_px:.2f}  size={pos_size:.6f}")
            log.info(f"    TP=${tp_px:.2f} (+{TAKE_PROFIT*100:.1f}%)  "
                     f"SL=${sl_px:.2f} (-{STOP_LOSS*100:.2f}%)")

            try:
                tp_oid = limit_sell(pos_size, tp_px)
                sl_oid = stop_sell(pos_size, sl_px)
                state  = S.IN_TRADE
                trade_count += 1
                log.info(f"    TP order: {tp_oid}  SL order: {sl_oid}")
            except requests.HTTPError as e:
                log.error(f"Exit orders failed: {e.response.text}")
                state = S.IN_TRADE   # still track position

        elif status in ("cancelled", "rejected"):
            log.warning(f"Buy order {buy_oid[:8]}… {status}. Resetting.")
            _reset()

        else:
            log.info(f"    Buy order {buy_oid[:8]}… still {status}.")

    # ── IN_TRADE: monitor TP / SL ────────────────────────────────────────
    elif state == S.IN_TRADE:
        tp_o = order_status(tp_oid)
        sl_o = order_status(sl_oid)

        if tp_o.get("status") == "done":
            exit_val  = float(tp_o.get("executed_value", 0))
            exit_size = float(tp_o.get("filled_size", pos_size))
            exit_px   = exit_val / exit_size if exit_size else price
            pnl       = (exit_px - entry_price) * exit_size
            realized_pnl += pnl
            log.info(f">>> TAKE PROFIT  exit=${exit_px:.2f}  PnL=+${pnl:.4f}  "
                     f"Realized=${realized_pnl:+.4f}  Trades={trade_count}")
            cancel(sl_oid)
            _reset()
            return

        if sl_o.get("status") == "done":
            exit_val  = float(sl_o.get("executed_value", 0))
            exit_size = float(sl_o.get("filled_size", pos_size))
            exit_px   = exit_val / exit_size if exit_size else price
            pnl       = (exit_px - entry_price) * exit_size
            realized_pnl += pnl
            log.info(f">>> STOP LOSS    exit=${exit_px:.2f}  PnL=${pnl:.4f}  "
                     f"Realized=${realized_pnl:+.4f}  Trades={trade_count}")
            cancel(tp_oid)
            _reset()
            return

        # Early exit if trend reverses strongly
        bear_cross = fast_e < slow_e and r > 68
        if bear_cross:
            log.info("Trend reversal — exiting at market.")
            cancel(tp_oid); cancel(sl_oid)
            market_sell(pos_size)
            state = S.EXITING

    # ── EXITING: wait for market sell to clear ────────────────────────────
    elif state == S.EXITING:
        if get_balance("BTC") < 0.00001:
            log.info("Market exit confirmed. Back to FLAT.")
            _reset()

# ── Entry point ──────────────────────────────────────────────────────────────
def main():
    print("=" * 60)
    print("  COINBASE SANDBOX ALGO TRADER  (no real money)")
    print(f"  Product  : {PRODUCT}")
    print(f"  Strategy : EMA{FAST_EMA_LEN}/EMA{SLOW_EMA_LEN} crossover + RSI{RSI_LEN} filter")
    print(f"  TP={TAKE_PROFIT*100:.1f}%  SL={STOP_LOSS*100:.2f}%  "
          f"DailyLossCap=${DAILY_LOSS_CAP}")
    print(f"  Loop     : every {LOOP_INTERVAL}s")
    print("=" * 60)

    if not all([API_KEY, API_SECRET, API_PASSPHRASE]):
        print("\nERROR: missing credentials. Set environment variables:")
        print("  export CB_SANDBOX_KEY=<your key>")
        print("  export CB_SANDBOX_SECRET=<your secret>")
        print("  export CB_SANDBOX_PASSPHRASE=<your passphrase>")
        sys.exit(1)

    log.info("Connecting to Coinbase sandbox…")
    try:
        ticker = _get(f"/products/{PRODUCT}/ticker")
        log.info(f"Connected. {PRODUCT} bid={ticker['bid']}  ask={ticker['ask']}")
    except Exception as e:
        log.error(f"Cannot reach sandbox: {e}")
        sys.exit(1)

    while True:
        try:
            evaluate()
        except requests.HTTPError as e:
            log.error(f"HTTP {e.response.status_code}: {e.response.text[:200]}")
        except requests.Timeout:
            log.warning("Request timed out — will retry next loop.")
        except Exception as e:
            log.error(f"Unexpected error: {e}", exc_info=True)
        time.sleep(LOOP_INTERVAL)

if __name__ == "__main__":
    main()
