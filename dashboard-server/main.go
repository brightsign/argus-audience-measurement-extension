// Command dashboard-server serves the Argus analytics dashboard (static files in
// ./dashboard) on its own port, separate from the image-stream-server. It
// reverse-proxies the live-frame endpoints (/video, /image, /health) to the
// image-stream-server so the dashboard can embed the MJPEG stream same-origin,
// and it advertises the mosquitto WebSocket port via /config.json so the
// browser can subscribe to analytics directly.
//
// The dashboard itself gets live analytics over MQTT-WebSockets from the
// bundled mosquitto broker; this server never touches MQTT.
package main

import (
	"context"
	"encoding/json"
	"flag"
	"fmt"
	"log"
	"net/http"
	"net/http/httputil"
	"net/url"
	"os"
	"os/signal"
	"syscall"
	"time"
)

func main() {
	var (
		port     = flag.Int("port", 8081, "HTTP port for the dashboard")
		dir      = flag.String("dir", "./dashboard", "directory of dashboard static files")
		upstream = flag.String("upstream", "127.0.0.1:8080", "image-stream-server host:port for /video,/image,/health proxy")
		wsPort   = flag.Int("ws-port", 9001, "mosquitto WebSocket port advertised to the dashboard")
		wsPath   = flag.String("ws-path", "/", "mosquitto WebSocket path advertised to the dashboard")
		topic    = flag.String("topic", "bs/argus/analytics", "MQTT analytics topic advertised to the dashboard")
		config   = flag.String("config", "", "optional config.json; its dashboard.port / dashboard.ws_port override the flags")
		debug    = flag.Bool("debug", false, "enable request logging")
	)
	flag.Parse()

	// Let config.json (the surface users already edit) drive the ports.
	// A present, non-zero value there overrides the corresponding flag/default.
	if *config != "" {
		if b, err := os.ReadFile(*config); err != nil {
			log.Printf("dashboard-server: config %q not readable (%v); using flags", *config, err)
		} else {
			var c struct {
				Dashboard struct {
					Port   int `json:"port"`
					WsPort int `json:"ws_port"`
				} `json:"dashboard"`
			}
			if err := json.Unmarshal(b, &c); err != nil {
				log.Printf("dashboard-server: config %q parse error (%v); using flags", *config, err)
			} else {
				if c.Dashboard.Port > 0 {
					*port = c.Dashboard.Port
				}
				if c.Dashboard.WsPort > 0 {
					*wsPort = c.Dashboard.WsPort
				}
			}
		}
	}

	if _, err := os.Stat(*dir); err != nil {
		log.Fatalf("dashboard dir %q not accessible: %v", *dir, err)
	}

	// Reverse proxy for the live-frame endpoints served by image-stream-server.
	// FlushInterval -1 flushes writes immediately so the multipart MJPEG stream
	// is not buffered.
	target := &url.URL{Scheme: "http", Host: *upstream}
	proxy := httputil.NewSingleHostReverseProxy(target)
	proxy.FlushInterval = -1

	mux := http.NewServeMux()
	for _, p := range []string{"/video", "/image", "/health"} {
		mux.Handle(p, proxy)
	}

	// Connection hints for the browser so the dashboard is not hard-coded.
	mux.HandleFunc("/config.json", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		w.Header().Set("Cache-Control", "no-store")
		_ = json.NewEncoder(w).Encode(map[string]any{
			"wsPort": *wsPort,
			"wsPath": *wsPath,
			"topic":  *topic,
		})
	})

	mux.Handle("/", http.FileServer(http.Dir(*dir)))

	var handler http.Handler = mux
	if *debug {
		handler = logging(mux)
	}

	srv := &http.Server{
		Addr:        fmt.Sprintf(":%d", *port),
		Handler:     handler,
		ReadTimeout: 10 * time.Second,
		// 0: the proxied MJPEG stream is long-lived.
		WriteTimeout: 0,
		IdleTimeout:  120 * time.Second,
	}

	c := make(chan os.Signal, 1)
	signal.Notify(c, os.Interrupt, syscall.SIGTERM)
	go func() {
		<-c
		log.Println("dashboard-server: shutting down...")
		ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
		defer cancel()
		_ = srv.Shutdown(ctx)
		os.Exit(0)
	}()

	log.Printf("dashboard-server: serving %s on :%d (frames proxied from %s, ws :%d)", *dir, *port, *upstream, *wsPort)
	if err := srv.ListenAndServe(); err != nil && err != http.ErrServerClosed {
		log.Fatalf("dashboard-server failed: %v", err)
	}
}

func logging(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		start := time.Now()
		next.ServeHTTP(w, r)
		log.Printf("[dash] %s %s (%v)", r.Method, r.URL.Path, time.Since(start))
	})
}
