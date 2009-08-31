# OpenVAS Testsuite for the NASL interpreter
# $Id$
# Description: Tests for the nasl functions rsa_sign and rsa_public_decrypt
#
# Authors:
# Bernhard Herzog <bernhard.herzog@intevation.de>
#
# Copyright:
# Copyright (c) 2008 Intevation GmbH, http://www.intevation.de
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License version 2,
# as published by the Free Software Foundation
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.

# rsa key generated with ssh-keygen -t rsa -f keyfile
rsa_key_plain = string(
"-----BEGIN RSA PRIVATE KEY-----\n",
"MIICWgIBAAKBgQCYC0xkHWB+7gQ0TJIaBJZjgUcRb9yij1FhpxtO3XM0p2bBslWF\n",
"7A06VUOZibNoRaaBqM8kC7qdYtj62vEDIb4fAo0ZuNfWfRGa6ejE2poUjH7tqce4\n",
"8hMe1inEBVG8l9dxIouloc7m4A+hrHnvPIedv+21dmeJ2y+uuZ1EFFB52wIBIwKB\n",
"gF+SEsKWH2W6LodGErFEtk0ssFQabW1+qC7Az1YkzBJ32jCb+z48mpmx7/ocCl7M\n",
"sc3X0qjxbfyHRo8NRwlIaN9SuJ7S9P/tZfvmsf2IzhAWK+05FkeE+piSDRfbMt4M\n",
"EUzoxI8wKyX/cWXETHrk3ZiAx+xCqbWt3ixjXYEcheobAkEAxl82mE4NlXl6xL53\n",
"Mh11VXFR1n4xGEONOJjcuWBek5MUGkKIL8yAbRotRqtGpngxQFwsbovSdDtI9VoK\n",
"TwfEpwJBAMQ2t+gyjtakbHv33TeR4rawc+ExuPqd+/sHtbUJK04o1wpWOaVgqf97\n",
"OzkmcLWXnMn2Yp3NuUa12VS6GulcY60CQAtV5dzRQprTvd9bV0SxOeegE04V1utx\n",
"kwqMZGJdRzuiASYSbi6d+LXG+0Xd5sevGMHZYZ/qu5Gc/NrR8fXjL88CQQCGi+vS\n",
"a8+pIEpjoqZR/aLGal4WwwJ/902zcwBBn+MuR+Pp1LnQfMxXanHAyebi6562X86Y\n",
"GAoEmfQc12LpKWjrAkAhOX5iAuY5KCYmTzhJKzJP81aTlLLBFMKEuVDV9h6Thh7y\n",
"CsEnYYrHm6FwUDLHAPFFj/ycMPguH3XjUtUaHpQs\n",
"-----END RSA PRIVATE KEY-----\n");

# public key parameters corrsponding to rsa_key_plain
e = raw_string(0x23);
n = raw_string(0x00, 0x98, 0x0b, 0x4c, 0x64, 0x1d, 0x60, 0x7e,
	       0xee, 0x04, 0x34, 0x4c, 0x92, 0x1a, 0x04, 0x96,
	       0x63, 0x81, 0x47, 0x11, 0x6f, 0xdc, 0xa2, 0x8f,
	       0x51, 0x61, 0xa7, 0x1b, 0x4e, 0xdd, 0x73, 0x34,
	       0xa7, 0x66, 0xc1, 0xb2, 0x55, 0x85, 0xec, 0x0d,
	       0x3a, 0x55, 0x43, 0x99, 0x89, 0xb3, 0x68, 0x45,
	       0xa6, 0x81, 0xa8, 0xcf, 0x24, 0x0b, 0xba, 0x9d,
	       0x62, 0xd8, 0xfa, 0xda, 0xf1, 0x03, 0x21, 0xbe,
	       0x1f, 0x02, 0x8d, 0x19, 0xb8, 0xd7, 0xd6, 0x7d,
	       0x11, 0x9a, 0xe9, 0xe8, 0xc4, 0xda, 0x9a, 0x14,
	       0x8c, 0x7e, 0xed, 0xa9, 0xc7, 0xb8, 0xf2, 0x13,
	       0x1e, 0xd6, 0x29, 0xc4, 0x05, 0x51, 0xbc, 0x97,
	       0xd7, 0x71, 0x22, 0x8b, 0xa5, 0xa1, 0xce, 0xe6,
	       0xe0, 0x0f, 0xa1, 0xac, 0x79, 0xef, 0x3c, 0x87,
	       0x9d, 0xbf, 0xed, 0xb5, 0x76, 0x67, 0x89, 0xdb,
	       0x2f, 0xae, 0xb9, 0x9d, 0x44, 0x14, 0x50, 0x79, 0xdb);

