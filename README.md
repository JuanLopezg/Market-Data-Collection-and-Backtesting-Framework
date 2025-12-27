# ALGO TRADING  

ALGO TRADING is a C++ crypto trading research project focused on market data collection and strategy backtesting.  
It is mainly used to maintain a local historical dataset and to test trading strategies on top of it.  

The core idea is simple:  
every day at **00:00 UTC**, the system downloads **daily OHLCV data** for the **top 50 cryptocurrencies by volume from Binance**, stores it locally, and makes it available for backtesting.  

---

## What this project does  

- Runs a database service that keeps market data up to date  
- Downloads daily OHLCV candles from Binance  
- Automatically tracks the top 50 coins by volume  
- Stores everything locally to avoid API dependency during backtests  
- Provides a basic backtesting engine to test strategies  

---

## Repository layout (high level)  

The project is roughly split into two parts:  

- `database/`  
  Handles data ingestion, scheduling, and storage  

- `src/`  
  Contains the backtesting engine, strategies, portfolio logic, and shared utilities  

Some things may look duplicated (like `database/` existing in two places), but one is the standalone data service and the other is the database access layer used by the backtester.  

---

## Database service  

The `database/` folder builds a standalone binary responsible for keeping the market data updated.  

Main components include:  

- Downloader: fetches OHLCV data from Binance  
- Scheduler: runs the update process once per day (00:00 UTC)  
- Pairs tracker: determines which symbols to download  
- Database helpers: reading, writing, and basic integrity checks  

The main entry point is:  database_main.cpp  


Logs are written to:  

- `database.log`  
- `database_roll.log`  

---

## Backtesting and research  

Everything related to strategy testing lives under `src/`.  

### Backtest  

- Drives the historical simulation loop  
- Feeds candles to strategies  
- Updates portfolio state over time  

### Strategies  

- All strategies implement a common interface  
- Example strategy included:  
  - `strategy_high_breakout.h`  
- Strategies are meant to be easy to swap or extend  

### Portfolio  

- Tracks positions, balances, and PnL during backtests  

### Data types  

- OHLCV structures and shared market data representations  

### Database access  

- Lightweight layer to read historical data from the local database  

---

## Utilities and shared code  

- `utils/`  
  - JSON helpers  
  - Time utilities (UTC handling, timestamps, etc.)  

- `types/`  
  - Configuration handling  
  - Logging  
  - Scheduler interfaces  

This is mostly plumbing, but it keeps components separated and reusable.  

---

## Build  

The project uses **Meson** as the build system.  

Basic build steps:  

```bash
meson setup build  
meson compile -C build  

Running the data downloader

After building, run the database service binary:

./build/database/database_main
This will:

Start the scheduler

Wait until 00:00 UTC

Download the new daily candles

Update the local database

Depending on configuration, it can also be run manually to force updates.

Running a backtest

Backtests are run via the backtest binary (exact name may vary depending on setup):

./build/backtest/backtest  


Strategies can be modified or added in the strategy/ folder.

Configuration

Configuration is handled through:

config_data.h

config_handler.h

This includes things like:

Data paths

API endpoints

Scheduler settings

Logging options

Notes

This project is under active development

Some parts may be messy or experimental

The focus is correctness and research flexibility, not polish

Disclaimer

This project is for research and educational purposes only.
Nothing here should be considered financial advice.


