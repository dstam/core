/*
   Copyright (C) CFEngine AS

   This file is part of CFEngine 3 - written and maintained by CFEngine AS.

   This program is free software; you can redistribute it and/or modify it
   under the terms of the GNU General Public License as published by the
   Free Software Foundation; version 3.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA

  To the extent this program is licensed as part of the Enterprise
  versions of CFEngine, the applicable Commerical Open Source License
  (COSL) may apply to this file if you as a licensee so wish it. See
  included file COSL.txt.
*/

#include "client_protocol.h"

#include "communication.h"
#include "net.h"

/* libutils */
#include "logging.h"                                            /* Log */

/* TODO remove all includes from libpromises. */
extern char VIPADDRESS[CF_MAX_IP_LEN];
extern char VDOMAIN[];
extern char VFQNAME[];
#include "sysinfo.h"                           /* GetCurrentUsername */
#include "lastseen.h"                          /* LastSaw */
#include "crypto.h"                            /* PublicKeyFile */
#include "files_hashes.h" /* HashString,HashesMatch,HashPubKey,HashPrintSafe */

#include <assert.h>


static bool SetSessionKey(AgentConnection *conn);

/*********************************************************************/

static int SKIPIDENTIFY;

/*********************************************************************/

void SetSkipIdentify(bool enabled)
{
    SKIPIDENTIFY = enabled;
}

/*********************************************************************/

int IdentifyAgent(int sd)
{
    char uname[CF_BUFSIZE], sendbuff[CF_BUFSIZE];
    char dnsname[CF_MAXVARSIZE], localip[CF_MAX_IP_LEN];
    int ret;

    if ((!SKIPIDENTIFY) && (strcmp(VDOMAIN, CF_START_DOMAIN) == 0))
    {
        Log(LOG_LEVEL_ERR, "Undefined domain name");
        return false;
    }

    if (!SKIPIDENTIFY)
    {
        /* First we need to find out the IP address and DNS name of the socket
           we are sending from. This is not necessarily the same as VFQNAME if
           the machine has a different uname from its IP name (!) This can
           happen on poorly set up machines or on hosts with multiple
           interfaces, with different names on each interface ... */
        struct sockaddr_storage myaddr = {0};
        socklen_t myaddr_len = sizeof(myaddr);

        if (getsockname(sd, (struct sockaddr *) &myaddr, &myaddr_len) == -1)
        {
            Log(LOG_LEVEL_ERR, "Couldn't get socket address: %s", GetErrorStr());
            return false;
        }

        /* No lookup, just convert the bound address to string. */
        ret = getnameinfo((struct sockaddr *) &myaddr, myaddr_len,
                          localip, sizeof(localip),
                          NULL, 0, NI_NUMERICHOST);
        if (ret != 0)
        {
            Log(LOG_LEVEL_ERR,
                  "IdentifyAgent: getnameinfo(NI_NUMERICHOST) ERROR: %s",
                  gai_strerror(ret));
            return false;
        }

        /* dnsname: Reverse lookup of the bound IP address. */
        ret = getnameinfo((struct sockaddr *) &myaddr, myaddr_len,
                          dnsname, sizeof(dnsname), NULL, 0, 0);
        if (ret != 0)
        {
            /* getnameinfo doesn't fail on resolution failure, it just prints
             * the IP, so here something else is wrong. */
            Log(LOG_LEVEL_ERR,
                  "getnameinfo ERROR for %s: %s",
                  localip, gai_strerror(ret));
            return false;
        }

        /* getnameinfo() should always return FQDN. Some resolvers will not
         * return FQNAME and missing PTR will give numerical result */
        if ((strlen(VDOMAIN) > 0)                      /* TODO true always? */
            && (!IsIPV6Address(dnsname)) && (!strchr(dnsname, '.')))
        {
            strcat(dnsname, ".");
            strncat(dnsname, VDOMAIN, CF_MAXVARSIZE / 2);
        }

        /* Seems to be a bug in some resolvers that adds garbage, when it just
         * returns the input. */
        if (strncmp(dnsname, localip, strlen(localip)) == 0
            && dnsname[strlen(localip)] != '\0')
        {
            dnsname[strlen(localip)] = '\0';
            Log(LOG_LEVEL_WARNING,
                "WARNING getnameinfo() seems to append garbage to unresolvable IPs, bug mitigated by CFEngine but please report your platform!");
        }
    }
    else
    {
        assert(sizeof(localip) >= sizeof(VIPADDRESS));
        strcpy(localip, VIPADDRESS);

        Log(LOG_LEVEL_VERBOSE,
            "skipidentify was promised, so we are trusting and simply announcing the identity as \"%s\" for this host",
            strlen(VFQNAME) > 0 ? VFQNAME : "skipident");
        if (strlen(VFQNAME) > 0)
        {
            strcpy(dnsname, VFQNAME);
        }
        else
        {
            strcpy(dnsname, "skipident");
        }
    }

/* client always identifies as root on windows */
#ifdef __MINGW32__
    snprintf(uname, sizeof(uname), "%s", "root");
#else
    GetCurrentUserName(uname, sizeof(uname));
#endif

    snprintf(sendbuff, sizeof(sendbuff), "CAUTH %s %s %s %d",
             localip, dnsname, uname, 0);

    if (SendTransaction(sd, sendbuff, 0, CF_DONE) == -1)
    {
        Log(LOG_LEVEL_ERR,
              "!! IdentifyAgent: Could not send auth response");
        return false;
    }

    return true;
}

