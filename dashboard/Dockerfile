FROM node:22-alpine AS build
WORKDIR /app
COPY package*.json ./
RUN npm install
COPY . .
ARG VITE_DASHBOARD_DATA_MODE=mock
ARG VITE_DASHBOARD_API_BASE_URL=/api
ARG VITE_DASHBOARD_API_TIMEOUT_MS=8000
ENV VITE_DASHBOARD_DATA_MODE=$VITE_DASHBOARD_DATA_MODE
ENV VITE_DASHBOARD_API_BASE_URL=$VITE_DASHBOARD_API_BASE_URL
ENV VITE_DASHBOARD_API_TIMEOUT_MS=$VITE_DASHBOARD_API_TIMEOUT_MS
RUN npm run build

FROM caddy:2-alpine
COPY Caddyfile /etc/caddy/Caddyfile
COPY --from=build /app/dist /srv
EXPOSE 80
HEALTHCHECK --interval=30s --timeout=3s --start-period=10s --retries=3 CMD wget -qO- http://127.0.0.1/ >/dev/null || exit 1
