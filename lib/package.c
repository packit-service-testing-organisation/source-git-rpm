/** \ingroup header
 * \file lib/package.c
 */

#include "system.h"

#include <netinet/in.h>
#include <pthread.h>

#include <rpm/rpmlib.h>			/* XXX RPMSIGTAG, other sig stuff */
#include <rpm/rpmts.h>
#include <rpm/rpmlog.h>
#include <rpm/rpmstring.h>
#include <rpm/rpmkeyring.h>

#include "lib/rpmlead.h"
#include "rpmio/rpmio_internal.h"	/* fd digest bits */
#include "lib/header_internal.h"	/* XXX headerCheck */
#include "lib/rpmvs.h"

#include "debug.h"

/** \ingroup header
 * Translate and merge legacy signature tags into header.
 * @param h		header (dest)
 * @param sigh		signature header (src)
 */
static
void headerMergeLegacySigs(Header h, Header sigh)
{
    HeaderIterator hi;
    struct rpmtd_s td;

    hi = headerInitIterator(sigh);
    for (; headerNext(hi, &td); rpmtdFreeData(&td))
    {
	switch (td.tag) {
	/* XXX Translate legacy signature tag values. */
	case RPMSIGTAG_SIZE:
	    td.tag = RPMTAG_SIGSIZE;
	    break;
	case RPMSIGTAG_PGP:
	    td.tag = RPMTAG_SIGPGP;
	    break;
	case RPMSIGTAG_MD5:
	    td.tag = RPMTAG_SIGMD5;
	    break;
	case RPMSIGTAG_GPG:
	    td.tag = RPMTAG_SIGGPG;
	    break;
	case RPMSIGTAG_PGP5:
	    td.tag = RPMTAG_SIGPGP5;
	    break;
	case RPMSIGTAG_PAYLOADSIZE:
	    td.tag = RPMTAG_ARCHIVESIZE;
	    break;
	case RPMSIGTAG_SHA1:
	case RPMSIGTAG_SHA256:
	case RPMSIGTAG_DSA:
	case RPMSIGTAG_RSA:
	default:
	    if (!(td.tag >= HEADER_SIGBASE && td.tag < HEADER_TAGBASE))
		continue;
	    break;
	}
	if (!headerIsEntry(h, td.tag)) {
	    switch (td.type) {
	    case RPM_NULL_TYPE:
		continue;
		break;
	    case RPM_CHAR_TYPE:
	    case RPM_INT8_TYPE:
	    case RPM_INT16_TYPE:
	    case RPM_INT32_TYPE:
	    case RPM_INT64_TYPE:
		if (td.count != 1)
		    continue;
		break;
	    case RPM_STRING_TYPE:
	    case RPM_BIN_TYPE:
		if (td.count >= 16*1024)
		    continue;
		break;
	    case RPM_STRING_ARRAY_TYPE:
	    case RPM_I18NSTRING_TYPE:
		continue;
		break;
	    }
	    (void) headerPut(h, &td, HEADERPUT_DEFAULT);
	}
    }
    headerFreeIterator(hi);
}

/**
 * Remember current key id.
 * XXX: This s*** needs to die. Hook it into keyring or sumthin...
 * @param keyid		signature keyid
 * @return		0 if new keyid, otherwise 1
 */
static int stashKeyid(unsigned int keyid)
{
    static pthread_mutex_t keyid_lock = PTHREAD_MUTEX_INITIALIZER;
    static const unsigned int nkeyids_max = 256;
    static unsigned int nkeyids = 0;
    static unsigned int nextkeyid  = 0;
    static unsigned int * keyids;

    int i;
    int seen = 0;

    if (keyid == 0)
	return 0;

    /* Just pretend we didn't see the keyid if we fail to lock */
    if (pthread_mutex_lock(&keyid_lock))
	return 0;

    if (keyids != NULL)
    for (i = 0; i < nkeyids; i++) {
	if (keyid == keyids[i]) {
	    seen = 1;
	    goto exit;
        }
    }

    if (nkeyids < nkeyids_max) {
	nkeyids++;
	keyids = xrealloc(keyids, nkeyids * sizeof(*keyids));
    }
    if (keyids)		/* XXX can't happen */
	keyids[nextkeyid] = keyid;
    nextkeyid++;
    nextkeyid %= nkeyids_max;

exit:
    pthread_mutex_unlock(&keyid_lock);
    return seen;
}

