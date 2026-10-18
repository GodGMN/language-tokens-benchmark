# Build context is the repo root: the page is generated from results/.
#   docker build -f site/Dockerfile -t token-golf .
FROM python:3.12-alpine AS build
WORKDIR /src
COPY results/dataset.json results/dataset.csv results/
COPY site site
RUN python3 site/build.py

FROM nginx:1.27-alpine
COPY --from=build /src/site/dist /usr/share/nginx/html
COPY site/nginx.conf /etc/nginx/conf.d/default.conf
EXPOSE 80
CMD ["nginx", "-g", "daemon off;"]
