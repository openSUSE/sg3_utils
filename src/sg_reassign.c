/*
 * Copyright (c) 2005-2016 Douglas Gilbert.
 * All rights reserved.
 * Use of this source code is governed by a BSD-style
 * license that can be found in the BSD_LICENSE file.
 */

#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <getopt.h>
#include <limits.h>
#define __STDC_FORMAT_MACROS 1
#include <inttypes.h>

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include "sg_lib.h"
#include "sg_cmds_basic.h"
#include "sg_cmds_extra.h"
#include "sg_unaligned.h"
#include "sg_pr2serr.h"

/* A utility program originally written for the Linux OS SCSI subsystem.
 *
 * This utility invokes the REASSIGN BLOCKS SCSI command to reassign
 * an existing (possibly damaged) lba on a direct access device (e.g.
 * a disk) to a new physical location. The previous contents is
 * recoverable then it is written to the remapped lba otherwise
 * vendor specific data is written.
 */

static const char * version_str = "1.19 20160612";

#define DEF_DEFECT_LIST_FORMAT 4        /* bytes from index */

#define MAX_NUM_ADDR 1024

#ifndef UINT32_MAX
#define UINT32_MAX ((uint32_t)-1)
#endif


static struct option long_options[] = {
        {"address", 1, 0, 'a'},
        {"dummy", 0, 0, 'd'},
        {"eight", 1, 0, 'e'},
        {"grown", 0, 0, 'g'},
        {"help", 0, 0, 'h'},
        {"hex", 0, 0, 'H'},
        {"longlist", 1, 0, 'l'},
        {"primary", 0, 0, 'p'},
        {"verbose", 0, 0, 'v'},
        {"version", 0, 0, 'V'},
        {0, 0, 0, 0},
};

static void
usage()
{
    pr2serr("Usage: sg_reassign [--address=A,A...] [--dummy] [--eight=0|1] "
            "[--grown]\n"
            "                   [--help] [--hex] [--longlist=0|1] "
            "[--primary] [--verbose]\n"
            "                   [--version] DEVICE\n"
            "  where:\n"
            "    --address=A,A...|-a A,A...    comma separated logical block "
            "addresses\n"
            "                                  one or more, assumed to be "
            "decimal\n"
            "    --address=-|-a -    read stdin for logical block "
            "addresses\n"
            "    --dummy|-d          prepare but do not execute REASSIGN "
            "BLOCKS command\n"
            "    --eight=0|1\n"
            "      -e 0|1            force eight byte (64 bit) lbas "
            "when 1,\n"
            "                        four byte (32 bit) lbas when 0 "
            "(def)\n"
            "    --grown|-g          fetch grown defect list length, "
            "don't reassign\n"
            "    --help|-h           print out usage message\n"
            "    --hex|-H            print response in hex (for '-g' or "
            "'-p')\n"
            "    --longlist=0|1\n"
            "       -l 0|1           use 4 byte list length when 1, safe to "
            "ignore\n"
            "                        (def: 0 (2 byte list length))\n"
            "    --primary|-p        fetch primary defect list length, "
            "don't reassign\n"
            "    --verbose|-v        increase verbosity\n"
            "    --version|-V        print version string and exit\n\n"
            "Perform a SCSI REASSIGN BLOCKS command (or READ DEFECT LIST)\n");
}

/* Read numbers (up to 64 bits in size) from command line (comma (or
 * (single) space) separated list) or from stdin (one per line, comma
 * separated list or space separated list). Assumed decimal unless prefixed
 * by '0x', '0X' or contains trailing 'h' or 'H' (which indicate hex).
 * Returns 0 if ok, or 1 if error. */