static int handleHdrVS(struct rpmsinfo_s *sinfo, void *cbdata)
{
    char **buf  = cbdata;
    if (buf) {
	char *vsmsg = rpmsinfoMsg(sinfo);
	*buf = rstrscat(buf, "\n", vsmsg, NULL);
	free(vsmsg);
    }
    return 1;
}

static void updateHdrDigests(rpmDigestBundle bundle, struct hdrblob_s *blob)
{
    int32_t ildl[2] = { htonl(blob->ril), htonl(blob->rdl) };

    rpmDigestBundleUpdate(bundle, rpm_header_magic, sizeof(rpm_header_magic));
    rpmDigestBundleUpdate(bundle, ildl, sizeof(ildl));
    rpmDigestBundleUpdate(bundle, blob->pe, (blob->ril * sizeof(*blob->pe)));
    rpmDigestBundleUpdate(bundle, blob->dataStart, blob->rdl);
}

rpmRC headerCheck(rpmts ts, const void * uh, size_t uc, char ** msg)
{
    rpmRC rc = RPMRC_FAIL;
    rpmVSFlags vsflags = rpmtsVSFlags(ts) | RPMVSF_NEEDPAYLOAD;
    rpmKeyring keyring = rpmtsGetKeyring(ts, 1);
    struct hdrblob_s blob;

    if (hdrblobInit(uh, uc, 0, 0, &blob, msg) == RPMRC_OK) {
	struct rpmvs_s *vs = rpmvsCreate(0, vsflags, keyring);
	rpmDigestBundle bundle = rpmDigestBundleNew();

	rpmswEnter(rpmtsOp(ts, RPMTS_OP_DIGEST), 0);

	rpmvsInit(vs, &blob, bundle);
	rpmvsInitRange(vs, RPMSIG_HEADER);
	updateHdrDigests(bundle, &blob);
	rpmvsFiniRange(vs, RPMSIG_HEADER);

	rc = rpmvsVerify(vs, RPMSIG_VERIFIABLE_TYPE, handleHdrVS, msg);

	rpmswExit(rpmtsOp(ts, RPMTS_OP_DIGEST), uc);

	if (rc == RPMRC_OK && msg != NULL && *msg == NULL)
	    rasprintf(msg, "Header sanity check: OK");

	rpmDigestBundleFree(bundle);
	rpmvsFree(vs);
    }

    rpmKeyringFree(keyring);

    return rc;
}

rpmRC rpmReadHeader(rpmts ts, FD_t fd, Header *hdrp, char ** msg)
{
    char *buf = NULL;
    struct hdrblob_s blob;
    Header h = NULL;
    rpmRC rc = RPMRC_FAIL;		/* assume failure */

    if (hdrp)
	*hdrp = NULL;
    if (msg)
	*msg = NULL;

    if (hdrblobRead(fd, 1, 1, RPMTAG_HEADERIMMUTABLE, &blob, &buf) != RPMRC_OK)
	goto exit;

    /* OK, blob looks sane, load the header. */
    rc = hdrblobImport(&blob, 0, &h, &buf);
    
exit:
    if (hdrp && h && rc == RPMRC_OK)
	*hdrp = headerLink(h);
    headerFree(h);

    if (msg != NULL && *msg == NULL && buf != NULL) {
	*msg = buf;
    } else {
	free(buf);
    }

    return rc;
}

static
void applyRetrofits(Header h)
{
    /*
     * Make sure that either RPMTAG_SOURCERPM or RPMTAG_SOURCEPACKAGE
     * is set. Use a simple heuristic to find the type if both are unset.
     */
    if (!headerIsEntry(h, RPMTAG_SOURCERPM) && !headerIsEntry(h, RPMTAG_SOURCEPACKAGE)) {
	/* the heuristic needs the compressed file list */
	if (headerIsEntry(h, RPMTAG_OLDFILENAMES))
	    headerConvert(h, HEADERCONV_COMPRESSFILELIST);
	if (headerIsSourceHeuristic(h)) {
	    /* Retrofit RPMTAG_SOURCEPACKAGE to srpms for compatibility */
	    uint32_t one = 1;
	    headerPutUint32(h, RPMTAG_SOURCEPACKAGE, &one, 1);
	} else {
	    /*
	     * Make sure binary rpms have RPMTAG_SOURCERPM set as that's
	     * what we use for differentiating binary vs source elsewhere.
	     */
	    headerPutString(h, RPMTAG_SOURCERPM, "(none)");
	}
    }

    /*
     * Convert legacy headers on the fly. Not having immutable region
     * equals a truly ancient package, do full retrofit. OTOH newer
     * packages might have been built with --nodirtokens, test and handle
     * the non-compressed filelist case separately.
     */
    if (!headerIsEntry(h, RPMTAG_HEADERIMMUTABLE))
	headerConvert(h, HEADERCONV_RETROFIT_V3);
    else if (headerIsEntry(h, RPMTAG_OLDFILENAMES))
	headerConvert(h, HEADERCONV_COMPRESSFILELIST);
}

