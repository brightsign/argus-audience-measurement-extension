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
	"crypto/subtle"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"log"
	"net/http"
	"net/http/httputil"
	"net/url"
	"os"
	"os/signal"
	"strings"
	"syscall"
	"time"
)

func main() {
	var (
		port     = flag.Int("port", 20300, "HTTP port for the dashboard")
		dir      = flag.String("dir", "./dashboard", "directory of dashboard static files")
		upstream = flag.String("upstream", "127.0.0.1:8080", "image-stream-server host:port for /video,/image,/health proxy")
		wsPort   = flag.Int("ws-port", 9001, "mosquitto WebSocket port advertised to the dashboard")
		wsPath   = flag.String("ws-path", "/", "mosquitto WebSocket path advertised to the dashboard")
		topic    = flag.String("topic", "bs/argus/analytics", "MQTT analytics topic advertised to the dashboard")
		vmUp     = flag.String("vm-upstream", "127.0.0.1:20310", "vm_api_server host:port for the /api/vm/ proxy")
		config   = flag.String("config", "", "optional config.json; its dashboard.port / dashboard.ws_port override the flags")
		debug    = flag.Bool("debug", false, "enable request logging")
	)
	flag.Parse()

	// Let config.json (the surface users already edit) drive the ports.
	// A present, non-zero value there overrides the corresponding flag/default.
	vmToken := ""
	if *config != "" {
		if b, err := os.ReadFile(*config); err != nil {
			log.Printf("dashboard-server: config %q not readable (%v); using flags", *config, err)
		} else {
			var c struct {
				Dashboard struct {
					Port    int    `json:"port"`
					WsPort  int    `json:"ws_port"`
					VMToken string `json:"vm_token"`
				} `json:"dashboard"`
			}
			if err := json.Unmarshal(b, &c); err != nil {
				log.Printf("dashboard-server: config %q parse error (%v); using flags", *config, err)
			} else {
				if c.Dashboard.Port > 0 {
					*port = c.Dashboard.Port
				}
				vmToken = c.Dashboard.VMToken
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

	// Visual Memory API (read-only gallery + bounded search POSTs).
	mux.Handle("/api/vm/", newVMHandler(&url.URL{Scheme: "http", Host: *vmUp}, vmToken))
	mux.Handle("/api/pop/", newVMHandler(&url.URL{Scheme: "http", Host: *vmUp}, vmToken))     // D5 Proof of Play: same policy, same upstream
	mux.Handle("/api/display/", newVMHandler(&url.URL{Scheme: "http", Host: *vmUp}, vmToken)) // display-output preview (OS rendered frames): GET only, never exposes the OS gRPC socket

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

	mux.Handle("/", revalidate(http.FileServer(http.Dir(*dir))))

	var handler http.Handler = mux
	if *debug {
		handler = logging(mux)
	}

	srv := &http.Server{
		Addr:              fmt.Sprintf(":%d", *port),
		Handler:           handler,
		ReadHeaderTimeout: 5 * time.Second,
		ReadTimeout:       10 * time.Second,
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

// revalidate makes browsers re-check the dashboard's static files on every load (cheap 304s via Last-Modified)
// instead of applying heuristic freshness, so an upgraded vm.js/index.html is never masked by a stale cached copy.
func revalidate(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Cache-Control", "no-cache")
		next.ServeHTTP(w, r)
	})
}

func logging(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		start := time.Now()
		next.ServeHTTP(w, r)
		log.Printf("[dash] %s %s (%v)", r.Method, r.URL.Path, time.Since(start))
	})
}

// ---- Visual Memory API proxy ------------------------------------------------

const (
	vmMaxBody    = 8 << 20 // 8 MiB cap on request bodies
	vmAuthCap    = 4 << 10
	vmObjMaxBody = 16 << 10 // object index/search requests are tiny JSON documents
	vmCookie     = "vm_token"
	vmTokHeader  = "X-VM-Token"
)

func vmJSON(w http.ResponseWriter, status int, v any) {
	w.Header().Set("Content-Type", "application/json")
	w.Header().Set("Cache-Control", "no-store")
	w.WriteHeader(status)
	_ = json.NewEncoder(w).Encode(v)
}

func vmErr(w http.ResponseWriter, status int, code, msg string) {
	vmJSON(w, status, map[string]string{"error": code, "message": msg})
}

// sameOrigin reports whether Origin (or, failing that, Referer) names r.Host.
func sameOrigin(r *http.Request) bool {
	h := r.Header.Get("Origin")
	if h == "" || h == "null" {
		h = r.Header.Get("Referer")
	}
	if h == "" {
		return false
	}
	u, err := url.Parse(h)
	if err != nil || u.Host == "" {
		return false
	}
	return strings.EqualFold(u.Host, r.Host)
}

func tokenEq(a, b string) bool {
	return subtle.ConstantTimeCompare([]byte(a), []byte(b)) == 1
}

// vmPostAllowed lists the only POST routes the browser may reach (all others are read-only GETs).
func vmPostAllowed(p string) bool {
	return strings.HasPrefix(p, "/api/vm/search/") || p == "/api/pop/search/display" || vmSmallBody(p)
}

// vmSmallBody: JSON-only object endpoints (bounded to vmObjMaxBody).
func vmSmallBody(p string) bool {
	if p == "/api/vm/objects" || p == "/api/vm/objects/search" || p == "/api/pop/media" || p == "/api/pop/sessions" {
		return true
	}
	// /api/pop/media/{id}/references/build and /api/pop/sessions/{id}/verify (exactly one id segment)
	seg := strings.Split(strings.Trim(p, "/"), "/")
	if len(seg) == 5 && seg[0] == "api" && seg[1] == "pop" && seg[2] == "media" && seg[3] != "" && seg[4] == "references" {
		return false
	}
	if len(seg) == 6 && seg[0] == "api" && seg[1] == "pop" && seg[2] == "media" && seg[3] != "" && seg[4] == "references" && seg[5] == "build" {
		return true
	}
	if len(seg) == 5 && seg[0] == "api" && seg[1] == "pop" && seg[2] == "sessions" && seg[3] != "" && seg[4] == "verify" {
		return true
	}
	return false
}

// newVMHandler builds the /api/vm/ handler: method/path allow-list, body cap,
// same-origin check for POST, optional shared-secret token, JSON 502.
func newVMHandler(target *url.URL, token string) http.Handler {
	proxy := httputil.NewSingleHostReverseProxy(target)
	orig := proxy.Director
	proxy.Director = func(r *http.Request) {
		orig(r)
		// Never forward the dashboard credential upstream.
		r.Header.Del(vmTokHeader)
		var keep []string
		for _, c := range r.Cookies() {
			if c.Name != vmCookie {
				keep = append(keep, c.Name+"="+c.Value)
			}
		}
		if len(keep) == 0 {
			r.Header.Del("Cookie")
		} else {
			r.Header.Set("Cookie", strings.Join(keep, "; "))
		}
	}
	proxy.ErrorHandler = func(w http.ResponseWriter, r *http.Request, err error) {
		var mbe *http.MaxBytesError
		if errors.As(err, &mbe) {
			vmErr(w, http.StatusRequestEntityTooLarge, "too_large", "request body exceeds 8 MiB")
			return
		}
		vmJSON(w, http.StatusBadGateway, map[string]string{"error": "vm_api_unavailable"})
	}

	authed := func(r *http.Request) bool {
		if token == "" {
			return true
		}
		if c, err := r.Cookie(vmCookie); err == nil && tokenEq(c.Value, token) {
			return true
		}
		return tokenEq(r.Header.Get(vmTokHeader), token)
	}

	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path == "/api/vm/auth" {
			switch r.Method {
			case http.MethodGet, http.MethodHead:
				vmJSON(w, 200, map[string]bool{"required": token != "", "authenticated": authed(r)})
			case http.MethodPost:
				if !sameOrigin(r) {
					vmErr(w, 403, "forbidden", "cross-origin request refused")
					return
				}
				var body struct {
					Token string `json:"token"`
				}
				if err := json.NewDecoder(http.MaxBytesReader(w, r.Body, vmAuthCap)).Decode(&body); err != nil {
					vmErr(w, 400, "invalid_argument", "expected {\"token\":\"...\"}")
					return
				}
				if token != "" {
					if !tokenEq(body.Token, token) {
						vmErr(w, 401, "unauthorized", "wrong token")
						return
					}
					http.SetCookie(w, &http.Cookie{Name: vmCookie, Value: token, Path: "/api/",
						HttpOnly: true, SameSite: http.SameSiteStrictMode})
				}
				vmJSON(w, 200, map[string]bool{"ok": true})
			default:
				w.Header().Set("Allow", "GET, HEAD, POST")
				vmErr(w, 405, "method_not_allowed", "method not allowed")
			}
			return
		}

		switch r.Method {
		case http.MethodGet, http.MethodHead:
		case http.MethodPost:
			if !vmPostAllowed(r.URL.Path) {
				w.Header().Set("Allow", "GET, HEAD")
				vmErr(w, 405, "method_not_allowed", "POST is only allowed on /api/vm/search/*, /api/pop/search/display, /api/vm/objects, /api/vm/objects/search and the /api/pop/ media, sessions and verify routes")
				return
			}
			if !sameOrigin(r) {
				vmErr(w, 403, "forbidden", "cross-origin request refused")
				return
			}
		default:
			w.Header().Set("Allow", "GET, HEAD, POST")
			vmErr(w, 405, "method_not_allowed", "method not allowed")
			return
		}
		if !authed(r) {
			vmErr(w, 401, "unauthorized", "access token required")
			return
		}
		if r.Method == http.MethodPost {
			limit := int64(vmMaxBody)
			if vmSmallBody(r.URL.Path) {
				limit = vmObjMaxBody
			}
			if r.ContentLength > limit {
				vmErr(w, 413, "too_large", "request body too large")
				return
			}
			r.Body = http.MaxBytesReader(w, r.Body, limit)
		}
		proxy.ServeHTTP(w, r)
	})
}
