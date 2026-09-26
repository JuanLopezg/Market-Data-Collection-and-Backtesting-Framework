package sqlite

// Canonical market-data schema verified against
// live_trading/market_data_service/src/market_data_store.cpp.
const (
	TableTrackedPairs          = "tracked_pairs"
	TableOHLCVData             = "ohlcv_data"
	TableDateOfStart           = "date_of_start"
	TableMarketVolumeRankDaily = "market_volume_rank_daily"
)
