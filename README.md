
# Containerizing SPDK Demo

This version of SPDK is used as a demo for containerizing SPDK application.
This demo is verification that with interrupt mode, SPDK application can be launched
as much more container instances.

In this demo, SPDK hello_sock server is launched by docker with Kata runtime as tens of
container instances. Then launch a hello_sock client by docker to connect any one of
the hello_sock servers.
Users may check if running hello_sock in polling mode, then
CPU% is very high like **100% X Number of container instances**.
But when hello_sock is in interrupt mode, the CPU% will be decreased a lot.


Here we would like to use exmaple/sock/hello_sock as the demo application.
hello_sock has no requirement on any storage resource. It is a simple server/client
demo app, and can work as a server as well as client. When hello_sock works as
a server, it will accept connection, receive message from client and echo the message
back to client. When hello_sock works as a client, it will simply connect a server,
user may send some text in STDIN, and then get the same response text from server
to client's STDOUT.


**Note:**
> **In this version of SPDK hello_sock, it can work both in polling mode (by default)
or in interrupt mode (by set "-E").
Hugepage dependency is removed as experiment, and one extra memsize setting is required
like ("-s  512").**

## Demo env

We recommend user to use docker with Kata Containers runtime， since Kata containers has
a abstract layer to hide host server CPU index.

## Preparation

1. Prepare a docker image with SPDK required dependent libraries by Dockerfile, like

~~~{.text}
# start with the latest Fedora
FROM fedora

# if you are behind a proxy, set that up now
ADD dnf.conf /etc/dnf/dnf.conf

# these are the min dependencies for the SPDK app
RUN dnf install libaio-devel -y
RUN dnf install numactl-devel -y

# set our working dir
WORKDIR /
~~~

2. Then Create your spdk runnable image

~~~{.sh}
sudo docker image build -t spdk_demo:0.1 .
~~~

3. Download this demo version of SPDK to dir like /test/spdk

4. Configure and compile demo version of SPDK

~~~{.sh}
sudo ./configure
sudo make -j18
~~~

>**Note:** SPDK hello_sock app will be passed to container by shared volume

## Containerizing Demo

### Polling mode demo

~~~{.sh}
for i in {1..10}; do
  docker run --rm -d --runtime=kata-runtime -v "/test/spdk:/spdk" spdk_demo:0.1 \
  /bin/bash -c "serverip=\`awk 'END{print \$1}' /etc/hosts\` && \
  /spdk/build/examples/hello_sock -H \$serverip -P 12345 -V  -s 512 -S"
done

# Check your hello_sock server containers
docker ps
~~~

~~~{.sh}
serverip=172.18.0.10
docker run --rm -ti --runtime=kata-runtime -v "/test/spdk:/spdk" spdk_demo:0.1 \
/bin/bash -c "/spdk/build/examples/hello_sock -H $serverip -P 12345 -s 512"
~~~

The output of the hello_sock client should be:
~~~{.text}
[2020-10-30 15:32:25.840628] Starting SPDK v21.01-pre git sha1 a28f7097e / DPDK 20.08.0 initialization...
[2020-10-30 15:32:25.843098] [ DPDK EAL parameters: [2020-10-30 15:32:25.844728] hello_sock [2020-10-30 15:32:25.846321] --no-shconf [2020-10-30 15:32:25.847884] -c 0x1 [2020-10-30 15:32:25.849463] -m 512 [2020-10-30 15:32:25.851063] --no-huge [2020-10-30 15:32:25.852574] --log-level=lib.eal:6 [2020-10-30 15:32:25.854089] --log-level=lib.cryptodev:5 [2020-10-30 15:32:25.855663] --log-level=user1:6 [2020-10-30 15:32:25.857242] --base-virtaddr=0x200000000000 [2020-10-30 15:32:25.858793] --file-prefix=spdk_pid1 [2020-10-30 15:32:25.860356] ]
EAL:   cannot open VFIO container, error 2 (No such file or directory)
EAL: VFIO support could not be initialized
EAL: No legacy callbacks, legacy socket not created
[2020-10-30 15:32:26.007758] app.c: 464:spdk_app_start: *NOTICE*: Total cores available: 1
[2020-10-30 15:32:26.141058] reactor.c: 693:reactor_run: *NOTICE*: Reactor started on core 0
[2020-10-30 15:32:26.148421] hello_sock.c: 566:hello_start: *NOTICE*: Successfully started the application
[2020-10-30 15:32:26.151132] hello_sock.c: 317:hello_sock_connect: *NOTICE*: Connecting to the server on 172.18.0.10:12345 with sock_impl((null))
[2020-10-30 15:32:26.158067] hello_sock.c: 332:hello_sock_connect: *NOTICE*: Connection accepted from (172.18.0.10, 12345) to (172.18.0.12, 45396)
~~~

