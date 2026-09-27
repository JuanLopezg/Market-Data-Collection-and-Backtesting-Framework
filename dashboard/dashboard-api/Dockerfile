FROM golang:1.23-alpine AS build
WORKDIR /src
COPY go.mod ./
COPY cmd ./cmd
COPY internal ./internal
RUN CGO_ENABLED=0 GOOS=linux go build -trimpath -ldflags="-s -w" -o /out/dashboard-api ./cmd/dashboard-api \
 && CGO_ENABLED=0 GOOS=linux go build -trimpath -ldflags="-s -w" -o /out/dashboard-watchdog ./cmd/dashboard-watchdog \
 && CGO_ENABLED=0 GOOS=linux go build -trimpath -ldflags="-s -w" -o /out/dashboard-alert-notifier ./cmd/dashboard-alert-notifier

# psql: authenticated bounded PostgreSQL reads.
# sqlite3: canonical market-data reads in explicit read-only mode.
FROM postgres:16-alpine
USER root
RUN apk add --no-cache sqlite \
 && mkdir -p /data/watchdog /data/manual-audit /data/alert-ack /data/notifier \
 && chown postgres:postgres /data/watchdog /data/manual-audit /data/alert-ack /data/notifier
COPY --from=build /out/dashboard-api /dashboard-api
COPY --from=build /out/dashboard-watchdog /dashboard-watchdog
COPY --from=build /out/dashboard-alert-notifier /dashboard-alert-notifier
USER postgres
EXPOSE 8080
HEALTHCHECK --interval=30s --timeout=3s --start-period=5s --retries=3 CMD ["/dashboard-api", "--healthcheck"]
ENTRYPOINT ["/dashboard-api"]
CMD []
