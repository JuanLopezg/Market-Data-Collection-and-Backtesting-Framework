FROM ubuntu:24.04

RUN useradd --system --uid 10001 --create-home algotrading

WORKDIR /opt/algotrading
COPY bin/ /opt/algotrading/bin/
COPY lib/ /opt/algotrading/lib/
COPY config/ /opt/algotrading/config/

ENV LD_LIBRARY_PATH=/opt/algotrading/lib
ENV PATH=/opt/algotrading/bin:${PATH}

USER 10001
