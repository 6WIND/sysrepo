[![Build Status](https://travis-ci.org/sysrepo/sysrepo.svg)](https://travis-ci.org/sysrepo/sysrepo)
[![Coverity Scan Build Status](https://scan.coverity.com/projects/7479/badge.svg)](https://scan.coverity.com/projects/sysrepo-sysrepo)
[![codecov.io](https://codecov.io/github/sysrepo/sysrepo/coverage.svg?branch=master)](https://codecov.io/github/sysrepo/sysrepo?branch=master)
[![GitHub license](https://img.shields.io/badge/license-Apache%20license%202.0-blue.svg)](https://github.com/sysrepo/sysrepo/blob/master/LICENSE)

## Sysrepo
Sysrepo is an [YANG](http://tools.ietf.org/html/rfc6020)-based configuration and operational data store for Unix/Linux applications. 

Applications can use sysrepo to store their configuration modeled by provided YANG model instead of using e.g. flat configuration files. Sysrepo will ensure data consistency of the data stored in the datastore and enforce data constraints defined by YANG model. Applications can currently use [C language API](inc/sysrepo.h) of sysrepo Client Library to access the configuration in the datastore, but the support for other programming languages is planed for later too (since sysrepo uses [Google Protocol Buffers](https://developers.google.com/protocol-buffers/) as the interface between the datastore and client library, writing of a native client library for any programing language that supports GPB is possible).

Sysrepo can be easily integrated with management agents such as [NETCONF](https://tools.ietf.org/html/rfc6241) or [RESTCONF](https://tools.ietf.org/html/draft-ietf-netconf-restconf) servers, using the same client library API that applications use to access their configuration. As of now, sysrepo is integrated with the [Netopeer 2 NETCONF server](https://github.com/CESNET/Netopeer2). This means that applications that use sysrepo to store their configuration can automatically benefit from the ability to control the application via NETCONF.

![Sysrepo Architecture](doc/high_level_architecture.png)

## Status
- July 2016: new features added into the [devel branch](https://github.com/sysrepo/sysrepo/tree/devel): experimental operational data support and event notifications support
- June 2016: new subscription API & changeset retrieval functionality ready, sysrepocfg tool, released as sysrepo [version 0.3.0](https://github.com/sysrepo/sysrepo/releases/tag/v0.3.0)
- May 2016: RPC support and sysrepo plugins infrastructure ready, working on new subscription API & changeset retrieval functionality
- April 2016: full concurrency and locking support ready, generated Python bindings, integrated with [Netopeer 2 NETCONF server](https://github.com/CESNET/Netopeer2), released as sysrepo [version 0.2.0](https://github.com/sysrepo/sysrepo/releases/tag/v0.2.0)
- March 2016: syrepo daemon and data manipulation (edit-config) functionality ready, working on full concurrency and locking support
- February 2016: working on sysrepo daemon, data manipulation (edit-config) functionality
- January 2016: data retrieval (get-config) functionality ready, released as sysrepo [version 0.1.0](https://github.com/sysrepo/sysrepo/releases/tag/v0.1.0)
- December 2015: implementation started - building internal infrastructure, data retrieval (get-config) functionality

## Features
- ability to store / retrieve YANG-modeled data elements adressed by XPath
- startup, running and candidate datastores
- data consistency and constraints enforecment according to YANG model (with help of [libyang](https://github.com/cesnet/libyang) library)
- no single point of failure design (client library is able to instantiate its own sysrepo engine and prerform most of the data-access operations also by itself, whithout the need of contacting system-wide daemon)
- full transaction and concurrency support, conforming all ACID properties (Atomicity, Consistency, Isolation, Durability)
- custom RPC support
- plugins infrastructure for use-cases where there is no daemon to be integrated with sysrepo
- notifications of subscribed applications about the changes made in the datastore
- (IN PROGRESS) operational data support (publishing of application's state data to sysrepo)
- (TODO) ability to subscribe to notifications as a verifier and validate the changes before they are committed
- (TODO) [NACM](https://tools.ietf.org/html/rfc6536) (NETCONF Access Control Model)
- (TODO) [NETCONF Event Notifications](https://tools.ietf.org/html/rfc5277) support
- (TODO) confirmed commit support
- (TODO) bindings / native client libraries for other programming languages (Python, Java, ...)

## Performance
According to our measurements using the [performance unit-test](tests/perf_test.c) and [concurrency unit-test](tests/concurr_test.c), sysrepo is able to handle more than 100 000 of requests per second (100 requests per millisecond) by concurrent access and about a half of it by sequential access on a conventional laptop hardware (read operations sent in-parallel from multiple sessions / sequentially within a single session).

## Build & Installation Steps
See [INSTALL.md](INSTALL.md) file, which contains detailed build and installation steps.

## Usage Examples
See [examples](examples) directory, which contains an example per each data-acess API function.

Also see our [fork of dnsmasq](https://github.com/sysrepo/dnsmasq-sysrepo) that uses sysrepo to store its configuration for short demonstration of how sysrepo can be integrated into an existing application ([see the diff](https://github.com/sysrepo/dnsmasq-sysrepo/compare/a92c41eda58624056242f0c3a71c1efb7bba91b5...master)).

## Documentation
Client Library API, as well as all internal modules of sysrepo are documented with Doxygen comments. To read the documentation, you can navigate to the [nigthly build of documentation on sysrepo.org](http://www.sysrepo.org/static/doc/html/), or [build your own copy](INSTALL.md) of the documentation.

## Contact
For bug reports, please open an issue on GitHub. For general questions and feedback, please post to our [mailing lists](http://lists.sysrepo.org/listinfo/). You are also welcome to subscribe to our mailing lists if you have interest in sysrepo:
- sysrepo-devel@sysrepo.org - if you want to be involved in all technical discussions
- sysrepo-announce@sysrepo.org - if you want to be informed about new releases and released features

## Other Resources
- [sysrepo.org](http://www.sysrepo.org/) - General information about the project
- CESNET's [Netopeer 2](https://github.com/CESNET/Netopeer2) NETCONF Toolset
- CESNET's [libyang](https://github.com/cesnet/libyang) YANG toolkit
- [RFC 6020](http://tools.ietf.org/html/rfc6020) (YANG Data Modeling Language)
- [RFC 6241](https://tools.ietf.org/html/rfc6241) (Network Configuration Protocol - NETCONF)