/*********************************************************************/

int AuthenticateAgent(AgentConnection *conn, bool trust_key)
{
    char sendbuffer[CF_EXPANDSIZE], in[CF_BUFSIZE], *out, *decrypted_cchall;
    BIGNUM *nonce_challenge, *bn = NULL;
    unsigned long err;
    unsigned char digest[EVP_MAX_MD_SIZE];
    int encrypted_len, nonce_len = 0, len, session_size;
    bool implicitly_trust_server;
    char enterprise_field = 'c';
    RSA *server_pubkey = NULL;

    if ((PUBKEY == NULL) || (PRIVKEY == NULL))
    {
        Log(LOG_LEVEL_ERR, "No public/private key pair found at %s", PublicKeyFile(GetWorkDir()));
        return false;
    }

    enterprise_field = CfEnterpriseOptions();
    session_size = CfSessionKeySize(enterprise_field);

/* Generate a random challenge to authenticate the server */

    nonce_challenge = BN_new();
    if (nonce_challenge == NULL)
    {
        Log(LOG_LEVEL_ERR, "Cannot allocate BIGNUM structure for server challenge");
        return false;
    }

    BN_rand(nonce_challenge, CF_NONCELEN, 0, 0);
    nonce_len = BN_bn2mpi(nonce_challenge, in);

    if (FIPS_MODE)
    {
        HashString(in, nonce_len, digest, CF_DEFAULT_DIGEST);
    }
    else
    {
        HashString(in, nonce_len, digest, HASH_METHOD_MD5);
    }

/* We assume that the server bound to the remote socket is the official one i.e. = root's */

    if ((server_pubkey = HavePublicKeyByIP(conn->username, conn->remoteip)))
    {
        implicitly_trust_server = false;
        encrypted_len = RSA_size(server_pubkey);
    }
    else
    {
        implicitly_trust_server = true;
        encrypted_len = nonce_len;
    }

// Server pubkey is what we want to has as a unique ID

    snprintf(sendbuffer, sizeof(sendbuffer), "SAUTH %c %d %d %c", implicitly_trust_server ? 'n': 'y', encrypted_len,
             nonce_len, enterprise_field);

    out = xmalloc(encrypted_len);

    if (server_pubkey != NULL)
    {
        if (RSA_public_encrypt(nonce_len, in, out, server_pubkey, RSA_PKCS1_PADDING) <= 0)
        {
            err = ERR_get_error();
            Log(LOG_LEVEL_ERR, "Public encryption failed = %s", ERR_reason_error_string(err));
            free(out);
            RSA_free(server_pubkey);
            return false;
        }

        memcpy(sendbuffer + CF_RSA_PROTO_OFFSET, out, encrypted_len);
    }
    else
    {
        memcpy(sendbuffer + CF_RSA_PROTO_OFFSET, in, nonce_len);
    }

/* proposition C1 - Send challenge / nonce */

    SendTransaction(conn->sd, sendbuffer, CF_RSA_PROTO_OFFSET + encrypted_len, CF_DONE);

    BN_free(bn);
    BN_free(nonce_challenge);
    free(out);

    if (DEBUG)
    {
        RSA_print_fp(stdout, PUBKEY, 0);
    }

/*Send the public key - we don't know if server has it */
/* proposition C2 */

    memset(sendbuffer, 0, CF_EXPANDSIZE);
    len = BN_bn2mpi(PUBKEY->n, sendbuffer);
    SendTransaction(conn->sd, sendbuffer, len, CF_DONE);        /* No need to encrypt the public key ... */

/* proposition C3 */
    memset(sendbuffer, 0, CF_EXPANDSIZE);
    len = BN_bn2mpi(PUBKEY->e, sendbuffer);
    SendTransaction(conn->sd, sendbuffer, len, CF_DONE);

/* check reply about public key - server can break connection here */

/* proposition S1 */
    memset(in, 0, CF_BUFSIZE);

    if (ReceiveTransaction(conn->sd, in, NULL) == -1)
    {
        Log(LOG_LEVEL_ERR, "Protocol transaction broken off (1): %s", GetErrorStr());
        RSA_free(server_pubkey);
        return false;
    }

    if (BadProtoReply(in))
    {
        Log(LOG_LEVEL_ERR, "%s", in);
        RSA_free(server_pubkey);
        return false;
    }

/* Get challenge response - should be CF_DEFAULT_DIGEST of challenge */

/* proposition S2 */
    memset(in, 0, CF_BUFSIZE);

    if (ReceiveTransaction(conn->sd, in, NULL) == -1)
    {
        Log(LOG_LEVEL_ERR, "Protocol transaction broken off (2): %s", GetErrorStr());
        RSA_free(server_pubkey);
        return false;
    }

    if ((HashesMatch(digest, in, CF_DEFAULT_DIGEST)) || (HashesMatch(digest, in, HASH_METHOD_MD5)))  // Legacy
    {
        if (implicitly_trust_server == false)        /* challenge reply was correct */
        {
            Log(LOG_LEVEL_VERBOSE, ".....................[.h.a.i.l.].................................");
            Log(LOG_LEVEL_VERBOSE, "Strong authentication of server=%s connection confirmed", conn->this_server);
        }
        else
        {
            if (trust_key)
            {
                Log(LOG_LEVEL_VERBOSE, " -> Trusting server identity, promise to accept key from %s=%s", conn->this_server,
                      conn->remoteip);
            }
            else
            {
                Log(LOG_LEVEL_ERR, " !! Not authorized to trust the server=%s's public key (trustkey=false)",
                      conn->this_server);
                RSA_free(server_pubkey);
                return false;
            }
        }
    }
    else
    {
        Log(LOG_LEVEL_ERR, "Challenge response from server %s/%s was incorrect!", conn->this_server,
             conn->remoteip);
        RSA_free(server_pubkey);
        return false;
    }

/* Receive counter challenge from server */

/* proposition S3 */
    memset(in, 0, CF_BUFSIZE);
    encrypted_len = ReceiveTransaction(conn->sd, in, NULL);

    if (encrypted_len <= 0)
    {
        Log(LOG_LEVEL_ERR, "Protocol transaction sent illegal cipher length");
        RSA_free(server_pubkey);
        return false;
    }

    decrypted_cchall = xmalloc(encrypted_len);

    if (RSA_private_decrypt(encrypted_len, in, decrypted_cchall, PRIVKEY, RSA_PKCS1_PADDING) <= 0)
    {
        err = ERR_get_error();
        Log(LOG_LEVEL_ERR, "Private decrypt failed = %s, abandoning",
             ERR_reason_error_string(err));
        RSA_free(server_pubkey);
        return false;
    }

/* proposition C4 */
    if (FIPS_MODE)
    {
        HashString(decrypted_cchall, nonce_len, digest, CF_DEFAULT_DIGEST);
    }
    else
    {
        HashString(decrypted_cchall, nonce_len, digest, HASH_METHOD_MD5);
    }

    if (FIPS_MODE)
    {
        SendTransaction(conn->sd, digest, CF_DEFAULT_DIGEST_LEN, CF_DONE);
    }
    else
    {
        SendTransaction(conn->sd, digest, CF_MD5_LEN, CF_DONE);
    }

    free(decrypted_cchall);

/* If we don't have the server's public key, it will be sent */

    if (server_pubkey == NULL)
    {
        RSA *newkey = RSA_new();

        Log(LOG_LEVEL_VERBOSE, " -> Collecting public key from server!");

        /* proposition S4 - conditional */
        if ((len = ReceiveTransaction(conn->sd, in, NULL)) <= 0)
        {
            Log(LOG_LEVEL_ERR, "Protocol error in RSA authentation from IP %s", conn->this_server);
            return false;
        }

        if ((newkey->n = BN_mpi2bn(in, len, NULL)) == NULL)
        {
            err = ERR_get_error();
            Log(LOG_LEVEL_ERR, "Private key decrypt failed = %s", ERR_reason_error_string(err));
            RSA_free(newkey);
            return false;
        }

        /* proposition S5 - conditional */

        if ((len = ReceiveTransaction(conn->sd, in, NULL)) <= 0)
        {
            Log(LOG_LEVEL_INFO, "Protocol error in RSA authentation from IP %s",
                 conn->this_server);
            RSA_free(newkey);
            return false;
        }

        if ((newkey->e = BN_mpi2bn(in, len, NULL)) == NULL)
        {
            err = ERR_get_error();
            Log(LOG_LEVEL_ERR, "Public key decrypt failed = %s", ERR_reason_error_string(err));
            RSA_free(newkey);
            return false;
        }

        server_pubkey = RSAPublicKey_dup(newkey);
        RSA_free(newkey);
    }

/* proposition C5 */

    if (!SetSessionKey(conn))
    {
        Log(LOG_LEVEL_ERR, "Unable to set session key");
        return false;
    }

    if (conn->session_key == NULL)
    {
        Log(LOG_LEVEL_ERR, "A random session key could not be established");
        RSA_free(server_pubkey);
        return false;
    }

    encrypted_len = RSA_size(server_pubkey);

    out = xmalloc(encrypted_len);

    if (RSA_public_encrypt(session_size, conn->session_key, out, server_pubkey, RSA_PKCS1_PADDING) <= 0)
    {
        err = ERR_get_error();
        Log(LOG_LEVEL_ERR, "Public encryption failed = %s", ERR_reason_error_string(err));
        free(out);
        RSA_free(server_pubkey);
        return false;
    }

    SendTransaction(conn->sd, out, encrypted_len, CF_DONE);

    if (server_pubkey != NULL)
    {
        char buffer[EVP_MAX_MD_SIZE * 4];
        HashPubKey(server_pubkey, conn->digest, CF_DEFAULT_DIGEST);
        Log(LOG_LEVEL_VERBOSE, " -> Public key identity of host \"%s\" is \"%s\"", conn->remoteip,
              HashPrintSafe(CF_DEFAULT_DIGEST, conn->digest, buffer));
        SavePublicKey(conn->username, buffer, server_pubkey);       // FIXME: username is local
        LastSaw(conn->remoteip, conn->digest, LAST_SEEN_ROLE_CONNECT);
    }

    free(out);
    RSA_free(server_pubkey);

    return true;
}

/*********************************************************************/
/* Level                                                             */
/*********************************************************************/

static bool SetSessionKey(AgentConnection *conn)
{
    BIGNUM *bp;
    int session_size = CfSessionKeySize(conn->encryption_type);

    bp = BN_new();

    if (bp == NULL)
    {
        Log(LOG_LEVEL_ERR, "Could not allocate session key");
        return false;
    }

    // session_size is in bytes
    if (!BN_rand(bp, session_size * 8, -1, 0))
    {
        Log(LOG_LEVEL_ERR, "Can't generate cryptographic key");
        BN_clear_free(bp);
        return false;
    }

    conn->session_key = (unsigned char *) bp->d;
    return true;
}

/*********************************************************************/

int BadProtoReply(char *buf)
{
    return (strncmp(buf, "BAD:", 4) == 0);
}

/*********************************************************************/

int OKProtoReply(char *buf)
{
    return (strncmp(buf, "OK:", 3) == 0);
}

/*********************************************************************/

int FailedProtoReply(char *buf)
{
    return (strncmp(buf, CF_FAILEDSTR, strlen(CF_FAILEDSTR)) == 0);
}