struct pkgdata_s {
    const char *fn;
    rpmRC rc;
};

static int handlePkgVS(struct rpmsinfo_s *sinfo, void *cbdata)
{
    struct pkgdata_s *pkgdata = cbdata;
    int lvl = RPMLOG_DEBUG;
    char *vsmsg = rpmsinfoMsg(sinfo);
    switch (sinfo->rc) {
    case RPMRC_OK:		/* Signature is OK. */
	break;
    case RPMRC_NOTTRUSTED:	/* Signature is OK, but key is not trusted. */
    case RPMRC_NOKEY:		/* Public key is unavailable. */
	/* XXX Print NOKEY/NOTTRUSTED warning only once. */
	if (stashKeyid(sinfo->keyid) == 0)
	    lvl = RPMLOG_WARNING;
	break;
    case RPMRC_NOTFOUND:	/* Signature/digest not present. */
	lvl = RPMLOG_WARNING;
	break;
    default:
    case RPMRC_FAIL:		/* Signature does not verify. */
	lvl = RPMLOG_ERR;
	break;
    }

    rpmlog(lvl, "%s: %s\n", pkgdata->fn, vsmsg);

    /* Remember actual return code, but don't override a previous failure */
    if (sinfo->rc && pkgdata->rc != RPMRC_FAIL)
	pkgdata->rc = sinfo->rc;

    /* Preserve traditional behavior for now: only failure prevents read */
    if (sinfo->rc != RPMRC_FAIL)
	sinfo->rc = RPMRC_OK;

    free(vsmsg);
    return 1;
}

rpmRC rpmReadPackageFile(rpmts ts, FD_t fd, const char * fn, Header * hdrp)
{
    char *msg = NULL;
    Header h = NULL;
    Header sigh = NULL;
    hdrblob blob = NULL;
    hdrblob sigblob = NULL;
    rpmVSFlags vsflags = rpmtsVSFlags(ts) | RPMVSF_NEEDPAYLOAD;
    rpmKeyring keyring = rpmtsGetKeyring(ts, 1);
    struct rpmvs_s *vs = rpmvsCreate(0, vsflags, keyring);
    struct pkgdata_s pkgdata = {
	.fn = fn ? fn : Fdescr(fd),
	.rc = RPMRC_OK,
    };

    /* XXX: lots of 3rd party software relies on the behavior */
    if (hdrp)
	*hdrp = NULL;

    rpmRC rc = rpmpkgRead(vs, fd, &sigblob, &blob, &msg);
    if (rc)
	goto exit;

    /* Actually all verify discovered signatures and digests */
    rc = RPMRC_FAIL;
    if (!rpmvsVerify(vs, RPMSIG_VERIFIABLE_TYPE, handlePkgVS, &pkgdata)) {
	/* Finally import the headers and do whatever required retrofits etc */
	if (hdrp) {
	    if (hdrblobImport(sigblob, 0, &sigh, &msg))
		goto exit;
	    if (hdrblobImport(blob, 0, &h, &msg))
		goto exit;

	    /* Append (and remap) signature tags to the metadata. */
	    headerMergeLegacySigs(h, sigh);
	    applyRetrofits(h);

	    /* Bump reference count for return. */
	    *hdrp = headerLink(h);
	}
	rc = RPMRC_OK;
    }

    /* If there was a "substatus" (NOKEY in practise), return that instead */
    if (rc == RPMRC_OK && pkgdata.rc)
	rc = pkgdata.rc;

exit:
    if (rc && msg)
	rpmlog(RPMLOG_ERR, "%s: %s\n", Fdescr(fd), msg);
    hdrblobFree(sigblob);
    hdrblobFree(blob);
    headerFree(sigh);
    headerFree(h);
    rpmKeyringFree(keyring);
    rpmvsFree(vs);
    free(msg);

    return rc;
}