User may input some text with `enter`, then get server's feed back
~~~{.text}
This is a Containerize SPDK demo
This is a Containerize SPDK demo
It's in polling mode
It's in polling mode
~~~

Check CPU% by top:
~~~
   PID USER      PR  NI    VIRT    RES    SHR S  %CPU %MEM     TIME+ COMMAND
 13841 root      20   0 3250352 313100 293156 S 100.0  0.3   1:05.14 qemu-system-x86
 13971 root      20   0 3250352 313280 293204 S 100.0  0.3   1:03.60 qemu-system-x86
 14235 root      20   0 3250352 312992 293056 S 100.0  0.3   1:00.54 qemu-system-x86
 14368 root      20   0 3250352 312956 292804 S 100.0  0.3   0:59.04 qemu-system-x86
 14501 root      20   0 3250352 312940 292908 S 100.0  0.3   0:57.34 qemu-system-x86
 14634 root      20   0 3250352 312936 292736 S 100.0  0.3   0:55.89 qemu-system-x86
 14773 root      20   0 3250352 313008 292996 S 100.0  0.3   0:54.44 qemu-system-x86
 14904 root      20   0 3250352 312940 292996 S 100.0  0.3   0:53.08 qemu-system-x86
 15064 root      20   0 3250352 313452 293424 S 100.0  0.3   0:40.44 qemu-system-x86
 11767 root      20   0 3250352 313104 293116 S  99.7  0.3   2:38.68 qemu-system-x86
 14101 root      20   0 3250352 313172 293212 S  99.7  0.3   1:02.07 qemu-system-x86
~~~

Close these SPDK hello_sock instances
~~~
sudo docker container rm -f `docker ps -q`
~~~

### Interrupt mode demo

~~~{.sh}
for i in {1..10}; do
  docker run --rm -d --runtime=kata-runtime -v "/test/spdk:/spdk" spdk_demo:0.1 \
  /bin/bash -c "serverip=\`awk 'END{print \$1}' /etc/hosts\` && \
  /spdk/build/examples/hello_sock -H \$serverip -P 12345 -V  -s 512 -E -S"
done

# Check your hello_sock server containers
docker ps
~~~

~~~{.sh}
serverip=172.18.0.10
docker run --rm -ti --runtime=kata-runtime -v "/test/spdk:/spdk" spdk_demo:0.1 \
/bin/bash -c "/spdk/build/examples/hello_sock -H $serverip -P 12345 -s 512 -E"
~~~

The output of the hello_sock client should be:
~~~{.text}
[2020-10-30 15:21:48.863964] thread.c:2004:spdk_interrupt_mode_enable: *NOTICE*: Set SPDK running in interrupt mode.
[2020-10-30 15:21:48.867873] Starting SPDK v21.01-pre git sha1 a28f7097e / DPDK 20.08.0 initialization...
[2020-10-30 15:21:48.869359] [ DPDK EAL parameters: [2020-10-30 15:21:48.871758] hello_sock [2020-10-30 15:21:48.874736] --no-shconf [2020-10-30 15:21:48.877562] -c 0x1 [2020-10-30 15:21:48.880430] -m 512 [2020-10-30 15:21:48.883206] --no-huge [2020-10-30 15:21:48.885584] --log-level=lib.eal:6 [2020-10-30 15:21:48.887980] --log-level=lib.cryptodev:5 [2020-10-30 15:21:48.890343] --log-level=user1:6 [2020-10-30 15:21:48.892737] --base-virtaddr=0x200000000000 [2020-10-30 15:21:48.894830] --file-prefix=spdk_pid1 [2020-10-30 15:21:48.896679] ]
EAL:   cannot open VFIO container, error 2 (No such file or directory)
EAL: VFIO support could not be initialized
EAL: No legacy callbacks, legacy socket not created
[2020-10-30 15:21:49.048467] app.c: 464:spdk_app_start: *NOTICE*: Total cores available: 1
[2020-10-30 15:21:49.178801] reactor.c: 693:reactor_run: *NOTICE*: Reactor started on core 0
[2020-10-30 15:21:49.185671] hello_sock.c: 566:hello_start: *NOTICE*: Successfully started the application
[2020-10-30 15:21:49.188538] hello_sock.c: 317:hello_sock_connect: *NOTICE*: Connecting to the server on 172.18.0.10:12345 with sock_impl((null))
[2020-10-30 15:21:49.195135] hello_sock.c: 332:hello_sock_connect: *NOTICE*: Connection accepted from (172.18.0.10, 12345) to (172.18.0.12, 57024)
~~~

User may input some text with `enter`, then get server's feed back
~~~{.text}
This is a Containerize SPDK demo
This is a Containerize SPDK demo
It's in interrupt mode
It's in interrupt mode
~~~

