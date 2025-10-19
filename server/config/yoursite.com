# SPDX-License-Identifier: AGPL-3.0-only
#
# Copyright (c) 2025 Oleksandr Kozlov

# https://certbot.eff.org/instructions?ws=nginx&os=pip
# sudo rm /etc/nginx/sites-enabled/default
# sudo touch /etc/nginx/sites-available/yoursite.com
# sudo ln -s /etc/nginx/sites-available/yoursite.com /etc/nginx/sites-enabled/
# sudo nginx -t
# sudo chmod o+x /home/yoursite
# sudo nginx

server {
    listen 80;
    listen [::]:80;

    server_name yoursite.com www.yoursite.com;

    return 301 https://yoursite.com/preferans$request_uri;
}

server {
    listen 443 ssl;
    listen [::]:443 ssl;
    server_name yoursite.com;

    ssl_certificate /etc/letsencrypt/live/yoursite.com/fullchain.pem;
    ssl_certificate_key /etc/letsencrypt/live/yoursite.com/privkey.pem;
    include /etc/letsencrypt/options-ssl-nginx.conf;
    ssl_dhparam /etc/letsencrypt/ssl-dhparams.pem;

    location = / {
        return 301 /preferans;
    }

    location /preferans {
        alias /home/yoursite/preferans/build-client/bin/;
        index index.html;
        try_files $uri $uri/ /preferans/index.html;
    }

    location = /favicon.ico {
        alias /home/yoursite/preferans/build-client/bin/favicon.ico;
        log_not_found off;
        access_log off;
    }
}

server {
    listen 443 ssl;
    listen [::]:443 ssl;
    server_name www.yoursite.com;

    ssl_certificate /etc/letsencrypt/live/yoursite.com/fullchain.pem;
    ssl_certificate_key /etc/letsencrypt/live/yoursite.com/privkey.pem;
    include /etc/letsencrypt/options-ssl-nginx.conf;
    ssl_dhparam /etc/letsencrypt/ssl-dhparams.pem;

    return 301 https://yoursite.com/preferans$request_uri;
}
