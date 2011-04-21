Nginx module to use SPNEGO+GSSAPI+Kerberos for http SSO authentication
======================================================================

Whatsizit
---------

Code 97% stolen from mod_auth_gss_krb5 (http://modgssapache.sf.net);
version 0.0.5.

Compilation
-----------

First you need to compile the spnegohelp dynamic library. 'make' in that
subdirectory should do it, then place it by hand somewhere where linker
and loader can find it by default (probably /usr/lib or perhaps even
/usr/local/lib depending on your setup).

When compiling from source build as usual adding the -add-module option:

  ./configure --add-module=$PATH_TO_MODULE

inside top Nginx source directory.

Configuration
-------------

The module has following directives:

- auth_gss: "on"/"off", for ease of unsecuring while leaving other
  options in the config file,

- auth_gss_realm: what Kerberos realm name to use, for now only used to
  remove it from full user@realm.name,

- auth_gss_keytab: absolute path-name to keytab file containing service
  credentials,

- auth_gss_service_name: what service name to use when acquiring
  credentials. (TOFIX: HTTP but should be a list in case of some other
  browsers wanting perhaps khttp or http)

TOFIX: for now they are all merely location specific. i.e. no way to
specify main or per server defaults, except for ...

Examples
--------

... current "hardcodeds" ;-}

location /topsecret {
  auth_gss on;
  auth_gss_realm LOCALDOMAIN;
  auth_gss_keytab /etc/krb5.keytab;
  auth_gss_service_name HTTP;
}

Additional steps...
-------------------

pray for no segfaults...

TOFIX: perhaps add instructions on how to create the service keytab...