Check CPU% by top:
~~~
 PID USER      PR  NI    VIRT    RES    SHR S  %CPU %MEM     TIME+ COMMAND
  9937 root      20   0 3250352 312964 293000 S  17.8  0.3   0:38.84 qemu-system-x86
  9807 root      20   0 3250352 313280 293352 S  13.9  0.3   0:30.76 qemu-system-x86
 10369 root      20   0 3250352 313952 293896 S  10.2  0.3   0:13.19 qemu-system-x86
  9422 root      20   0 3250352 313312 293364 S   8.9  0.3   0:23.85 qemu-system-x86
  8765 root      20   0 3250352 313180 293180 S   8.6  0.3   0:23.75 qemu-system-x86
  9163 root      20   0 3250352 313224 293224 S   8.6  0.3   0:23.60 qemu-system-x86
  9554 root      20   0 3250352 313376 293288 S   8.6  0.3   0:22.81 qemu-system-x86
  9029 root      20   0 3250352 313428 293448 S   8.3  0.3   0:23.62 qemu-system-x86
  9291 root      20   0 3250352 312988 292968 S   8.3  0.3   0:23.48 qemu-system-x86
  8899 root      20   0 3250352 313260 293280 S   7.9  0.3   0:23.96 qemu-system-x86
  9679 root      20   0 3250352 313236 293228 S   7.9  0.3   0:21.32 qemu-system-x86
~~~

Close these SPDK hello_sock instances
~~~
sudo docker container rm -f `docker ps -q`
~~~

> **Note:** From the CPU%, we can find that, polling mode will consume too much CPU cores, so the container instances will be limited seriously. While interrupt mode will only consume a few CPU resources, so running spdk application in interrupt mode is more friendly to contaierize SPDK related applications.

# Storage Performance Development Kit

