package provider

import (
	"bytes"
	"context"
	"fmt"
	"os"
	"path/filepath"
)

// Closed-positions CSV report for the yearly tax return and for matching against the
// exchange's own trade export. Written into the installation's reports/ folder:
//
//	closed_positions.csv          every closed position, oldest first
//	closed_positions_<YEAR>.csv   positions closed in that calendar year (UTC)
//
// Amounts are in the settlement currency (USD). When the fill history did not fit the
// bounded read the files are not written, so an incomplete report never looks final.

// ClosedPositionsReporter is implemented by providers that can rebuild trade history.
type ClosedPositionsReporter interface {
	ClosedPositionsReport(ctx context.Context) (ClosedPositionsReport, error)
}

// WriteClosedPositionsReports rewrites the report files atomically and returns their names.
func WriteClosedPositionsReports(dir string, report ClosedPositionsReport) ([]string, error) {
	if !report.Complete {
		return nil, fmt.Errorf("trade history incomplete (%s); report not written", report.Source)
	}
	if err := os.MkdirAll(dir, 0o755); err != nil {
		return nil, err
	}
	written := []string{}
	write := func(name string, year int) error {
		var buffer bytes.Buffer
		if err := writeClosedPositionsCSV(&buffer, report.Closed, year); err != nil {
			return err
		}
		if err := writeFileAtomic(filepath.Join(dir, name), buffer.Bytes()); err != nil {
			return err
		}
		written = append(written, name)
		return nil
	}
	if err := write("closed_positions.csv", 0); err != nil {
		return written, err
	}
	for _, year := range closedPositionYears(report.Closed) {
		if err := write(fmt.Sprintf("closed_positions_%d.csv", year), year); err != nil {
			return written, err
		}
	}
	return written, nil
}

func writeFileAtomic(path string, content []byte) error {
	temp, err := os.CreateTemp(filepath.Dir(path), "."+filepath.Base(path)+".*.tmp")
	if err != nil {
		return err
	}
	if _, err := temp.Write(content); err != nil {
		temp.Close()
		os.Remove(temp.Name())
		return err
	}
	if err := temp.Close(); err != nil {
		os.Remove(temp.Name())
		return err
	}
	if err := os.Chmod(temp.Name(), 0o644); err != nil {
		os.Remove(temp.Name())
		return err
	}
	return os.Rename(temp.Name(), path)
}

// WriteClosedPositionsCSV streams the report for one year (0 = all) to the HTTP download.
func WriteClosedPositionsCSV(out *bytes.Buffer, report ClosedPositionsReport, year int) error {
	return writeClosedPositionsCSV(out, report.Closed, year)
}
