# syntax=docker/dockerfile:1

# ---- build: compile a static binary and run the server tests ----
FROM alpine:3.20 AS build
RUN apk add --no-cache build-base bash curl coreutils
WORKDIR /src
COPY Makefile ./
COPY src ./src
COPY tests/server_test.sh ./tests/
COPY public ./public
RUN make STATIC=1 CFLAGS="-O2" && make test-server STATIC=1 CFLAGS="-O2" && strip build/server

# ---- runtime: just the binary and the site, no shell, non-root ----
FROM scratch
COPY --from=build /src/build/server /server
COPY --from=build /src/public /public
USER 65534:65534
ENV ROOT=/public PORT=10000
EXPOSE 10000
ENTRYPOINT ["/server"]
