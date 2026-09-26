package nats

import (
	"strings"
	"testing"
)

func TestAuditedRuntimeSubjectsContainNoSharedClock(t *testing.T) {
	if SubjectMarketDataUpdated != "market.data.updated.v1" || SubjectFill != "execution.event.fill.v1" {
		t.Fatal("audited trading subject mapping changed")
	}
	if len(AuditedRuntimeSubjects) == 0 {
		t.Fatal("audited runtime subject list is empty")
	}
	for _, subject := range AuditedRuntimeSubjects {
		if strings.Contains(strings.ToLower(subject), "clock") {
			t.Fatalf("legacy clock subject present: %s", subject)
		}
	}
}
