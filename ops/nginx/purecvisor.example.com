server {
    listen 80 default_server;
    listen [::]:80 default_server;
    server_name purecvisor.example.com _;

    location ^~ /.well-known/acme-challenge/ {
        root /var/www/letsencrypt;
        default_type text/plain;
    }

    location / {
        return 301 https://purecvisor.example.com$request_uri;
    }
}

server {
    listen 443 ssl default_server;
    listen [::]:443 ssl default_server;
    server_name _;

    ssl_certificate /etc/letsencrypt/live/purecvisor.example.com/fullchain.pem;
    ssl_certificate_key /etc/letsencrypt/live/purecvisor.example.com/privkey.pem;
    ssl_protocols TLSv1.2 TLSv1.3;
    ssl_prefer_server_ciphers off;

    return 444;
}

server {
    listen 443 ssl http2;
    listen [::]:443 ssl http2;
    server_name purecvisor.example.com;

    ssl_certificate /etc/letsencrypt/live/purecvisor.example.com/fullchain.pem;
    ssl_certificate_key /etc/letsencrypt/live/purecvisor.example.com/privkey.pem;
    ssl_protocols TLSv1.2 TLSv1.3;
    ssl_prefer_server_ciphers off;
    ssl_session_timeout 1d;
    ssl_session_cache shared:PCVSSL:10m;
    ssl_session_tickets off;

    add_header Strict-Transport-Security "max-age=63072000; includeSubDomains; preload" always;
    add_header X-Frame-Options "DENY" always;
    add_header X-Content-Type-Options "nosniff" always;
    add_header Referrer-Policy "strict-origin-when-cross-origin" always;
    add_header Permissions-Policy "accelerometer=(), autoplay=(), camera=(), display-capture=(), encrypted-media=(), fullscreen=(self), geolocation=(), gyroscope=(), microphone=(), midi=(), payment=(), usb=()" always;
    add_header Content-Security-Policy "default-src 'self'; script-src 'self' 'unsafe-inline'; style-src 'self' 'unsafe-inline'; font-src 'self'; connect-src 'self' wss://purecvisor.example.com https://purecvisor.example.com; img-src 'self' data: blob:; frame-src 'none'; object-src 'none'; base-uri 'self'; form-action 'self'" always;

    client_max_body_size 10g;
    limit_conn pcv_conn 30;
    limit_req zone=pcv_api burst=120 nodelay;
    limit_req_status 429;

    error_page 502 503 504 =503 /maintenance.html;

    proxy_hide_header Access-Control-Allow-Origin;
    proxy_hide_header Access-Control-Allow-Credentials;
    proxy_hide_header Access-Control-Allow-Methods;
    proxy_hide_header Access-Control-Allow-Headers;
    proxy_hide_header Content-Security-Policy;
    proxy_hide_header X-Frame-Options;
    proxy_hide_header X-Content-Type-Options;
    proxy_hide_header X-XSS-Protection;
    proxy_hide_header Referrer-Policy;
    proxy_hide_header Permissions-Policy;

    proxy_http_version 1.1;
    proxy_set_header Host $host;
    proxy_set_header X-Real-IP $remote_addr;
    proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
    proxy_set_header X-Forwarded-Proto https;
    proxy_set_header Upgrade $http_upgrade;
    proxy_set_header Connection $connection_upgrade;
    proxy_read_timeout 3600s;
    proxy_send_timeout 3600s;
    proxy_buffering off;
    proxy_redirect http://purecvisor.example.com/ https://purecvisor.example.com/;

    location = /maintenance.html {
        root /usr/local/share/purecvisor/fallback;
        default_type text/html;
        add_header Retry-After "60" always;
        add_header Cache-Control "no-store, max-age=0" always;
        add_header Strict-Transport-Security "max-age=63072000; includeSubDomains; preload" always;
        add_header X-Frame-Options "DENY" always;
        add_header X-Content-Type-Options "nosniff" always;
        add_header Referrer-Policy "strict-origin-when-cross-origin" always;
        add_header Permissions-Policy "accelerometer=(), autoplay=(), camera=(), display-capture=(), encrypted-media=(), fullscreen=(self), geolocation=(), gyroscope=(), microphone=(), midi=(), payment=(), usb=()" always;
        add_header Content-Security-Policy "default-src 'self'; script-src 'self' 'unsafe-inline'; style-src 'self' 'unsafe-inline'; connect-src 'self' https://purecvisor.example.com; img-src 'self' data:; frame-src 'none'; object-src 'none'; base-uri 'self'; form-action 'self'" always;
        try_files /maintenance.html =404;
    }

    location = /maintenance-status.json {
        root /usr/local/share/purecvisor/fallback;
        default_type application/json;
        add_header Cache-Control "no-store, max-age=0" always;
        add_header Strict-Transport-Security "max-age=63072000; includeSubDomains; preload" always;
        add_header X-Content-Type-Options "nosniff" always;
        try_files /maintenance-status.json =404;
    }

    location = /api/v1/auth/token {
        if (-f /usr/local/share/purecvisor/fallback/maintenance.enabled) {
            return 503;
        }
        limit_req zone=pcv_auth burst=3 nodelay;
        proxy_pass http://127.0.0.1:8080;
    }

    # pcv static browser assets served directly to preserve MIME under nosniff
    location ~ ^/ui/.+\.(png|ico|svg|webp|woff|woff2)$ {
        if (-f /usr/local/share/purecvisor/fallback/maintenance.enabled) {
            return 503;
        }
        root /usr/local/share/purecvisor;
        types {
            image/png png;
            image/x-icon ico;
            image/svg+xml svg;
            image/webp webp;
            font/woff woff;
            font/woff2 woff2;
        }
        try_files $uri =404;
    }

    location / {
        if (-f /usr/local/share/purecvisor/fallback/maintenance.enabled) {
            return 503;
        }
        proxy_pass http://127.0.0.1:8080;
    }
}
