#######################
#
#  Licensed to the Apache Software Foundation (ASF) under one or more contributor license
#  agreements.  See the NOTICE file distributed with this work for additional information regarding
#  copyright ownership.  The ASF licenses this file to you under the Apache License, Version 2.0
#  (the "License"); you may not use this file except in compliance with the License.  You may obtain
#  a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
#  Unless required by applicable law or agreed to in writing, software distributed under the License
#  is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
#  or implied. See the License for the specific language governing permissions and limitations under
#  the License.
#
#######################


add_library(inknet STATIC
        AcceptOptions.cc
        ALPNSupport.cc
        AsyncSignalEventIO.cc
        BIO_fastopen.cc
        BoringSSLUtils.cc
        Connection.cc
        EventIO.cc
        Inline.cc
        YamlSNIConfig.cc
        Net.cc
        NetHandler.cc
        NetVCOptions.cc
        NetAcceptEventIO.cc
        NetVConnection.cc
        PollCont.cc
        PreWarmManager.cc
        ProxyProtocol.cc
        ReadWriteEventIO.cc
        Socks.cc
        SSLCertLookup.cc
        SSLClientCoordinator.cc
        SSLClientUtils.cc
        SSLConfig.cc
        SSLSecret.cc
        SSLDiags.cc
        SSLInternal.cc
        SSLNetAccept.cc
        SSLNetProcessor.cc
        SSLNetVConnection.cc
        SSLNextProtocolAccept.cc
        SSLNextProtocolSet.cc
        SSLSNIConfig.cc
        SSLStats.cc
        SSLSessionCache.cc
        SSLSessionTicket.cc
        SSLUtils.cc
        OCSPStapling.cc
        TLSBasicSupport.cc
        TLSCertSwitchSupport.cc
        TLSEarlyDataSupport.cc
        TLSKeyLogger.cc
        TLSSessionResumptionSupport.cc
        TLSSNISupport.cc
        TLSTunnelSupport.cc
        UDPEventIO.cc
        UDPIOEvent.cc
        UnixConnection.cc
        UnixNet.cc
        UnixNetAccept.cc
        UnixNetPages.cc
        UnixNetProcessor.cc
        UnixNetVConnection.cc
        UnixUDPConnection.cc
        UnixUDPNet.cc
        SSLDynlock.cc
        SNIActionPerformer.cc
        )
add_library(ts::inknet ALIAS inknet)

if(TS_USE_QUIC)
    add_subdirectory(quic)

    target_sources(inknet
        PRIVATE
            QUICClosedConCollector.cc
            QUICMultiCertConfigLoader.cc
            QUICNet.cc
            QUICNetProcessor_quiche.cc
            QUICNetVConnection_quiche.cc
            QUICNextProtocolAccept_quiche.cc
            QUICPacketHandler_quiche.cc
    )

    target_link_libraries(inknet
        PUBLIC
            quiche::quiche
            ts::quic
    )
endif()


if(BUILD_REGRESSION_TESTING)
    target_sources(inknet PRIVATE NetVCTest.cc)
endif()


target_compile_options(inknet PUBLIC -Wno-deprecated-declarations)
target_include_directories(inknet PUBLIC
        ${CMAKE_SOURCE_DIR}/iocore/dns
        ${CMAKE_SOURCE_DIR}/iocore/io_uring
        ${CMAKE_SOURCE_DIR}/iocore/aio
        ${CMAKE_SOURCE_DIR}/iocore/net
        ${CMAKE_SOURCE_DIR}/iocore/cache
        ${CMAKE_SOURCE_DIR}/iocore/hostdb
        ${CMAKE_SOURCE_DIR}/proxy/hdrs
        ${CMAKE_SOURCE_DIR}/proxy/shared
        ${CMAKE_SOURCE_DIR}/proxy/http
        ${CMAKE_SOURCE_DIR}/proxy/http/remap
        ${CMAKE_SOURCE_DIR}/src/traffic_server
)

target_link_libraries(inknet
    PUBLIC
        ts::inkevent
        ts::proxy
        ts::records
        ts::tscore
        OpenSSL::Crypto
        OpenSSL::SSL
    PRIVATE
        yaml-cpp::yaml-cpp
)

# Fails to link because of circular dep with proxy (ParentSelection)
# add_executable(test_net unit_tests/test_ProxyProtocol.cc)
# target_link_libraries(test_net records_p inknet inkevent tscore yaml-cpp libswoc)
# target_include_directories(test_net PRIVATE
#         ${CMAKE_SOURCE_DIR}/include
#         ${CMAKE_SOURCE_DIR}/iocore/net
#         ${CMAKE_SOURCE_DIR}/iocore/eventsystem
#         ${CATCH_INCLUDE_DIR}
# )