[![Build Status](https://travis-ci.org/spdk/spdk.svg?branch=master)](https://travis-ci.org/spdk/spdk)


The Storage Performance Development Kit ([SPDK](http://www.spdk.io)) provides a set of tools
and libraries for writing high performance, scalable, user-mode storage
applications. It achieves high performance by moving all of the necessary
drivers into userspace and operating in a polled mode instead of relying on
interrupts, which avoids kernel context switches and eliminates interrupt
handling overhead.

The development kit currently includes:

* [NVMe driver](http://www.spdk.io/doc/nvme.html)
* [I/OAT (DMA engine) driver](http://www.spdk.io/doc/ioat.html)
* [NVMe over Fabrics target](http://www.spdk.io/doc/nvmf.html)
* [iSCSI target](http://www.spdk.io/doc/iscsi.html)
* [vhost target](http://www.spdk.io/doc/vhost.html)
* [Virtio-SCSI driver](http://www.spdk.io/doc/virtio.html)

# In this readme

* [Documentation](#documentation)
* [Prerequisites](#prerequisites)
* [Source Code](#source)
* [Build](#libraries)
* [Unit Tests](#tests)
* [Vagrant](#vagrant)
* [AWS](#aws)
* [Advanced Build Options](#advanced)
* [Shared libraries](#shared)
* [Hugepages and Device Binding](#huge)
* [Example Code](#examples)
* [Contributing](#contributing)

<a id="documentation"></a>
## Documentation

[Doxygen API documentation](http://www.spdk.io/doc/) is available, as
well as a [Porting Guide](http://www.spdk.io/doc/porting.html) for porting SPDK to different frameworks
and operating systems.

<a id="source"></a>
## Source Code

~~~{.sh}
git clone https://github.com/spdk/spdk
cd spdk
git submodule update --init
~~~

<a id="prerequisites"></a>
## Prerequisites

The dependencies can be installed automatically by `scripts/pkgdep.sh`.
The `scripts/pkgdep.sh` script will automatically install the bare minimum
dependencies required to build SPDK.
Use `--help` to see information on installing dependencies for optional components

~~~{.sh}
./scripts/pkgdep.sh
~~~

<a id="libraries"></a>
## Build

Linux:

~~~{.sh}
./configure
make
~~~

FreeBSD:
Note: Make sure you have the matching kernel source in /usr/src/ and
also note that CONFIG_COVERAGE option is not available right now
for FreeBSD builds.

~~~{.sh}
./configure
gmake
~~~

<a id="tests"></a>
## Unit Tests

~~~{.sh}
./test/unit/unittest.sh
~~~

You will see several error messages when running the unit tests, but they are
part of the test suite. The final message at the end of the script indicates
success or failure.

<a id="vagrant"></a>
## Vagrant

A [Vagrant](https://www.vagrantup.com/downloads.html) setup is also provided
to create a Linux VM with a virtual NVMe controller to get up and running
quickly.  Currently this has been tested on MacOS, Ubuntu 16.04.2 LTS and
Ubuntu 18.04.3 LTS with the VirtualBox and Libvirt provider.
The [VirtualBox Extension Pack](https://www.virtualbox.org/wiki/Downloads)
or [Vagrant Libvirt] (https://github.com/vagrant-libvirt/vagrant-libvirt) must
also be installed in order to get the required NVMe support.

Details on the Vagrant setup can be found in the
[SPDK Vagrant documentation](http://spdk.io/doc/vagrant.html).

<a id="aws"></a>
## AWS

The following setup is known to work on AWS:
Image: Ubuntu 18.04
Before running  `setup.sh`, run `modprobe vfio-pci`
then: `DRIVER_OVERRIDE=vfio-pci ./setup.sh`

<a id="advanced"></a>
## Advanced Build Options

Optional components and other build-time configuration are controlled by
settings in the Makefile configuration file in the root of the repository. `CONFIG`
contains the base settings for the `configure` script. This script generates a new
file, `mk/config.mk`, that contains final build settings. For advanced configuration,
there are a number of additional options to `configure` that may be used, or
`mk/config.mk` can simply be created and edited by hand. A description of all
possible options is located in `CONFIG`.

Boolean (on/off) options are configured with a 'y' (yes) or 'n' (no). For
example, this line of `CONFIG` controls whether the optional RDMA (libibverbs)
support is enabled:

	CONFIG_RDMA?=n

To enable RDMA, this line may be added to `mk/config.mk` with a 'y' instead of
'n'. For the majority of options this can be done using the `configure` script.
For example:

~~~{.sh}
./configure --with-rdma
~~~

Additionally, `CONFIG` options may also be overridden on the `make` command
line:

~~~{.sh}
make CONFIG_RDMA=y
~~~

Users may wish to use a version of DPDK different from the submodule included
in the SPDK repository.  Note, this includes the ability to build not only
from DPDK sources, but also just with the includes and libraries
installed via the dpdk and dpdk-devel packages.  To specify an alternate DPDK
installation, run configure with the --with-dpdk option.  For example:

Linux:

~~~{.sh}
./configure --with-dpdk=/path/to/dpdk/x86_64-native-linuxapp-gcc
make
~~~

FreeBSD:

~~~{.sh}
./configure --with-dpdk=/path/to/dpdk/x86_64-native-bsdapp-clang
gmake
~~~

The options specified on the `make` command line take precedence over the
values in `mk/config.mk`. This can be useful if you, for example, generate
a `mk/config.mk` using the `configure` script and then have one or two
options (i.e. debug builds) that you wish to turn on and off frequently.

<a id="shared"></a>
## Shared libraries

By default, the build of the SPDK yields static libraries against which
the SPDK applications and examples are linked.
Configure option `--with-shared` provides the ability to produce SPDK shared
libraries, in addition to the default static ones.  Use of this flag also
results in the SPDK executables linked to the shared versions of libraries.
SPDK shared libraries by default, are located in `./build/lib`.  This includes
the single SPDK shared lib encompassing all of the SPDK static libs
(`libspdk.so`) as well as individual SPDK shared libs corresponding to each
of the SPDK static ones.

In order to start a SPDK app linked with SPDK shared libraries, make sure
to do the following steps:

- run ldconfig specifying the directory containing SPDK shared libraries
- provide proper `LD_LIBRARY_PATH`

If DPDK shared libraries are used, you may also need to add DPDK shared
libraries to `LD_LIBRARY_PATH`

Linux:

~~~{.sh}
./configure --with-shared
make
ldconfig -v -n ./build/lib
LD_LIBRARY_PATH=./build/lib/:./dpdk/build/lib/ ./build/bin/spdk_tgt
~~~

<a id="huge"></a>
## Hugepages and Device Binding

Before running an SPDK application, some hugepages must be allocated and
any NVMe and I/OAT devices must be unbound from the native kernel drivers.
SPDK includes a script to automate this process on both Linux and FreeBSD.
This script should be run as root.

~~~{.sh}
sudo scripts/setup.sh
~~~

Users may wish to configure a specific memory size. Below is an example of
configuring 8192MB memory.

~~~{.sh}
sudo HUGEMEM=8192 scripts/setup.sh
~~~

<a id="examples"></a>
## Example Code

Example code is located in the examples directory. The examples are compiled
automatically as part of the build process. Simply call any of the examples
with no arguments to see the help output. You'll likely need to run the examples
as a privileged user (root) unless you've done additional configuration
to grant your user permission to allocate huge pages and map devices through
vfio.

<a id="contributing"></a>
## Contributing

For additional details on how to get more involved in the community, including
[contributing code](http://www.spdk.io/development) and participating in discussions and other activities, please
refer to [spdk.io](http://www.spdk.io/community)
