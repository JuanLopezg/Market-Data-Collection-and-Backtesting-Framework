package provider

import (
	"encoding/csv"
	"io"
	"math"
	"sort"
	"strconv"
	"time"
)

// Position history: every position from flat to flat, rebuilt from fills, trading fees
// and funding payments. It feeds the Positions page (open positions' fees/funding so
// far, closed positions with net PnL) and the closed-positions CSV report.
//
// Rules: adding to a position averages its entry price; reducing realizes
// (exit - entry) x quantity (sign flipped for shorts) at the average entry; a fill that
// crosses zero closes the position and opens the opposite one with the remainder, and
// its fee is split by quantity. Funding goes to the position open on that asset at
// that moment (positive = received). Net PnL = gross PnL - fees + funding.

type tradeEventKind int

const (
	tradeFill tradeEventKind = iota
	tradeFunding
)

type tradeEvent struct {
	Time     uint64 // business timestamp; sortable (YYYYMMDD or YYYYMMDD + phase digit)
	Sequence uint64 // tie-break inside one timestamp
	Kind     tradeEventKind
	Asset    string
	Buy      bool
	Quantity float64
	Price    float64
	Fee      float64 // fee paid for this fill (a rebate is negative)
	Amount   float64 // funding: positive received, negative paid
}

type positionLot struct {
	Asset       string
	Side        string // Long or Short
	OpenedAt    uint64
	ClosedAt    uint64
	Quantity    float64 // signed size still open
	MaxQuantity float64 // largest absolute size reached
	EntryPrice  float64 // average entry price
	EntryValue  float64 // sum of entry quantity x price (cost basis)
	ExitValue   float64 // sum of exit quantity x price (proceeds)
	ExitQty     float64
	GrossPnL    float64
	Fees        float64
	Funding     float64
}

func (l positionLot) NetPnL() float64 { return l.GrossPnL - l.Fees + l.Funding }

func (l positionLot) ExitPrice() float64 {
	if l.ExitQty == 0 {
		return 0
	}
	return l.ExitValue / l.ExitQty
}

// quantityEpsilon treats float residue from summing fills as zero.
func quantityEpsilon(reference float64) float64 {
	return 1e-9 * math.Max(1, math.Abs(reference))
}

// buildPositionHistory returns closed positions (oldest first) and the open position per asset.
func buildPositionHistory(events []tradeEvent) ([]positionLot, map[string]*positionLot) {
	ordered := append([]tradeEvent(nil), events...)
	sort.SliceStable(ordered, func(i, j int) bool {
		if ordered[i].Time != ordered[j].Time {
			return ordered[i].Time < ordered[j].Time
		}
		// Funding at a timestamp belongs to the position held up to it.
		if ordered[i].Kind != ordered[j].Kind {
			return ordered[i].Kind == tradeFunding
		}
		return ordered[i].Sequence < ordered[j].Sequence
	})

	closed := []positionLot{}
	open := map[string]*positionLot{}
	openLot := func(event tradeEvent, quantity, fee float64) {
		side, signed := "Long", quantity
		if !event.Buy {
			side, signed = "Short", -quantity
		}
		open[event.Asset] = &positionLot{
			Asset: event.Asset, Side: side, OpenedAt: event.Time, Quantity: signed, MaxQuantity: quantity,
			EntryPrice: event.Price, EntryValue: quantity * event.Price, Fees: fee,
		}
	}

	for _, event := range ordered {
		lot := open[event.Asset]
		if event.Kind == tradeFunding {
			if lot != nil {
				lot.Funding += event.Amount
			}
			continue
		}
		if event.Quantity <= 0 {
			continue
		}
		if lot == nil {
			openLot(event, event.Quantity, event.Fee)
			continue
		}
		increasing := (lot.Quantity > 0) == event.Buy
		if increasing {
			size := math.Abs(lot.Quantity)
			lot.EntryPrice = (size*lot.EntryPrice + event.Quantity*event.Price) / (size + event.Quantity)
			lot.EntryValue += event.Quantity * event.Price
			if event.Buy {
				lot.Quantity += event.Quantity
			} else {
				lot.Quantity -= event.Quantity
			}
			lot.MaxQuantity = math.Max(lot.MaxQuantity, math.Abs(lot.Quantity))
			lot.Fees += event.Fee
			continue
		}

		size := math.Abs(lot.Quantity)
		closing := math.Min(size, event.Quantity)
		direction := 1.0
		if lot.Quantity < 0 {
			direction = -1.0
		}
		lot.GrossPnL += (event.Price - lot.EntryPrice) * closing * direction
		lot.ExitValue += closing * event.Price
		lot.ExitQty += closing
		lot.Fees += event.Fee * closing / event.Quantity
		lot.Quantity -= closing * direction

		if math.Abs(lot.Quantity) <= quantityEpsilon(lot.MaxQuantity) {
			lot.Quantity = 0
			lot.ClosedAt = event.Time
			closed = append(closed, *lot)
			delete(open, event.Asset)
			if remainder := event.Quantity - closing; remainder > quantityEpsilon(event.Quantity) {
				openLot(event, remainder, event.Fee*remainder/event.Quantity)
			}
		}
	}
	return closed, open
}

