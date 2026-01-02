# Prometheus & Grafana Setup for BrightSign Players

This document describes how to set up Prometheus and Grafana for monitoring the Argus analytics system on BrightSign players.

## Overview

The monitoring stack consists of:
- **Argus Exporter**: Converts MQTT analytics to Prometheus metrics (port 9101)
- **Prometheus**: Time-series database for metrics collection (port 9090)
- **Grafana**: Dashboard visualization (port 3000)

## File Formats

| File Type | Format | Extension |
|-----------|--------|-----------|
| Application config | JSON | `.json` |
| Grafana dashboard | JSON | `.json` |
| Prometheus config | YAML | `.yml` |
| Prometheus rules | YAML | `.yml` |
| Grafana provisioning | YAML | `.yml` |

## BrightSign Path Mapping

On BrightSign players, all configuration and data files must be stored under `/storage/flash` instead of standard Linux paths.

| Standard Linux Path | BrightSign Player Path |
|---------------------|------------------------|
| `/etc/prometheus/prometheus.yml` | `/storage/flash/prometheus/prometheus.yml` |
| `/etc/prometheus/rules/` | `/storage/flash/prometheus/rules/` |
| `/var/lib/prometheus/` (data) | `/storage/flash/prometheus/data/` |
| `/etc/grafana/provisioning/dashboards/` | `/storage/flash/grafana/provisioning/dashboards/` |
| `/var/lib/grafana/dashboards/` | `/storage/flash/grafana/dashboards/` |
| `/var/lib/grafana/` (data) | `/storage/flash/grafana/data/` |

## Directory Structure

```
/storage/flash/
├── prometheus/
│   ├── prometheus.yml              # YAML - Prometheus server config
│   ├── data/                       # Prometheus time-series data
│   └── rules/
│       ├── recording-rules.yml     # YAML - pre-computed aggregations
│       └── alerting-rules.yml      # YAML - alert definitions
├── grafana/
│   ├── grafana.ini                 # Grafana server config
│   ├── data/                       # Grafana SQLite DB, sessions, etc.
│   ├── provisioning/
│   │   └── dashboards/
│   │       └── argus.yml           # YAML - dashboard provider config
│   └── dashboards/
│       └── argus/
│           └── argus-analytics.json  # JSON - dashboard definition
└── configs/
    └── argus-config.json           # JSON - application config
```

## Configuration Files

### Prometheus Configuration

```yaml
# /storage/flash/prometheus/prometheus.yml
global:
  scrape_interval: 15s
  evaluation_interval: 15s

rule_files:
  - /storage/flash/prometheus/rules/*.yml

scrape_configs:
  - job_name: 'prometheus'
    static_configs:
      - targets: ['localhost:9090']

  - job_name: 'argus-exporter'
    scrape_interval: 10s
    scrape_timeout: 5s
    static_configs:
      - targets: ['localhost:9101']

# Optional: Alertmanager configuration
# alerting:
#   alertmanagers:
#     - static_configs:
#         - targets: ['localhost:9093']
```

### Grafana Dashboard Provisioning

```yaml
# /storage/flash/grafana/provisioning/dashboards/argus.yml
apiVersion: 1

providers:
  - name: 'Argus'
    orgId: 1
    folder: 'BrightSign Analytics'
    type: file
    disableDeletion: false
    updateIntervalSeconds: 30
    options:
      path: /storage/flash/grafana/dashboards/argus
```

### Grafana Server Configuration

```ini
# /storage/flash/grafana/grafana.ini
[paths]
data = /storage/flash/grafana/data
logs = /storage/flash/grafana/logs
plugins = /storage/flash/grafana/plugins
provisioning = /storage/flash/grafana/provisioning

[server]
http_port = 3000

[security]
admin_user = admin
admin_password = admin

[auth.anonymous]
enabled = true
org_role = Viewer
```

## Auto-Installation

### Step 1: Create Directory Structure

```bash
# Create all required directories
mkdir -p /storage/flash/prometheus/rules
mkdir -p /storage/flash/prometheus/data
mkdir -p /storage/flash/grafana/provisioning/dashboards
mkdir -p /storage/flash/grafana/dashboards/argus
mkdir -p /storage/flash/grafana/data
mkdir -p /storage/flash/grafana/logs
mkdir -p /storage/flash/grafana/plugins
```

### Step 2: Copy Configuration Files

Source files are in the argus-exporter repository under `dashboard/` and `configs/`.

```bash
# Copy Prometheus configuration
cp prometheus.yml /storage/flash/prometheus/
cp recording-rules.yml /storage/flash/prometheus/rules/
cp alerting-rules.yml /storage/flash/prometheus/rules/

# Copy Grafana provisioning and dashboard
cp argus.yml /storage/flash/grafana/provisioning/dashboards/
cp argus-analytics.json /storage/flash/grafana/dashboards/argus/

# Copy Grafana server config
cp grafana.ini /storage/flash/grafana/
```

### Step 3: Start Services