static int
build_lba_arr(const char * inp, uint64_t * lba_arr,
              int * lba_arr_len, int max_arr_len)
{
    int in_len, k, j, m;
    const char * lcp;
    int64_t ll;
    char * cp;
    char * c2p;

    if ((NULL == inp) || (NULL == lba_arr) ||
        (NULL == lba_arr_len))
        return 1;
    lcp = inp;
    in_len = strlen(inp);
    if (0 == in_len)
        *lba_arr_len = 0;
    if ('-' == inp[0]) {        /* read from stdin */
        char line[1024];
        int off = 0;

        for (j = 0; j < 512; ++j) {
            if (NULL == fgets(line, sizeof(line), stdin))
                break;
            // could improve with carry_over logic if sizeof(line) too small
            in_len = strlen(line);
            if (in_len > 0) {
                if ('\n' == line[in_len - 1]) {
                    --in_len;
                    line[in_len] = '\0';
                }
            }
            if (in_len < 1)
                continue;
            lcp = line;
            m = strspn(lcp, " \t");
            if (m == in_len)
                continue;
            lcp += m;
            in_len -= m;
            if ('#' == *lcp)
                continue;
            k = strspn(lcp, "0123456789aAbBcCdDeEfFhHxX ,\t");
            if ((k < in_len) && ('#' != lcp[k])) {
                pr2serr("build_lba_arr: syntax error at line %d, pos %d\n",
                        j + 1, m + k + 1);
                return 1;
            }
            for (k = 0; k < 1024; ++k) {
                ll = sg_get_llnum_nomult(lcp);
                if (-1 != ll) {
                    if ((off + k) >= max_arr_len) {
                        pr2serr("build_lba_arr: array length exceeded\n");
                        return 1;
                    }
                    lba_arr[off + k] = (uint64_t)ll;
                    lcp = strpbrk(lcp, " ,\t");
                    if (NULL == lcp)
                        break;
                    lcp += strspn(lcp, " ,\t");
                    if ('\0' == *lcp)
                        break;
                } else {
                    if ('#' == *lcp) {
                        --k;
                        break;
                    }
                    pr2serr("build_lba_arr: error in line %d, at pos %d\n",
                            j + 1, (int)(lcp - line + 1));
                    return 1;
                }
            }
            off += (k + 1);
        }
        *lba_arr_len = off;
    } else {        /* list of numbers (default decimal) on command line */
        k = strspn(inp, "0123456789aAbBcCdDeEfFhHxX, ");
        if (in_len != k) {
            pr2serr("build_lba_arr: error at pos %d\n", k + 1);
            return 1;
        }
        for (k = 0; k < max_arr_len; ++k) {
            ll = sg_get_llnum_nomult(lcp);
            if (-1 != ll) {
                lba_arr[k] = (uint64_t)ll;
                cp = (char *)strchr(lcp, ',');
                c2p = (char *)strchr(lcp, ' ');
                if (NULL == cp)
                    cp = c2p;
                if (NULL == cp)
                    break;
                if (c2p && (c2p < cp))
                    cp = c2p;
                lcp = cp + 1;
            } else {
                pr2serr("build_lba_arr: error at pos %d\n",
                        (int)(lcp - inp + 1));
                return 1;
            }
        }
        *lba_arr_len = k + 1;
        if (k == max_arr_len) {
            pr2serr("build_lba_arr: array length exceeded\n");
            return 1;
        }
    }
    return 0;
}


