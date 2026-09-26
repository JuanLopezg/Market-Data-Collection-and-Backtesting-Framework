package catalog

import "testing"

func TestCatalogCoversEveryDashboardResource(t *testing.T) {
	if err := Validate(); err != nil {
		t.Fatal(err)
	}
}
