 # Copyright (c) 2024-2026 fei_cong(https://github.com/feicong/frida-course)
FROM --platform=linux/amd64 mcr.microsoft.com/devcontainers/base:noble
LABEL maintainer=feicong

ENV DEBIAN_FRONTEND=noninteractive
ENV TZ="Asia/Shanghai"

COPY ./docker-24.04.sh /docker.sh
RUN bash /docker.sh && rm /docker.sh

RUN apt clean \
    && rm -rf /var/lib/apt/lists/*
    
CMD ["bash"]