// tradeTimeLabel formats a business timestamp: YYYYMMDD -> date; YYYYMMDD + phase
// digit -> date plus open (1), intraday (2-8) or close (9).
func tradeTimeLabel(value uint64) string {
	raw := strconv.FormatUint(value, 10)
	if len(raw) == 9 {
		date := formatTradingTimestamp(value / 10)
		switch value % 10 {
		case 1:
			return date + " open"
		case 9:
			return date + " close"
		default:
			return date + " intraday"
		}
	}
	return formatTradingTimestamp(value)
}

// Closed position as the Positions page shows it.
type closedPositionRow struct {
	Asset      string  `json:"asset"`
	Side       string  `json:"side"`
	OpenedAt   string  `json:"openedAt"`
	ClosedAt   string  `json:"closedAt"`
	Quantity   float64 `json:"quantity"` // largest size held
	EntryPrice float64 `json:"entryPrice"`
	ExitPrice  float64 `json:"exitPrice"`
	GrossPnL   float64 `json:"grossPnl"`
	Fees       float64 `json:"fees"`
	Funding    float64 `json:"funding"`
	NetPnL     float64 `json:"netPnl"`
	NetPnLPct  float64 `json:"netPnlPct"` // net PnL / cost basis
}

func closedPositionRows(closed []positionLot) []closedPositionRow {
	rows := make([]closedPositionRow, 0, len(closed))
	for i := len(closed) - 1; i >= 0; i-- { // newest first
		lot := closed[i]
		pct := 0.0
		if lot.EntryValue != 0 {
			pct = lot.NetPnL() / lot.EntryValue * 100
		}
		rows = append(rows, closedPositionRow{
			Asset: lot.Asset, Side: lot.Side, OpenedAt: tradeTimeLabel(lot.OpenedAt), ClosedAt: tradeTimeLabel(lot.ClosedAt),
			Quantity: lot.MaxQuantity, EntryPrice: lot.EntryPrice, ExitPrice: lot.ExitPrice(),
			GrossPnL: lot.GrossPnL, Fees: lot.Fees, Funding: lot.Funding, NetPnL: lot.NetPnL(), NetPnLPct: pct,
		})
	}
	return rows
}

// closedPositionsCSVHeader is the report layout: one row per closed position, amounts in
// the settlement currency (USD). Cost basis = entry value, proceeds = exit value.
var closedPositionsCSVHeader = []string{
	"asset", "side", "opened_at", "closed_at", "quantity", "entry_price", "exit_price",
	"cost_basis", "proceeds", "gross_pnl", "fees", "funding", "net_pnl",
}

// writeClosedPositionsCSV writes the closed positions whose close falls in `year`
// (0 = all years), oldest first.
func writeClosedPositionsCSV(out io.Writer, closed []positionLot, year int) error {
	writer := csv.NewWriter(out)
	if err := writer.Write(closedPositionsCSVHeader); err != nil {
		return err
	}
	number := func(value float64) string { return strconv.FormatFloat(value, 'f', -1, 64) }
	money := func(value float64) string { return strconv.FormatFloat(value, 'f', 8, 64) }
	for _, lot := range closed {
		if year != 0 && closeYear(lot.ClosedAt) != year {
			continue
		}
		if err := writer.Write([]string{
			lot.Asset, lot.Side, tradeTimeLabel(lot.OpenedAt), tradeTimeLabel(lot.ClosedAt),
			number(lot.MaxQuantity), number(lot.EntryPrice), number(lot.ExitPrice()),
			money(lot.EntryValue), money(lot.ExitValue), money(lot.GrossPnL),
			money(lot.Fees), money(lot.Funding), money(lot.NetPnL()),
		}); err != nil {
			return err
		}
	}
	writer.Flush()
	return writer.Error()
}

func closeYear(value uint64) int {
	for value >= 100000000 { // strip the phase digit
		value /= 10
	}
	return int(value / 10000)
}

// closedPositionYears lists the calendar years that have closed positions.
func closedPositionYears(closed []positionLot) []int {
	seen := map[int]bool{}
	years := []int{}
	for _, lot := range closed {
		if year := closeYear(lot.ClosedAt); !seen[year] {
			seen[year] = true
			years = append(years, year)
		}
	}
	sort.Ints(years)
	return years
}

// ClosedPositionsReport is what a provider exposes for the CSV report.
type ClosedPositionsReport struct {
	Closed    []positionLot
	Complete  bool   // false when the fill history did not fit the bounded read
	FundingIn bool   // funding payments are part of the source
	Source    string // short description for the report header/logs
	BuiltAt   time.Time
}