```bash
# Start Prometheus with BrightSign paths
prometheus \
  --config.file=/storage/flash/prometheus/prometheus.yml \
  --storage.tsdb.path=/storage/flash/prometheus/data \
  --web.listen-address=:9090

# Start Grafana with BrightSign paths
grafana-server \
  --config=/storage/flash/grafana/grafana.ini \
  --homepath=/usr/share/grafana
```

### Step 4: Verify Installation

1. **Prometheus targets**: `http://<player-ip>:9090/targets`
   - Verify `argus-exporter` target shows as UP

2. **Prometheus rules**: `http://<player-ip>:9090/rules`
   - Verify recording and alerting rules are loaded

3. **Prometheus metrics**: `http://<player-ip>:9090/graph?g0.expr=argus_visitors_total`
   - Query should return data if analytics are running

4. **Grafana dashboard**: `http://<player-ip>:3000`
   - Default login: admin/admin
   - Dashboard should auto-appear in "BrightSign Analytics" folder

## Dashboard Features

The Argus Analytics dashboard includes:

### Key Performance Indicators (Row 1)
- Current Occupancy
- Visitors (in time range)
- Visitors/Hour rate
- Attention Rate (% who dwelt AND gazed)
- Average Dwell Time
- Gaze Rate

### Traffic Analysis (Row 2)
- Occupancy Over Time with gaze overlay
- Visitor Flow (entries/exits)
- Cumulative Visitors

### Engagement Analysis (Row 3)
- Engagement Distribution pie chart
- Engagement Funnel (visitor -> engaged -> attention)
- Engagement Over Time stacked area
- Attention Events Rate

### Collapsed Sections
- **Gaze Analytics**: Gaze activity, duration distribution, percentiles
- **Dwell Time Analysis**: Distribution, percentiles, averages
- **Movement Patterns**: Entry/exit directions, direction changes, speed
- **System Health**: FPS, NPU load, message rate
- **Processing Health**: Detector/tracker FPS, queue latency, dropped frames

## Prometheus Recording Rules

Pre-computed metrics for dashboard performance:

| Rule | Description |
|------|-------------|
| `argus:visitors_per_hour` | Visitor rate per hour |
| `argus:attention_rate_percent` | Attention conversion rate |
| `argus:engagement_percent` | Engagement distribution percentages |
| `argus:dwell_p50/p90/p99` | Dwell time percentiles |
| `argus:gaze_duration_p50/p90/p99` | Gaze duration percentiles |
| `argus:gaze_proportion_percent` | % of occupants gazing |
| `argus:visitors_hourly` | Hourly visitor count |

## Alerting Rules

### Critical Alerts
| Alert | Condition | Description |
|-------|-----------|-------------|
| `ArgusExporterDown` | `up == 0` for 1m | Exporter unreachable |
| `ArgusNoData` | No messages for 5m | MQTT pipeline broken |
| `ArgusCriticallyLowFPS` | FPS < 10 for 1m | Analytics unusable |
| `ArgusCriticalNPULoad` | NPU > 95% for 2m | System may crash |

### Warning Alerts
| Alert | Condition | Description |
|-------|-----------|-------------|
| `ArgusLowFPS` | FPS < 20 for 2m | Performance degraded |
| `ArgusHighNPULoad` | NPU > 85% for 5m | High resource usage |
| `ArgusUnusualTrafficDrop` | Traffic 70%+ below yesterday | Anomaly detection |

### Info Alerts
| Alert | Condition | Description |
|-------|-----------|-------------|
| `ArgusLowAttentionRate` | < 5% for 1h | Content needs improvement |
| `ArgusNoVisitors` | 0 visitors for 1h | No foot traffic |
| `ArgusExcellentEngagement` | > 30% highly engaged | Content performing well |

## Troubleshooting

### No Data in Dashboard
1. Check Prometheus targets are UP: `http://<player-ip>:9090/targets`
2. Verify argus-exporter is running: `curl http://localhost:9101/metrics`
3. Check MQTT broker is receiving data: `mosquitto_sub -t 'bs/argus/analytics'`
4. Verify device/stream template variables match your data

### Slow Dashboard
1. Enable recording rules for pre-computed aggregations
2. Reduce time range selection
3. Check Prometheus resource usage

### Alerts Not Firing
1. Verify rules loaded: `http://<player-ip>:9090/rules`
2. Check Alertmanager is configured (if using alerts)
3. Look for rule evaluation errors in Prometheus logs

## Requirements

- **Prometheus**: 2.30+ (for recording rules support)
- **Grafana**: 9.0+ (uses built-in panels: stat, gauge, timeseries, piechart, barchart, bargauge)
- **Argus Exporter**: Running and connected to MQTT broker
- **Storage**: Minimum 1GB free space for Prometheus data retention

## Related Documentation

- [MQTT Message Format](mqtt-message-format.md) - Analytics message structure
- [Argus Exporter README](/external/argus-exporter/README.md) - Exporter configuration
- [Dashboard README](/external/argus-exporter/dashboard/README.md) - Dashboard details
