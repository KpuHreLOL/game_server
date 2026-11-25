# Этап сборки
FROM gcc:11.3 AS build

RUN sed -i '/security/d' /etc/apt/sources.list && \
    apt-get update && \
    apt-get install -y --no-install-recommends \
        python3-pip \
        cmake \
        ca-certificates \
    && rm -rf /var/lib/apt/lists/* \
    && pip3 install --no-cache-dir 'conan==1.*'

WORKDIR /app

# Сначала зависимости — этот слой будет кэшироваться отдельно от исходников.
COPY conanfile.txt /app/
RUN mkdir -p /app/build && \
    cd /app/build && \
    conan install .. --build=missing -s compiler.cppstd=20 -s compiler.libcxx=libstdc++11

COPY CMakeLists.txt /app/
COPY ./src /app/src
COPY ./tests /app/tests

RUN cd /app/build && \
    cmake -DCMAKE_BUILD_TYPE=Release .. && \
    cmake --build . -j"$(nproc)"

# Этап запуска
FROM ubuntu:22.04 AS run

RUN groupadd -r www && useradd -r -g www www

# Бинарник собирается в build/bin благодаря conan_basic_setup().
COPY --from=build /app/build/bin/game_server /app/game_server

# Серверу нужны игровой конфиг и статические файлы.
COPY ./data /app/data
COPY ./static /app/static

USER www

# main ожидает: <game-config-json> <static-root>
ENTRYPOINT ["/app/game_server", "/app/data/config.json", "/app/static"]
