#
# Copyright (C) 2018-2019 by frePPLe bv
#
# This library is free software; you can redistribute it and/or modify it
# under the terms of the GNU Affero General Public License as published
# by the Free Software Foundation; either version 3 of the License, or
#
# This library is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU Affero
# General Public License for more details.
#
# You should have received a copy of the GNU Affero General Public
# License along with this program.  If not, see <http://www.gnu.org/licenses/>.
#

#
# STAGE 1: Compile and build the application
#

FROM ubuntu:18.04 as builder

ENV LANG C.UTF-8
ENV LC_ALL C.UTF-8

RUN apt -y -q update && DEBIAN_FRONTEND=noninteractive apt -y install \
  cmake g++ git python3 python3-pip python3-dev \
  libxerces-c3.2 libxerces-c-dev psmisc \
  openssl libssl-dev libpq5 libpq-dev locales

# An alternative to the copy is to clone from git:
# RUN git clone https://github.com/frepple/frepple.git frepple
COPY frepple-*.tar.gz ./

RUN src=`basename --suffix=.tar.gz frepple-*` && \
  tar -xzf *.tar.gz && \
  rm *.tar.gz && \
  cd $src && \
  python3 -m pip install --upgrade pip && \
  python3 -m pip install -r requirements.dev.txt && \
  mkdir build && \
  cd build && \
  cmake .. && \
  cmake --build . --target package

FROM scratch as package
COPY --from=builder frepple-*/build/*.deb .