# plain text and corresponding SHA1 signature made with rsa_key_plain
plain_text = "abc";
signature = raw_string(0x5b, 0x94, 0x24, 0x6a, 0xf5, 0x34, 0x1c, 0xd5,
		       0x5c, 0x1d, 0xfd, 0x90, 0xef, 0x21, 0xe5, 0xc6,
		       0xef, 0xa7, 0x07, 0x0c, 0xfa, 0x3d, 0xa7, 0x6e,
		       0xb7, 0x88, 0x78, 0x92, 0xb3, 0x75, 0xae, 0xec,
		       0x5b, 0xe8, 0xe7, 0x26, 0x5e, 0xa0, 0xa5, 0xd7,
		       0xe8, 0x07, 0x3d, 0x29, 0xae, 0x8a, 0x6b, 0xea,
		       0xd3, 0xdf, 0x74, 0x90, 0xe9, 0xae, 0x8b, 0xb6,
		       0x48, 0x13, 0x8a, 0xa9, 0x32, 0x7c, 0xa6, 0xe6,
		       0x69, 0x32, 0x04, 0x96, 0xb2, 0x25, 0x2c, 0xa8,
		       0x43, 0xf3, 0x41, 0xb1, 0x60, 0x62, 0x43, 0x10,
		       0x3e, 0xbb, 0x26, 0x13, 0x42, 0x07, 0xc9, 0x6d,
		       0x4b, 0x48, 0x59, 0x18, 0x1a, 0x8e, 0xe5, 0xc6,
		       0xc4, 0xbe, 0x58, 0x04, 0x05, 0x6e, 0x52, 0x7b,
		       0x7e, 0x78, 0xa4, 0xc1, 0xd6, 0xdd, 0x24, 0xf2,
		       0x6d, 0x75, 0x31, 0x60, 0xcb, 0x03, 0xe2, 0x4d,
		       0x67, 0x5e, 0xd7, 0xf9, 0x40, 0xaa, 0xe0, 0xd9);

# another signature where the signature value has the most significant
# bit set.
plain_text2 = "XXXXXX";
signature2 = raw_string(0x91, 0x3c, 0x12, 0x5d, 0x76, 0x6c, 0x29, 0x43,
			0x32, 0x95, 0x04, 0xdb, 0xe3, 0x20, 0xd2, 0x98,
			0xf3, 0xe6, 0xd2, 0x0f, 0x08, 0x53, 0xbb, 0xc7,
			0xde, 0x5f, 0xf3, 0x55, 0xb5, 0x0b, 0x15, 0x84,
			0xf9, 0x28, 0xe9, 0x3e, 0x36, 0x1b, 0xfd, 0x17,
			0x96, 0x5b, 0x30, 0xdb, 0x39, 0x36, 0x2c, 0xe8,
			0xb3, 0xb6, 0xdb, 0x61, 0x96, 0x14, 0x98, 0xd1,
			0xa9, 0x58, 0x99, 0x07, 0x4f, 0x05, 0x5f, 0x7d,
			0x24, 0xe4, 0xac, 0xf2, 0x12, 0xb5, 0x50, 0x3f,
			0x91, 0x87, 0xc8, 0x81, 0x96, 0x47, 0x1d, 0x48,
			0x7a, 0x3b, 0x20, 0xc6, 0x3c, 0x30, 0x12, 0x36,
			0x68, 0xbd, 0x35, 0x03, 0x95, 0x7a, 0x83, 0xb0,
			0xc2, 0x3d, 0x85, 0x24, 0xfe, 0x45, 0xe2, 0x24,
			0x63, 0x89, 0xa2, 0x99, 0x1c, 0x82, 0x9a, 0x54,
			0xfc, 0xf9, 0x62, 0x18, 0x7d, 0x95, 0x8c, 0xa6,
			0x1b, 0x7b, 0xf5, 0x1f, 0xd8, 0x28, 0x31, 0x3b);

function test_rsa_sign(priv, e, n, hash, expected)
{
  local_var signature;

  testcase_start("test_rsa_sign");

  signature = rsa_sign(priv:priv, passphrase:"", data:hash);

  if (hexstr(signature) == hexstr(expected))
    testcase_ok();
  else
    {
      testcase_failed();
      display("signature=");
      display(hexstr(signature));
      display("\n");
    }
}

function test_rsa_public_decrypt(e, n, sig, plaintext, variant)
{
  local_var decrypted, prefix, expected;

  testcase_start("test_rsa_public_decrypt " + variant);

  decrypted = rsa_public_decrypt(sig:sig, e:e, n:n);

  # prefix is the SHA oid.
  prefix="3021300906052b0e03021a05000414";
  expected = strcat(prefix, hexstr(SHA1(plaintext)));
  if (hexstr(decrypted) == expected)
    testcase_ok();
  else
    {
      testcase_failed();
      display("decrypted=");
      display(hexstr(decrypted));
      display("\n");
    }
}

test_rsa_sign(priv:rsa_key_plain, e:e, n:n, hash:SHA1(plain_text),
	      expected=signature);
test_rsa_public_decrypt(e:e, n:n, sig:signature, plaintext:plain_text,
			variant:"(msb not set)");
test_rsa_public_decrypt(e:e, n:n, sig:signature2, plaintext:plain_text2,
			variant:"(msb set)");
