# Step 14 hotfix v0.14.2

Fixes the post-login blank screen in API mode.

Root cause: `useDashboardResource` extracted class methods from `ApiDashboardDataSource` and invoked them without their instance, so `this.client` was undefined. The synchronous exception escaped the resource promise chain and React rendered a blank page.

Fix: bind each selected data-source method to the active data-source instance and start it through a Promise chain so synchronous errors are converted into normal resource errors.

This package also retains the v0.14.1 Docker/network cleanup: dashboard-web publishes 8080:80 through dashboard-public while remaining connected to dashboard-private for the API.
