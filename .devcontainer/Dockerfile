FROM espressif/idf:v6.0

ENV LC_ALL=C.UTF-8
ENV LANG=C.UTF-8

RUN apt-get update -y && apt-get install udev tree vim-tiny jq -y
RUN bash -c "source /opt/esp/idf/export.sh && pip install --upgrade pip && pip install git-cliff"

RUN echo "source /opt/esp/idf/export.sh > /dev/null 2>&1" >> ~/.bashrc
RUN echo "set -o vi" >> ~/.bashrc

ENTRYPOINT [ "/opt/esp/entrypoint.sh" ]
CMD ["/bin/bash", "-c"]