int
main(int argc, char * argv[])
{
    int sg_fd, res, c, num, k, j;
    int dummy = 0;
    int got_addr = 0;
    int eight = -1;
    int addr_arr_len = 0;
    int grown = 0;
    int do_hex = 0;
    int longlist = 0;
    int primary = 0;
    int verbose = 0;
    const char * device_name = NULL;
    uint64_t addr_arr[MAX_NUM_ADDR];
    unsigned char param_arr[4 + (MAX_NUM_ADDR * 8)];
    char b[80];
    int param_len = 4;
    int ret = 0;

    while (1) {
        int option_index = 0;

        c = getopt_long(argc, argv, "a:de:ghHl:pvV", long_options,
                        &option_index);
        if (c == -1)
            break;

        switch (c) {
        case 'a':
            memset(addr_arr, 0, sizeof(addr_arr));
            if (0 != build_lba_arr(optarg, addr_arr, &addr_arr_len,
                                   MAX_NUM_ADDR)) {
                pr2serr("bad argument to '--address'\n");
                return SG_LIB_SYNTAX_ERROR;
            }
            got_addr = 1;
            break;
        case 'd':
            dummy = 1;
            break;
        case 'e':
            num = sscanf(optarg, "%d", &res);
            if ((1 == num) && ((0 == res) || (1 == res)))
                eight = res;
            else {
                pr2serr("value for '--eight=' must be 0 or 1\n");
                return SG_LIB_SYNTAX_ERROR;
            }
            break;
        case 'g':
            grown = 1;
            break;
        case 'h':
        case '?':
            usage();
            return 0;
        case 'H':
            ++do_hex;
            break;
        case 'l':
            num = sscanf(optarg, "%d", &res);
            if ((1 == num) && ((0 == res) || (1 == res)))
                longlist = res;
            else {
                pr2serr("value for '--longlist=' must be 0 or 1\n");
                return SG_LIB_SYNTAX_ERROR;
            }
            break;
        case 'p':
            primary = 1;
            break;
        case 'v':
            ++verbose;
            break;
        case 'V':
            pr2serr("version: %s\n", version_str);
            return 0;
        default:
            pr2serr("unrecognised option code 0x%x ??\n", c);
            usage();
            return SG_LIB_SYNTAX_ERROR;
        }
    }
    if (optind < argc) {
        if (NULL == device_name) {
            device_name = argv[optind];
            ++optind;
        }
        if (optind < argc) {
            for (; optind < argc; ++optind)
                pr2serr("Unexpected extra argument: %s\n", argv[optind]);
            usage();
            return SG_LIB_SYNTAX_ERROR;
        }
    }
    if (NULL == device_name) {
        pr2serr("missing device name!\n");
        usage();
        return SG_LIB_SYNTAX_ERROR;
    }
    if (grown || primary) {
        if (got_addr) {
            pr2serr("can't have '--address=' with '--grown' or '--primary'\n");
            usage();
            return SG_LIB_SYNTAX_ERROR;
        }
    } else if ((0 == got_addr) || (addr_arr_len < 1)) {
        pr2serr("need at least one address (see '--address=')\n");
        usage();
        return SG_LIB_SYNTAX_ERROR;
    }
    if (got_addr) {
        for (k = 0; k < addr_arr_len; ++k) {
            if (addr_arr[k] >= UINT32_MAX) {
                if (eight < 0) {
                    eight = 1;
                    break;
                } else if (0 == eight) {
                    pr2serr("address number %d exceeds 32 bits so "
                            "'--eight=0' invalid\n", k + 1);
                    return SG_LIB_SYNTAX_ERROR;
                }
            }
        }
        if (eight < 0)
            eight = 0;

        k = 4;
        for (j = 0; j < addr_arr_len; ++j) {
            if (eight) {
                sg_put_unaligned_be64(addr_arr[j], param_arr + k);
                k += 8;
            } else {
                sg_put_unaligned_be32((uint32_t)addr_arr[j], param_arr + k);
                k += 4;
            }
        }
        param_len = k;
        k -= 4;
        if (longlist)
            sg_put_unaligned_be32((uint32_t)k, param_arr + 0);
        else
            sg_put_unaligned_be16((uint16_t)k, param_arr + 2);
    }

    sg_fd = sg_cmds_open_device(device_name, 0 /* rw */, verbose);
    if (sg_fd < 0) {
        pr2serr("open error: %s: %s\n", device_name, safe_strerror(-sg_fd));
        return SG_LIB_FILE_ERROR;
    }

    if (got_addr) {
        if (dummy) {
            pr2serr(">>> dummy: REASSIGN BLOCKS not executed\n");
            if (verbose) {
                pr2serr("  Would have reassigned these blocks:\n");
                for (j = 0; j < addr_arr_len; ++j)
                    printf("    0x%" PRIx64 "\n", addr_arr[j]);
            }
            return 0;
        }
        res = sg_ll_reassign_blocks(sg_fd, eight, longlist, param_arr,
                                    param_len, 1, verbose);
        ret = res;
        if (res) {
            sg_get_category_sense_str(res, sizeof(b), b, verbose);
            pr2serr("REASSIGN BLOCKS: %s\n", b);
            goto err_out;
        }
    } else /* if (grown || primary) */ {
        int dl_format = DEF_DEFECT_LIST_FORMAT;
        int div = 0;
        int dl_len, got_grown, got_primary;
        const char * lstp;

        param_len = 4;
        memset(param_arr, 0, param_len);
        res = sg_ll_read_defect10(sg_fd, primary, grown, dl_format,
                                  param_arr, param_len, 0, verbose);
        ret = res;
        if (res) {
            sg_get_category_sense_str(res, sizeof(b), b, verbose);
            pr2serr("READ DEFECT DATA(10): %s\n", b);
            goto err_out;
        }
        if (do_hex) {
            dStrHex((const char *)param_arr, param_len, 1);
            goto err_out;       /* ret is zero */
        }
        got_grown = !!(param_arr[1] & 0x8);
        got_primary = !!(param_arr[1] & 0x10);
        if (got_grown && got_primary)
            lstp = "grown and primary defect lists";
        else if (got_grown)
            lstp = "grown defect list";
        else if (got_primary)
            lstp = "primary defect list";
        else {
            pr2serr("didn't get grown or primary list in response\n");
            goto err_out;
        }
        if (verbose)
            pr2serr("asked for defect list format %d, got %d\n", dl_format,
                    (param_arr[1] & 0x7));
        dl_format = (param_arr[1] & 0x7);
        switch (dl_format) {
            case 0:     /* short block */
                div = 4;
                break;
            case 3:     /* long block */
            case 4:     /* bytes from index */
            case 5:     /* physical sector */
                div = 8;
                break;
            default:
                pr2serr("defect list format %d unknown\n", dl_format);
                break;
        }
        dl_len = sg_get_unaligned_be16(param_arr + 2);
        if (0 == dl_len)
            printf(">> Elements in %s: 0\n", lstp);
        else {
            if (0 == div)
                printf(">> %s length=%d bytes [unknown number of elements]\n",
                       lstp, dl_len);
            else
                printf(">> Elements in %s: %d\n", lstp,
                       dl_len / div);
        }
    }

err_out:
    res = sg_cmds_close_device(sg_fd);
    if (res < 0) {
        pr2serr("close error: %s\n", safe_strerror(-res));
        if (0 == ret)
            return SG_LIB_FILE_ERROR;
    }
    return (ret >= 0) ? ret : SG_LIB_CAT_OTHER;
}
