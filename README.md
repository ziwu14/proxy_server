#### Introduction

---

Use of good Object-Oriented C++ abstractions and RAII to build a proxy server which support GET, POST, CONNECT requests that adhere to RFC 7234. The proxy server will run as a daemon in the background.

#### Language and Libraries

---

* C++

* boost/asio @ is intended for programmers using C++ for systems programing, where access to operating system functionality such as networking .

  https://github.com/boostorg/asio

* boost/beast @ HTTP and WebSocket built on Boost.Asio in C++11. Beast empowers users to create their own libraries, clients, and servers using HTTP/1 and WebSocket 

  https://github.com/boostorg/beast

#### USAGE

---

1. Setup

```bash
$ export http_proxy=127.0.0.1:12345
$ export https_proxy=127.0.0.1:12345
$ make
$ ./proxy
```

2. Log file
   logs/proxy.log

3. A small test running 38 curls involving repeated http and https websites

```bash
$ ./test.sh
```

